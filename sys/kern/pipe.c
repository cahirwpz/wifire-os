#define KL_LOG KL_FILE
#include <sys/klog.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/kmem.h>
#include <sys/pool.h>
#include <sys/errno.h>
#include <sys/pipe.h>
#include <sys/libkern.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/proc.h>
#include <sys/ringbuf.h>
#include <sys/uio.h>

typedef struct pipe_end pipe_end_t;
typedef struct pipe pipe_t;

struct pipe_end {
  mtx_t mtx;          /*!< protects pipe end internals */
  pipe_t *pipe;       /*!< pointer back to pipe structure */
  bool closed;        /*!< true if this end is closed */
  condvar_t nonempty; /*!< used to wait data to appear in the buffer */
  condvar_t nonfull;  /*!< used to wait for free space in the buffer */
  ringbuf_t buf;      /*!< buffer belongs to writer end */
  pipe_end_t *other;  /*!< the other end of the pipe */
};

struct pipe {
  mtx_t mtx;         /*!< protects reference counter */
  int refcnt;        /*!< pipe can be freed when reaches zero */
  pipe_end_t end[2]; /*!< both pipe ends */
};

static POOL_DEFINE(P_PIPE, "pipe", sizeof(pipe_t));

static void pipe_end_setup(pipe_end_t *end, pipe_end_t *other) {
  mtx_init(&end->mtx, 0);
  cv_init(&end->nonempty, "pipe_end_empty");
  cv_init(&end->nonfull, "pipe_end_full");
  end->buf.data = kmem_alloc(PIPE_SIZE, M_ZERO);
  end->buf.size = PIPE_SIZE;
  end->other = other;
}

static pipe_t *pipe_alloc(void) {
  pipe_t *pipe = pool_alloc(P_PIPE, M_ZERO);
  mtx_init(&pipe->mtx, 0);
  pipe_end_t *end0 = &pipe->end[0];
  pipe_end_t *end1 = &pipe->end[1];
  pipe_end_setup(end0, end1);
  pipe_end_setup(end1, end0);
  end0->pipe = pipe;
  end1->pipe = pipe;
  pipe->refcnt = 2;
  return pipe;
}

static void pipe_free(pipe_t *pipe) {
  int refcnt = 0;

  WITH_MTX_LOCK (&pipe->mtx)
    refcnt = --pipe->refcnt;

  if (refcnt == 0) {
    kmem_free(pipe->end[0].buf.data, PIPE_SIZE);
    kmem_free(pipe->end[1].buf.data, PIPE_SIZE);
    pool_free(P_PIPE, pipe);
  }
}

static int pipe_read(file_t *f, uio_t *uio) {
  pipe_end_t *consumer = f->f_data;
  pipe_end_t *producer = consumer->other;

  assert(!consumer->closed);

  /* user requested read of 0 bytes */
  if (uio->uio_resid == 0)
    return 0;

  /* no read atomicity for now! */
  WITH_MTX_LOCK (&producer->mtx) {
    /* pipe empty, no producers, return end-of-file */
    if (ringbuf_empty(&producer->buf) && producer->closed)
      return 0;

    if (f->f_flags & IO_NONBLOCK)
      return EAGAIN;
    /* pipe empty, producer exists, wait for data */
    while (ringbuf_empty(&producer->buf) && !producer->closed) {
      /* restart the syscall if we were interrupted by a signal */
      if (cv_wait_intr(&producer->nonempty, &producer->mtx))
        return ERESTARTSYS;
    }

    int res = ringbuf_read(&producer->buf, uio);
    if (res)
      return res;
    /* notify producer that free space is available */
    cv_broadcast(&producer->nonfull);
  }

  return 0;
}

static int pipe_write(file_t *f, uio_t *uio) {
  pipe_end_t *producer = f->f_data;
  pipe_end_t *consumer = producer->other;
  int res;

  assert(!producer->closed);

  /* Reading end is closed, no use in sending data there. */
  if (consumer->closed)
    return EPIPE;

  size_t old_resid = uio->uio_resid;

  /* no write atomicity for now! */
  WITH_MTX_LOCK (&producer->mtx) {
    do {
      res = ringbuf_write(&producer->buf, uio);
      if (res)
        break;
      /* notify consumer that new data is available */
      cv_broadcast(&producer->nonempty);
      /* nothing left to write? */
      if (uio->uio_resid == 0)
        return 0;
      /* buffer is full, so if we write in NONBLOCK then return with error */
      if (f->f_flags & IO_NONBLOCK)
        return EAGAIN;
      /* buffer is full, interruptly sleep while some data is consumed */
      if (cv_wait_intr(&producer->nonempty, &producer->mtx)) {
        res = ERESTARTSYS;
        break;
      }
    } while (!consumer->closed);
  }

  /* don't report errors on partial writes */
  if (uio->uio_resid < old_resid)
    res = 0;

  return res;
}

static int pipe_close(file_t *f) {
  pipe_end_t *end = f->f_data;

  WITH_MTX_LOCK (&end->mtx) {
    end->closed = true;
    /* Wake up consumers to let them finish their work! */
    cv_broadcast(&end->nonempty);
  }

  pipe_free(end->pipe);
  return 0;
}

static int pipe_stat(file_t *f, stat_t *sb) {
  return EOPNOTSUPP;
}

static int pipe_ioctl(file_t *f, u_long cmd, void *data) {
  return EOPNOTSUPP;
}

static fileops_t pipeops = {
  .fo_read = pipe_read,
  .fo_write = pipe_write,
  .fo_close = pipe_close,
  .fo_seek = noseek,
  .fo_stat = pipe_stat,
  .fo_ioctl = pipe_ioctl,
};

static file_t *make_pipe_file(pipe_end_t *end) {
  file_t *file = file_alloc();
  file->f_data = end;
  file->f_ops = &pipeops;
  file->f_type = FT_PIPE;
  file->f_flags = FF_READ | FF_WRITE;
  return file;
}

int do_pipe2(proc_t *p, int fds[2], int flags) {
  pipe_t *pipe = pipe_alloc();
  pipe_end_t *consumer = &pipe->end[0];
  pipe_end_t *producer = &pipe->end[1];

  file_t *file0 = make_pipe_file(consumer);
  file_t *file1 = make_pipe_file(producer);

  if (flags & O_NONBLOCK) {
    file0->f_flags |= IO_NONBLOCK;
    file1->f_flags |= IO_NONBLOCK;
  }

  int set_cloexec = flags & O_CLOEXEC;
  int error;

  if (!(error = fdtab_install_file(p->p_fdtable, file0, 0, &fds[0]))) {
    if (!(error = fdtab_install_file(p->p_fdtable, file1, 0, &fds[1]))) {
      if (!(error = fd_set_cloexec(p->p_fdtable, fds[0], set_cloexec)))
        return fd_set_cloexec(p->p_fdtable, fds[1], set_cloexec);
      fdtab_close_fd(p->p_fdtable, fds[1]);
    }
    fdtab_close_fd(p->p_fdtable, fds[0]);
  }
  pipe_close(file0);
  pipe_close(file1);
  return error;
}
