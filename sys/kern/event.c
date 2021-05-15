#include <sys/event.h>
#include <sys/mimiker.h>
#include <sys/libkern.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/condvar.h>
#include <sys/errno.h>
#include <sys/malloc.h>
#include <sys/device.h>
#include <sys/proc.h>
#include <sys/pool.h>
#include <sys/time.h>

#define KN_HASHSIZE 8

static POOL_DEFINE(P_KNOTE, "knote", sizeof(knote_t));

static void knote_enqueue(knote_t *kn);

typedef TAILQ_HEAD(, knote) knote_tailq_t;

typedef struct kqueue {
  int kq_count;                    /* number of pending events */
  knote_tailq_t kq_head; /* list of pending events */
  mtx_t kq_lock;               /* mutex for queue access */
  condvar_t kq_cv;
  knlist_t kq_knhash[KN_HASHSIZE]; /* hash table for knotes */
} kqueue_t;

static inline knlist_t *kq_gethash(kqueue_t *kq, void *obj) {
  return &kq->kq_knhash[((unsigned long) obj) % KN_HASHSIZE];
}

static kqueue_t *kqueue_create(void) {
  kqueue_t *kq = kmalloc(M_DEV, sizeof(kqueue_t), M_ZERO);
  mtx_init(&kq->kq_lock, 0);
  cv_init(&kq->kq_cv, 0);

  for (int i = 0; i < KN_HASHSIZE; i++)
    SLIST_INIT(&kq->kq_knhash[i]);

  return kq;
}

static void kqueue_destroy(kqueue_t *kq) {
  mtx_destroy(&kq->kq_lock);
  cv_destroy(&kq->kq_cv);
  kfree(M_DEV, kq);
}

static int filt_fileattach(knote_t *kn) {
  file_t *fp = kn->kn_obj;
  return FOP_KQFILTER(fp, kn);
}

static filterops_t file_filtops = {
  .f_attach = filt_fileattach,
};

static filterops_t *sys_kfilters[EVFILT_SYSCOUNT] = {
  [EVFILT_READ] = &file_filtops,
  [EVFILT_WRITE] = &file_filtops,
};

static filterops_t *filt_getops(uint32_t filter) {
  if (filter > EVFILT_SYSCOUNT)
    return NULL;
  return sys_kfilters[filter];
}

static int kqueue_read(file_t *f, uio_t *uio) {
  return EOPNOTSUPP;
}

static int kqueue_write(file_t *f, uio_t *uio) {
  return EOPNOTSUPP;
}

static int kqueue_close(file_t *f) {
  kqueue_t *kq = f->f_data;
  kqueue_destroy(kq);
  f->f_data = NULL;

  return 0;
}

static int kqueue_stat(file_t *f, stat_t *sb) {
  return EOPNOTSUPP;
}

static int kqueue_seek(file_t *f, off_t offset, int whence, off_t *newoffp) {
  return EOPNOTSUPP;
}

static int kqueue_ioctl(file_t *f, u_long cmd, void *data) {
  return EOPNOTSUPP;
}

static fileops_t kqueueops = {.fo_read = kqueue_read,
                              .fo_write = kqueue_write,
                              .fo_close = kqueue_close,
                              .fo_stat = kqueue_stat,
                              .fo_seek = kqueue_seek,
                              .fo_ioctl = kqueue_ioctl};

static void knote_detach(knote_t *kn) {
  kqueue_t *kq = kn->kn_kq;
  knlist_t *knote_list = kq_gethash(kq, kn->kn_obj);
  SLIST_REMOVE(knote_list, kn, knote, kn_hashlink);

  WITH_MTX_LOCK(&kq->kq_lock) {
    if (kn->kn_status & KN_QUEUED) {
      kq->kq_count--;
      TAILQ_REMOVE(&kq->kq_head, kn, kn_penlink);
    }
  }

  pool_free(P_KNOTE, kn);
}

static int kqueue_register(kqueue_t *kq, kevent_t *kev, void *obj) {
  knote_t *kn = NULL;
  int error;

  filterops_t *filops = filt_getops(kev->filter);
  if (filops == NULL)
    return EINVAL;

  /* Find an existing knote to use for this kevent. */
  knlist_t *knote_list = kq_gethash(kq, obj);
  SLIST_FOREACH(kn, knote_list, kn_hashlink) {
    if (kev->filter == kn->kn_kevent.filter && kn->kn_obj == obj)
      break;
  }

  if (kn == NULL) {
    if ((kev->flags & EV_ADD) == 0)
      return ENOENT;

    kn = pool_alloc(P_KNOTE, M_ZERO);
    kn->kn_kq = kq;
    kn->kn_kevent = *kev;
    kn->kn_obj = obj;
    kn->kn_fop = filops;
    
    SLIST_INSERT_HEAD(knote_list, kn, kn_hashlink);

    if ((error = kn->kn_fop->f_attach(kn)) != 0) {
    
    }

    if (kn->kn_fop->f_event(kn, 0)) {
      knote_enqueue(kn);
    }
  }

  if (kev->flags & EV_DELETE) {
    knote_detach(kn);
    return 0;
  }

  return 0;
}

