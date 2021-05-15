/* Based on NetBSD's sys/sys/event.h */

#ifndef _SYS_EVENT_H_
#define _SYS_EVENT_H_

#include <sys/types.h>

#define EVFILT_READ 0U
#define EVFILT_WRITE 1U
#define EVFILT_SYSCOUNT 2U /* number of filters */

struct kevent {
  uintptr_t ident; /* identifier for this event */
  uint32_t filter; /* filter for event */
  uint32_t flags;  /* action flags for kqueue */
  uint32_t fflags; /* filter flag value */
  int64_t data;    /* filter data value */
  void *udata;     /* opaque user data identifier */
};

#define EV_SET(kevp_, a, b, c, d, e, f)                                        \
  *(kevp_) = (struct kevent){                                                  \
    .ident = (a),                                                              \
    .filter = (b),                                                             \
    .flags = (c),                                                              \
    .fflags = (d),                                                             \
    .data = (e),                                                               \
    .udata = (f),                                                              \
  };

/* actions */
#define EV_ADD 0x0001U     /* add event to kq (implies ENABLE) */
#define EV_DELETE 0x0002U  /* delete event from kq */

/* flags */
#define EV_ONESHOT 0x0010U  /* only report one occurrence */
#define EV_CLEAR 0x0020U    /* clear event state after reporting */
#define EV_RECEIPT 0x0040U  /* force EV_ERROR on success, data=0 */
#define EV_DISPATCH 0x0080U /* disable event after reporting */

#define EV_SYSFLAGS 0xF000U /* reserved by system */
#define EV_FLAG1 0x2000U    /* filter-specific flag */

/* returned values */
#define EV_EOF 0x8000U   /* EOF detected */
#define EV_ERROR 0x4000U /* error, data contains errno */

#ifdef _KERNEL

#include <sys/queue.h>
#include <sys/time.h>

typedef struct knote knote_t;
typedef struct kevent kevent_t;
typedef struct kqueue kqueue_t;
typedef struct proc proc_t;

/*
 * Flag indicating hint is a signal.  Used by EVFILT_SIGNAL, and also
 * shared by EVFILT_PROC  (all knotes attached to p->p_klist)
 */
#define NOTE_SIGNAL 0x08000000U

/*
 * Hint values for the optional f_touch event filter.  If f_touch is not set
 * to NULL and f_isfd is zero the f_touch filter will be called with the type
 * argument set to EVENT_REGISTER during a kevent() system call.  It is also
 * called under the same conditions with the type argument set to EVENT_PROCESS
 * when the event has been triggered.
 */
#define EVENT_REGISTER 1
#define EVENT_PROCESS 2

/*
 * Callback methods for each filter type.
 */
typedef struct filterops {
  int (*f_attach)(knote_t *);
  /* called when knote is ADDed */
  void (*f_detach)(knote_t *);
  /* called when knote is DELETEd */
  int (*f_event)(knote_t *, long);
  /* called when event is triggered */
} filterops_t;

#define KN_QUEUED 0x01U   /* event is on queue */
#define KN_BUSY 0x02U     /* is being scanned */

typedef SLIST_HEAD(, knote) knlist_t;

/*
 * Field locking:
 *
 * f	kn_kq->kq_fdp->fd_lock
 * q	kn_kq->kq_lock
 * o	object mutex (e.g. device driver or vnode interlock)
 */

typedef struct knote {
  SLIST_ENTRY(knote) kn_hashlink; /* f: for hashmap in kqueue */
  SLIST_ENTRY(knote) kn_objlink; /* o: for list of knotes in the object*/
  TAILQ_ENTRY(knote) kn_penlink;  /* q: for list of pending events in kqueue */
  kqueue_t *kn_kq;            /* q: which queue we are on */
  kevent_t kn_kevent;
  uint32_t kn_status;  /* q: flags below */
  void *kn_obj;        /*    monitored obj */
  void *kn_hook;
  filterops_t *kn_fop;
  mtx_t *kn_objlock;
} knote_t;

int do_kqueue1(proc_t *p, int flags, int *fd);
int do_kevent(proc_t *p, int kq, kevent_t *changelist, size_t nchanges,
              kevent_t *eventlist, size_t nevents, timespec_t *timeout,
              int *retval);

void knote(knlist_t *knlist, long hint);

#else /* !_KERNEL */

struct timespec;

__BEGIN_DECLS
int kqueue(void);
int kqueue1(int);
int kevent(int, const struct kevent *, size_t, struct kevent *, size_t,
           const struct timespec *);
__END_DECLS

#endif /* !_KERNEL */

#endif /* !_SYS_EVENT_H_ */
