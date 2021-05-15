#ifndef _SYS_FILE_H_
#define _SYS_FILE_H_

#include <sys/cdefs.h>
#include <sys/fcntl.h>

#ifdef _KERNEL
#include <sys/mutex.h>
#include <sys/refcnt.h>

typedef struct file file_t;
typedef struct vnode vnode_t;
typedef struct stat stat_t;
typedef struct uio uio_t;
typedef struct proc proc_t;
typedef struct knote knote_t;

typedef int fo_read_t(file_t *f, uio_t *uio);
typedef int fo_write_t(file_t *f, uio_t *uio);
typedef int fo_close_t(file_t *f);
typedef int fo_seek_t(file_t *f, off_t offset, int whence, off_t *newoffp);
typedef int fo_stat_t(file_t *f, stat_t *sb);
typedef int fo_ioctl_t(file_t *f, u_long cmd, void *data);
typedef int fo_kqfilter_t(file_t *f, knote_t *kn);

typedef struct {
  fo_read_t *fo_read;
  fo_write_t *fo_write;
  fo_close_t *fo_close;
  fo_seek_t *fo_seek;
  fo_stat_t *fo_stat;
  fo_ioctl_t *fo_ioctl;
  fo_kqfilter_t *fo_kqfilter;
} fileops_t;

/* Put `nowrite` into `fo_write` if a file doesn't support writes. */
int nowrite(file_t *f, uio_t *uio);

/* Put `noseek` into `fo_seek` if a file is not seekable. */
int noseek(file_t *f, off_t offset, int whence, off_t *newoffp);

typedef enum filetype {
  FT_VNODE = 1,  /* regular file */
  FT_PIPE = 2,   /* pipe */
  FT_PTY = 3,    /* master side of a pseudoterminal */
  FT_KQUEUE = 4, /* kqueue */
} filetype_t;

#define FF_READ 1  /* file can be read from */
#define FF_WRITE 2 /* file can be written to */
#define FF_MASK (FF_READ | FF_WRITE)

#define IO_APPEND 4   /* file offset should be set to EOF prior to each write */
#define IO_NONBLOCK 8 /* read & write return EAGAIN instead of blocking */
#define IO_MASK (IO_APPEND | IO_NONBLOCK)

typedef struct file {
  void *f_data; /* File specific data */
  fileops_t *f_ops;
  filetype_t f_type; /* File type */
  vnode_t *f_vnode;
  off_t f_offset;
  refcnt_t f_count; /* Reference counter */
  unsigned f_flags; /* FF_* and IO_* flags */
} file_t;

file_t *file_alloc(void);
void file_destroy(file_t *f);

/*! \brief Increments reference counter. */
void file_hold(file_t *f);

/*! \brief Decrements refcounter and destroys file if it has reached 0. */
void file_drop(file_t *f);

/* File operations for files that lost identity. */
extern fileops_t badfileops;

/* Procedures called by system calls implementation. */
int do_close(proc_t *p, int fd);
int do_read(proc_t *p, int fd, uio_t *uio);
int do_write(proc_t *p, int fd, uio_t *uio);
int do_lseek(proc_t *p, int fd, off_t offset, int whence, off_t *newoffp);
int do_fstat(proc_t *p, int fd, stat_t *sb);
int do_dup(proc_t *p, int oldfd, int *newfdp);
int do_dup2(proc_t *p, int oldfd, int newfd);
int do_fcntl(proc_t *p, int fd, int cmd, int arg, int *resp);
int do_ioctl(proc_t *p, int fd, u_long cmd, void *data);
int do_umask(proc_t *p, int newmask, int *oldmaskp);

#endif /* !_KERNEL */

#endif /* !_SYS_FILE_H_ */