static int kqueue_scan(kqueue_t *kq, kevent_t *eventlist, size_t nevents,
                       timespec_t *tsp, int *retval) {
  int error;
  size_t count = 0;
  systime_t timeout, sleepts;
  knote_tailq_t knqueue;
  knote_t *kn;

  TAILQ_INIT(&knqueue);

  if (tsp) {
    timeout = ts2hz(tsp);
    sleepts = getsystime() + timeout;
    if (timeout <= 0)
      timeout = -1; /* do poll */
  } else {
    timeout = 0; /* no timeout, wait forever */
  }

  mtx_lock(&kq->kq_lock);
  while (kq->kq_count == 0) {
    if (timeout < 0) {
      mtx_unlock(&kq->kq_lock);
      return 0;
    }
    
    error = cv_wait_timed(&kq->kq_cv, &kq->kq_lock, timeout);
    if (error == EINTR) {
      mtx_unlock(&kq->kq_lock);
      return EINTR;
    }

    if (tsp)
      timeout = sleepts - getsystime();
  }

  while (count < nevents) {
    assert(mtx_owned(&kq->kq_lock));

    if (TAILQ_EMPTY(&kq->kq_head))
      break;

    kn = TAILQ_FIRST(&kq->kq_head);
    TAILQ_REMOVE(&kq->kq_head, kn, kn_penlink);
    kn->kn_status |= KN_BUSY;

    mtx_unlock(&kq->kq_lock);
    mtx_lock(&kn->kn_objlock);
    error = kn->kn_fop->f_event(kn, 0);
    mtx_lock(&kq->kq_lock);
    mtx_unlock(&kq->kq_lock);
    kn->kn_status &= ~KN_BUSY;

    if (error == 0) {
      kn->kn_status &= ~KN_QUEUED;
      kq->kq_count--;
      continue;
    }

    TAILQ_INSERT_HEAD(&knqueue, kn, kn_penlink);

    eventlist[count] = kn->kn_kevent;
    count++;
  }

  TAILQ_CONCAT(&kq->kq_head, &knqueue, kn_penlink);

  mtx_unlock(&kq->kq_lock);

  return 0;
}

static int kqueue_get_obj(proc_t *p, kevent_t *kev, void **obj) {
  if (kev->filter == EVFILT_READ || kev->filter == EVFILT_WRITE)
    return fdtab_get_file(p->p_fdtable, kev->ident, FF_READ, (file_t **) obj);

  return EINVAL;
}

static int kevent(proc_t *p, kqueue_t *kq, kevent_t *changelist, size_t nchanges,
           kevent_t *eventlist, size_t nevents, timespec_t *timeout,
           int *retval) {
  int error;
  size_t nerrors = 0;
  kevent_t *kev;
  void *obj;

  for (size_t i = 0; i < nchanges; i++) {
    kev = &changelist[i];

    error = kqueue_get_obj(p, kev, &obj);
    
    if (error == 0 && (error = kqueue_register(kq, kev, obj)) == 0)
      continue;

    /* No more space left in the the eventlist, so simply return the error. */
    if (nevents == nerrors)
      return error;

    kev->flags = EV_ERROR;
    kev->data = error;
    memcpy(&eventlist[nerrors], kev, sizeof(kevent_t));
    nerrors++;
  }

  if (nerrors) {
    *retval = nerrors;
    return 0;
  }

  return kqueue_scan(kq, eventlist, nevents, timeout, retval);
}

int do_kqueue1(proc_t *p, int flags, int *fd) {
  int error;
  kqueue_t *kq = kqueue_create();

  file_t *file = file_alloc();
  file->f_data = kq;
  file->f_ops = &kqueueops;
  file->f_type = FT_KQUEUE;
  file->f_flags = 0;

  if (!(error = fdtab_install_file(p->p_fdtable, file, 0, fd))) {
    if (!(error = fd_set_cloexec(p->p_fdtable, *fd, (flags & O_CLOEXEC) != 0)))
      return 0;

    fdtab_close_fd(p->p_fdtable, *fd);
  }

  kqueue_close(file);
  return error;
}

int do_kevent(proc_t *p, int kq, kevent_t *changelist, size_t nchanges,
              kevent_t *eventlist, size_t nevents, timespec_t *timeout,
              int *retval) {
  file_t *f;
  int error;

  if ((error = fdtab_get_file(p->p_fdtable, kq, 0, &f)))
    return error;

  if (f->f_type != FT_KQUEUE) {
    file_drop(f);
    return EBADF;
  }

  error = kevent(p, f->f_data, changelist, nchanges, eventlist, nevents, timeout,
                 retval);
  file_drop(f);

  return error;
}

/*
 * Queue new event for knote.
 */
static void knote_enqueue(knote_t *kn) {
	kqueue_t *kq = kn->kn_kq;
  assert(mtx_owned(&kq->kq_lock));

  if (kn->kn_status & KN_QUEUED)
    return;

  TAILQ_INSERT_TAIL(&kq->kq_head, kn, kn_penlink);
  kq->kq_count++;
  kn->kn_status |= KN_QUEUED;

  cv_broadcast(&kq->kq_cv);
}

/*
 * Walk down a list of knotes, activating them if their event has
 * triggered.  The caller's object lock (e.g. device driver lock)
 * must be held.
 */
void knote(knlist_t *list, long hint) {
	knote_t *kn, *tmpkn;

	SLIST_FOREACH_SAFE(kn, list, kn_objlink, tmpkn) {
    WITH_MTX_LOCK(&kq->kq_lock) {
      if ((*kn->kn_fop->f_event)(kn, hint))
        knote_enqueue(kn);
    }
	}
}