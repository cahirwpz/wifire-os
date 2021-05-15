#define KL_LOG KL_SYSCALL
#include <sys/klog.h>
#include <sys/sysent.h>
#include <sys/mimiker.h>
#include <sys/errno.h>
#include <sys/vm.h>
#include <sys/vfs.h>
#include <sys/uio.h>
#include <sys/file.h>
#include <sys/sbrk.h>
#include <sys/signal.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/exec.h>
#include <sys/time.h>
#include <sys/ioccom.h>
#include <sys/pipe.h>
#include <sys/malloc.h>
#include <sys/libkern.h>
#include <sys/syslimits.h>
#include <sys/context.h>
#include <sys/thread.h>
#include <sys/cred.h>
#include <sys/statvfs.h>
#include <sys/pty.h>
#include <sys/event.h>

#include "sysent.h"

/* Empty syscall handler, for unimplemented and deprecated syscall numbers. */
int sys_syscall(proc_t *p, syscall_args_t *args, register_t *res) {
  klog("unimplemented system call %ld", SCARG(args, number));
  return ENOSYS;
};

static int sys_sbrk(proc_t *p, sbrk_args_t *args, register_t *res) {
  klog("sbrk(%d)", SCARG(args, increment));

  return sbrk_resize(p, SCARG(args, increment), (vaddr_t *)res);
}

/* https://pubs.opengroup.org/onlinepubs/9699919799/functions/exit.html */
static int sys_exit(proc_t *p, exit_args_t *args, register_t *res) {
  klog("exit(%d)", SCARG(args, rval));
  proc_lock(p);
  proc_exit(MAKE_STATUS_EXIT(SCARG(args, rval)));
  __unreachable();
}

/* https://pubs.opengroup.org/onlinepubs/9699919799/functions/fork.html */
static int sys_fork(proc_t *p, void *args, register_t *res) {
  int error;
  pid_t pid;

  klog("fork()");

  if ((error = do_fork(NULL, NULL, &pid)))
    return error;

  *res = pid;
  return 0;
}

/* https://pubs.opengroup.org/onlinepubs/9699919799/functions/getpid.html */
static int sys_getpid(proc_t *p, void *args, register_t *res) {
  klog("getpid()");
  *res = p->p_pid;
  return 0;
}

/* https://pubs.opengroup.org/onlinepubs/9699919799/functions/getppid.html */
static int sys_getppid(proc_t *p, void *args, register_t *res) {
  klog("getppid()");
  assert(p->p_parent);
  *res = p->p_parent->p_pid;
  return 0;
}

/* Moves the process to the process group with ID specified by pgid.
 * If pid is zero, then the PID of the calling process is used.
 * If pgid is zero, then the process is moved to process group
 * with ID equal to the PID of the process.
 *
 * https://pubs.opengroup.org/onlinepubs/9699919799/functions/setpgid.html */
static int sys_setpgid(proc_t *p, setpgid_args_t *args, register_t *res) {
  pid_t pid = SCARG(args, pid);
  pgid_t pgid = SCARG(args, pgid);

  klog("setpgid(%d, %d)", pid, pgid);

  if (pgid < 0)
    return EINVAL;

  if (pid == 0)
    pid = p->p_pid;
  if (pgid == 0)
    pgid = pid;

  return pgrp_enter(p, pid, pgid);
}

/* Gets process group ID of the process with ID specified by pid.
 * If pid equals zero then process ID of the calling process is used.
 *
 * https://pubs.opengroup.org/onlinepubs/9699919799/functions/getpgid.html */
static int sys_getpgid(proc_t *p, getpgid_args_t *args, register_t *res) {
  pid_t pid = SCARG(args, pid);
  pgid_t pgid;

  if (pid < 0)
    return EINVAL;

  if (pid == 0)
    pid = p->p_pid;

  int error = proc_getpgid(pid, &pgid);
  *res = pgid;
  return error;
}

/* https://pubs.opengroup.org/onlinepubs/9699919799/functions/kill.html */
static int sys_kill(proc_t *p, kill_args_t *args, register_t *res) {
  klog("kill(%lu, %d)", SCARG(args, pid), SCARG(args, sig));
  return proc_sendsig(SCARG(args, pid), SCARG(args, sig));
}

/* Set and get the file mode creation mask.
 *
 * https://pubs.opengroup.org/onlinepubs/9699919799/functions/umask.html */
static int sys_umask(proc_t *p, umask_args_t *args, register_t *res) {
  mode_t newmask = SCARG(args, newmask);
  klog("umask(%x)", SCARG(args, newmask));
  return do_umask(p, newmask, (int *)res);
}

/* https://pubs.opengroup.org/onlinepubs/9699919799/functions/sigaction.html */
static int sys_sigaction(proc_t *p, sigaction_args_t *args, register_t *res) {
  int signo = SCARG(args, signum);
  const void *u_newact = SCARG(args, nsa);
  void *u_oldact = SCARG(args, osa);

  klog("sigaction(%d, %p, %p)", signo, u_newact, u_oldact);

  sigaction_t newact;
  sigaction_t oldact;
  int error;

  if (u_newact && (error = copyin_s(u_newact, newact)))
    return error;

  if ((error = do_sigaction(signo, u_newact ? &newact : NULL, &oldact)))
    return error;

  if (u_oldact != NULL)
    error = copyout_s(oldact, u_oldact);

  return error;
}

static int sys_sigreturn(proc_t *p, sigreturn_args_t *args, register_t *res) {
  ucontext_t *ucp = SCARG(args, sigctx_p);
  klog("sigreturn(%p)", ucp);
  return do_sigreturn(ucp);
}

static int sys_mmap(proc_t *p, mmap_args_t *args, register_t *res) {
  vaddr_t va = (vaddr_t)SCARG(args, addr);
  size_t length = SCARG(args, len);
  vm_prot_t prot = SCARG(args, prot);
  int flags = SCARG(args, flags);

  klog("mmap(%p, %u, %d, %d)", (void *)va, length, prot, flags);

  int error;
  if ((error = do_mmap(&va, length, prot, flags)))
    return error;

  *res = va;
  return 0;
}

static int sys_munmap(proc_t *p, munmap_args_t *args, register_t *res) {
  klog("munmap(%p, %u)", SCARG(args, addr), SCARG(args, len));
  return do_munmap((vaddr_t)SCARG(args, addr), SCARG(args, len));
}

/* TODO: implement it !!! */
static int sys_mprotect(proc_t *p, mprotect_args_t *args, register_t *res) {
  klog("mprotect(%p, %u, %u)", SCARG(args, addr), SCARG(args, len),
       SCARG(args, prot));
  return ENOTSUP;
}

static int sys_openat(proc_t *p, openat_args_t *args, register_t *res) {
  int fdat = SCARG(args, fd);
  const char *u_path = SCARG(args, path);
  int flags = SCARG(args, flags);
  mode_t mode = SCARG(args, mode);

  char *path = kmalloc(M_TEMP, PATH_MAX, 0);
  size_t n = 0;
  int fd, error;

  /* Copyout path. */
  if ((error = copyinstr(u_path, path, PATH_MAX, &n)))
    goto end;

  klog("openat(%d, \"%s\", %d, %d)", fdat, path, flags, mode);

  if ((error = do_openat(p, fdat, path, flags, mode, &fd)))
    goto end;

  *res = fd;

end:
  kfree(M_TEMP, path);
  return error;
}

static int sys_close(proc_t *p, close_args_t *args, register_t *res) {
  klog("close(%d)", SCARG(args, fd));

  return do_close(p, SCARG(args, fd));
}

static int sys_read(proc_t *p, read_args_t *args, register_t *res) {
  int fd = SCARG(args, fd);
  void *u_buf = SCARG(args, buf);
  size_t nbyte = SCARG(args, nbyte);
  int error;

  klog("read(%d, %p, %u)", fd, u_buf, nbyte);

  uio_t uio = UIO_SINGLE_USER(UIO_READ, 0, u_buf, nbyte);
  if ((error = do_read(p, fd, &uio)))
    return error;

  *res = nbyte - uio.uio_resid;
  return 0;
}

static int sys_write(proc_t *p, write_args_t *args, register_t *res) {
  int fd = SCARG(args, fd);
  const char *u_buf = SCARG(args, buf);
  size_t nbyte = SCARG(args, nbyte);
  int error;

  klog("write(%d, %p, %u)", fd, u_buf, nbyte);

  uio_t uio = UIO_SINGLE_USER(UIO_WRITE, 0, u_buf, nbyte);
  if ((error = do_write(p, fd, &uio)))
    return error;

  *res = nbyte - uio.uio_resid;
  return 0;
}

static int sys_lseek(proc_t *p, lseek_args_t *args, register_t *res) {
  off_t newoff;
  int error;

  klog("lseek(%d, %ld, %d)", SCARG(args, fd), SCARG(args, offset),
       SCARG(args, whence));
  if ((error = do_lseek(p, SCARG(args, fd), SCARG(args, offset),
                        SCARG(args, whence), &newoff)))
    return error;

  *res = newoff;
  return 0;
}

static int sys_fstat(proc_t *p, fstat_args_t *args, register_t *res) {
  int fd = SCARG(args, fd);
  stat_t *u_sb = SCARG(args, sb);
  stat_t sb;
  int error;

  klog("fstat(%d, %p)", fd, u_sb);

  if ((error = do_fstat(p, fd, &sb)))
    return error;
  return copyout_s(sb, u_sb);
}

static int sys_chdir(proc_t *p, chdir_args_t *args, register_t *res) {
  const char *u_path = SCARG(args, path);
  char *path = kmalloc(M_TEMP, PATH_MAX, 0);
  size_t len = 0;
  int error = 0;

  if ((error = copyinstr(u_path, path, PATH_MAX, &len)))
    goto end;

  error = do_chdir(p, path);

end:
  kfree(M_TEMP, path);
  return error;
}

static int sys_fchdir(proc_t *p, fchdir_args_t *args, register_t *res) {
  klog("fchdir(%d)", SCARG(args, fd));
  return do_fchdir(p, SCARG(args, fd));
}

static int sys_getcwd(proc_t *p, getcwd_args_t *args, register_t *res) {
  char *u_buf = SCARG(args, buf);
  size_t len = SCARG(args, len);
  int error;

  if (len == 0)
    return EINVAL;

  char *path = kmalloc(M_TEMP, PATH_MAX, 0);
  size_t last = PATH_MAX;

  /* We're going to construct the path backwards! */
  if ((error = do_getcwd(p, path, &last)))
    goto end;

  size_t used = PATH_MAX - last;
  if (used > len) {
    error = ERANGE;
    goto end;
  }

  error = copyout(&path[last], u_buf, used);

end:
  kfree(M_TEMP, path);
  return error;
}

static int sys_mount(proc_t *p, mount_args_t *args, register_t *res) {
  const char *u_type = SCARG(args, type);
  const char *u_path = SCARG(args, path);

  char *type = kmalloc(M_TEMP, PATH_MAX, 0);
  char *path = kmalloc(M_TEMP, PATH_MAX, 0);
  size_t n = 0;
  int error;

  /* Copyout fsysname. */
  if ((error = copyinstr(u_type, type, PATH_MAX, &n)))
    goto end;
  n = 0;
  /* Copyout pathname. */
  if ((error = copyinstr(u_path, path, PATH_MAX, &n)))
    goto end;

  klog("mount(\"%s\", \"%s\")", path, type);

  error = do_mount(p, type, path);
end:
  kfree(M_TEMP, type);
  kfree(M_TEMP, path);
  return error;
}

static int sys_getdents(proc_t *p, getdents_args_t *args, register_t *res) {
  int fd = SCARG(args, fd);
  void *u_buf = SCARG(args, buf);
  size_t len = SCARG(args, len);
  int error;

  klog("getdents(%d, %p, %u)", fd, u_buf, len);

  uio_t uio = UIO_SINGLE_USER(UIO_READ, 0, u_buf, len);
  if ((error = do_getdents(p, fd, &uio)))
    return error;

  *res = len - uio.uio_resid;
  return 0;
}

static int sys_dup(proc_t *p, dup_args_t *args, register_t *res) {
  int error, fd;
  klog("dup(%d)", SCARG(args, fd));
  error = do_dup(p, SCARG(args, fd), &fd);
  *res = fd;
  return error;
}

static int sys_dup2(proc_t *p, dup2_args_t *args, register_t *res) {
  klog("dup2(%d, %d)", SCARG(args, from), SCARG(args, to));

  *res = SCARG(args, to);

  return do_dup2(p, SCARG(args, from), SCARG(args, to));
}

static int sys_fcntl(proc_t *p, fcntl_args_t *args, register_t *res) {
  int error, value = 0;
  klog("fcntl(%d, %d, %ld)", SCARG(args, fd), SCARG(args, cmd),
       (long)SCARG(args, arg));
  error = do_fcntl(p, SCARG(args, fd), SCARG(args, cmd), (long)SCARG(args, arg),
                   &value);
  *res = value;
  return error;
}

static int sys_wait4(proc_t *p, wait4_args_t *args, register_t *res) {
  pid_t pid = SCARG(args, pid);
  int *u_status = SCARG(args, status);
  int options = SCARG(args, options);
  struct rusage *u_rusage = SCARG(args, rusage);
  int status = 0;
  int error;

  klog("wait4(%d, %x, %d, %p)", pid, u_status, options, u_rusage);

  if (u_rusage)
    klog("sys_wait4: acquiring rusage not implemented!");

  pid_t cld;
  if ((error = do_waitpid(pid, &status, options, &cld)))
    return error;

  if (u_status != NULL)
    if ((error = copyout_s(status, u_status)))
      return error;

  *res = cld;
  return 0;
}

static int sys_pipe2(proc_t *p, pipe2_args_t *args, register_t *res) {
  int *u_fdp = SCARG(args, fdp);
  int flags = SCARG(args, flags);
  int fds[2];
  int error;

  klog("pipe2(%x, %d)", u_fdp, flags);

  if (flags)
    klog("sys_pipe2: non-zero flags not handled!");

  if ((error = do_pipe(p, fds)))
    return error;

  return copyout(fds, u_fdp, 2 * sizeof(int));
}

static int sys_unlinkat(proc_t *p, unlinkat_args_t *args, register_t *res) {
  int fd = SCARG(args, fd);
  const char *u_path = SCARG(args, path);
  int flag = SCARG(args, flag);

  char *path = kmalloc(M_TEMP, PATH_MAX, 0);
  size_t n = 0;
  int error;

  /* Copyout pathname. */
  if ((error = copyinstr(u_path, path, PATH_MAX, &n)))
    goto end;

  klog("unlinkat(%d, \"%s\", %d)", fd, path, flag);

  error = do_unlinkat(p, fd, path, flag);

end:
  kfree(M_TEMP, path);
  return error;
}

static int sys_mkdirat(proc_t *p, mkdirat_args_t *args, register_t *res) {
  int fd = SCARG(args, fd);
  const char *u_path = SCARG(args, path);
  mode_t mode = SCARG(args, mode);

  char *path = kmalloc(M_TEMP, PATH_MAX, 0);
  size_t n = 0;
  int error;

  if ((error = copyinstr(u_path, path, PATH_MAX, &n)))
    goto end;

  klog("mkdirat(%d, %s, %d)", fd, u_path, mode);

  error = do_mkdirat(p, fd, path, mode);

end:
  kfree(M_TEMP, path);
  return error;
}

static int sys_execve(proc_t *p, execve_args_t *args, register_t *res) {
  const char *u_path = SCARG(args, path);
  char *const *u_argp = SCARG(args, argp);
  char *const *u_envp = SCARG(args, envp);

  /* do_execve handles copying data from user-space */
  return do_execve(u_path, u_argp, u_envp);
}

static int sys_faccessat(proc_t *p, faccessat_args_t *args, register_t *res) {
  int fd = SCARG(args, fd);
  const char *u_path = SCARG(args, path);
  mode_t mode = SCARG(args, mode);
  int flags = SCARG(args, flags);

  char *path = kmalloc(M_TEMP, PATH_MAX, 0);
  int error;

  if ((error = copyinstr(u_path, path, PATH_MAX, NULL)))
    goto end;

  klog("faccessat(%d, \"%s\", %d, %d)", fd, path, mode, flags);

  error = do_faccessat(p, fd, path, mode, flags);

end:
  kfree(M_TEMP, path);
  return error;
}

static int sys_clock_gettime(proc_t *p, clock_gettime_args_t *args,
                             register_t *res) {
  clockid_t clock_id = SCARG(args, clock_id);
  timespec_t *u_ts = SCARG(args, tsp);
  timespec_t ts;
  int error;

  if ((error = do_clock_gettime(clock_id, &ts)))
    return error;

  return copyout_s(ts, u_ts);
}

static int sys_clock_nanosleep(proc_t *p, clock_nanosleep_args_t *args,
                               register_t *res) {
  clockid_t clock_id = SCARG(args, clock_id);
  int flags = SCARG(args, flags);
  /* u_ - user, rm - remaining, rq - requested, t - time, p - pointer */
  const timespec_t *u_rqtp = SCARG(args, rqtp);
  timespec_t *u_rmtp = SCARG(args, rmtp);
  timespec_t rqtp, rmtp;
  int error, copy_err;

  if ((error = copyin_s(u_rqtp, rqtp)))
    return error;

  error = do_clock_nanosleep(clock_id, flags, &rqtp, u_rmtp ? &rmtp : NULL);

  if (u_rmtp == NULL || (error != 0 && error != EINTR))
    return error;

  /*  TIMER_ABSTIME - sleep to an absolute deadline */
  if ((flags & TIMER_ABSTIME) == 0 && (copy_err = copyout_s(rmtp, u_rmtp)))
    return copy_err;

  return error;
}

static int sys_sigaltstack(proc_t *p, sigaltstack_args_t *args,
                           register_t *res) {
  const stack_t *ss = SCARG(args, ss);
  stack_t *old_ss = SCARG(args, old_ss);
  int error;

  klog("sigaltstack(%p, %p)", ss, old_ss);

  /* TODO: Setting alternate signal stack is not implemented yet. */
  assert(ss == NULL);

  if (old_ss != NULL) {
    /* TODO: Currently we don't support alternate signal stack in the kernel,
       so this syscall always returns SS_DISABLE. */
    stack_t result = {.ss_sp = 0, .ss_size = 0, .ss_flags = SS_DISABLE};
    error = copyout_s(result, old_ss);
    if (error)
      return error;
  }

  return 0;
}

static int sys_sigprocmask(proc_t *p, sigprocmask_args_t *args,
                           register_t *res) {
  int how = SCARG(args, how);
  const sigset_t *u_set = SCARG(args, set);
  sigset_t *u_oset = SCARG(args, oset);
  sigset_t set, oset;
  int error;

  klog("sigprocmask(%d, %p, %p)", how, u_set, u_oset);

  if (u_set && (error = copyin_s(u_set, set)))
    return error;

  proc_lock(p);
  error = do_sigprocmask(how, u_set ? &set : NULL, &oset);
  proc_unlock(p);

  if (error)
    return error;

  if (u_oset)
    error = copyout_s(oset, u_oset);

  return error;
}

static int sys_sigsuspend(proc_t *p, sigsuspend_args_t *args, register_t *res) {
  const sigset_t *umask = SCARG(args, sigmask);
  sigset_t mask;
  int error;

  klog("sigsuspend(%p)", umask);

  if ((error = copyin_s(umask, mask)))
    return error;

  return do_sigsuspend(p, &mask);
}

static int sys_setcontext(proc_t *p, setcontext_args_t *args, register_t *res) {
  const ucontext_t *ucp = SCARG(args, ucp);
  klog("setcontext(%p)", ucp);

  ucontext_t uc;
  copyin_s(ucp, uc);

  return do_setcontext(p->p_thread, &uc);
}

static int sys_ioctl(proc_t *p, ioctl_args_t *args, register_t *res) {
  int fd = SCARG(args, fd);
  u_long cmd = SCARG(args, cmd);
  void *u_data = SCARG(args, data);
  int error;

  klog("ioctl(%d, %lx, %p)", fd, cmd, u_data);

  /* For cmd format look at <sys/ioccom.h> header file. */
  unsigned len = IOCPARM_LEN(cmd);
  unsigned dir = cmd & IOC_DIRMASK;
  void *data = NULL;

  /* Allocate a kernel buffer if ioctl needs/returns information. */
  if (dir)
    data = kmalloc(M_TEMP, len, 0);

  if ((dir & IOC_IN) && (error = copyin(u_data, data, len)))
    goto fail;

  if ((error = do_ioctl(p, fd, cmd, data)))
    goto fail;

  if ((dir & IOC_OUT) && (error = copyout(data, u_data, len)))
    goto fail;

fail:
  if (dir)
    kfree(M_TEMP, data);

  return error;
}

static int sys_getresuid(proc_t *p, getresuid_args_t *args, register_t *res) {
  uid_t *usr_ruid = SCARG(args, ruid);
  uid_t *usr_euid = SCARG(args, euid);
  uid_t *usr_suid = SCARG(args, suid);

  klog("getresuid()");

  uid_t ruid, euid, suid;
  do_getresuid(p, &ruid, &euid, &suid);

  int err1 = copyout(&ruid, usr_ruid, sizeof(uid_t));
  int err2 = copyout(&euid, usr_euid, sizeof(uid_t));
  int err3 = copyout(&suid, usr_suid, sizeof(uid_t));

  return err1 ? err1 : (err2 ? err2 : err3);
}

static int sys_getresgid(proc_t *p, getresgid_args_t *args, register_t *res) {
  gid_t *usr_rgid = SCARG(args, rgid);
  gid_t *usr_egid = SCARG(args, egid);
  gid_t *usr_sgid = SCARG(args, sgid);

  klog("getresgid()");

  gid_t rgid, egid, sgid;
  do_getresgid(p, &rgid, &egid, &sgid);

  int err1 = copyout(&rgid, usr_rgid, sizeof(gid_t));
  int err2 = copyout(&egid, usr_egid, sizeof(gid_t));
  int err3 = copyout(&sgid, usr_sgid, sizeof(gid_t));

  return err1 ? err1 : (err2 ? err2 : err3);
}

static int sys_setresuid(proc_t *p, setresuid_args_t *args, register_t *res) {
  uid_t ruid = SCARG(args, ruid);
  uid_t euid = SCARG(args, euid);
  uid_t suid = SCARG(args, suid);

  klog("setresuid(%d, %d, %d)", ruid, euid, suid);

  return do_setresuid(p, ruid, euid, suid);
}

static int sys_setresgid(proc_t *p, setresgid_args_t *args, register_t *res) {
  gid_t rgid = SCARG(args, rgid);
  gid_t egid = SCARG(args, egid);
  gid_t sgid = SCARG(args, sgid);

  klog("setresgid(%d, %d, %d)", rgid, egid, sgid);

  return do_setresgid(p, rgid, egid, sgid);
}

/* TODO: not implemented */
static int sys_issetugid(proc_t *p, void *args, register_t *res) {
  klog("issetugid()");
  *res = 0;
  return 0;
}

static int sys_truncate(proc_t *p, truncate_args_t *args, register_t *res) {
  const char *u_path = SCARG(args, path);
  off_t length = SCARG(args, length);

  char *path = kmalloc(M_TEMP, PATH_MAX, 0);
  int error;

  if ((error = copyinstr(u_path, path, PATH_MAX, NULL)))
    goto end;

  klog("truncate(\"%s\", %d)", path, length);

  error = do_truncate(p, path, length);

end:
  kfree(M_TEMP, path);
  return error;
}

static int sys_ftruncate(proc_t *p, ftruncate_args_t *args, register_t *res) {
  int fd = SCARG(args, fd);
  off_t length = SCARG(args, length);
  klog("ftruncate(%d, %d)", fd, length);
  return do_ftruncate(p, fd, length);
}

static int sys_fstatat(proc_t *p, fstatat_args_t *args, register_t *res) {
  int fd = SCARG(args, fd);
  const char *u_path = SCARG(args, path);
  stat_t *u_sb = SCARG(args, sb);
  int flag = SCARG(args, flag);
  stat_t sb;
  int error;

  char *path = kmalloc(M_TEMP, PATH_MAX, 0);

  if ((error = copyinstr(u_path, path, PATH_MAX, NULL)))
    goto end;

  klog("fstatat(%d, \"%s\", %p, %d)", fd, path, u_sb, flag);

  if (!(error = do_fstatat(p, fd, path, &sb, flag)))
    error = copyout_s(sb, u_sb);

end:
  kfree(M_TEMP, path);
  return error;
}

static int sys_readlinkat(proc_t *p, readlinkat_args_t *args, register_t *res) {
  int fd = SCARG(args, fd);
  const char *u_path = SCARG(args, path);
  char *u_buf = SCARG(args, buf);
  size_t bufsiz = SCARG(args, bufsiz);
  int error;

  char *path = kmalloc(M_TEMP, PATH_MAX, 0);

  if ((error = copyinstr(u_path, path, PATH_MAX, NULL)))
    goto end;

  klog("readlinkat(%d, \"%s\", %p, %u)", fd, path, u_buf, bufsiz);

  uio_t uio = UIO_SINGLE_USER(UIO_READ, 0, u_buf, bufsiz);
  if (!(error = do_readlinkat(p, fd, path, &uio)))
    *res = bufsiz - uio.uio_resid;

end:
  kfree(M_TEMP, path);
  return error;
}

static int sys_symlinkat(proc_t *p, symlinkat_args_t *args, register_t *res) {
  const char *u_target = SCARG(args, target);
  int newdirfd = SCARG(args, newdirfd);
  const char *u_linkpath = SCARG(args, linkpath);
  int error;

  char *target = kmalloc(M_TEMP, PATH_MAX, 0);
  char *linkpath = kmalloc(M_TEMP, PATH_MAX, 0);

  if ((error = copyinstr(u_target, target, PATH_MAX, NULL)))
    goto end;
  if ((error = copyinstr(u_linkpath, linkpath, PATH_MAX, NULL)))
    goto end;

  klog("symlinkat(\"%s\", %d, \"%s\")", target, newdirfd, linkpath);

  error = do_symlinkat(p, target, newdirfd, linkpath);

end:
  kfree(M_TEMP, target);
  kfree(M_TEMP, linkpath);
  return error;
}

static int sys_linkat(proc_t *p, linkat_args_t *args, register_t *res) {
  int fd1 = SCARG(args, fd1);
  const char *u_name1 = SCARG(args, name1);
  int fd2 = SCARG(args, fd2);
  const char *u_name2 = SCARG(args, name2);
  int flags = SCARG(args, flags);
  int error;

  char *name1 = kmalloc(M_TEMP, PATH_MAX, 0);
  char *name2 = kmalloc(M_TEMP, PATH_MAX, 0);

  if ((error = copyinstr(u_name1, name1, PATH_MAX, NULL)))
    goto end;
  if ((error = copyinstr(u_name2, name2, PATH_MAX, NULL)))
    goto end;

  klog("linkat(%d, \"%s\", %d, \"%s\", %d)", fd1, u_name1, fd2, u_name2, flags);

  error = do_linkat(p, fd1, name1, fd2, name2, flags);

end:
  kfree(M_TEMP, name1);
  kfree(M_TEMP, name2);
  return error;
}

static int sys_fchmod(proc_t *p, fchmod_args_t *args, register_t *res) {
  int fd = SCARG(args, fd);
  mode_t mode = SCARG(args, mode);

  klog("fchmod(%d, %d)", fd, mode);

  return do_fchmod(p, fd, mode);
}

static int sys_fchmodat(proc_t *p, fchmodat_args_t *args, register_t *res) {
  int fd = SCARG(args, fd);
  const char *u_path = SCARG(args, path);
  mode_t mode = SCARG(args, mode);
  int flag = SCARG(args, flag);
  int error;

  char *path = kmalloc(M_TEMP, PATH_MAX, 0);

  if ((error = copyinstr(u_path, path, PATH_MAX, NULL)))
    goto end;

  klog("fchmodat(%d, \"%s\", %d, %d)", fd, path, mode, flag);

  error = do_fchmodat(p, fd, path, mode, flag);

end:
  kfree(M_TEMP, path);
  return error;
}

static int sys_fchown(proc_t *p, fchown_args_t *args, register_t *res) {
  int fd = SCARG(args, fd);
  uid_t uid = SCARG(args, uid);
  gid_t gid = SCARG(args, gid);

  klog("fchown(%d, %d)", uid, gid);

  return do_fchown(p, fd, uid, gid);
}

static int sys_fchownat(proc_t *p, fchownat_args_t *args, register_t *res) {
  int fd = SCARG(args, fd);
  const char *u_path = SCARG(args, path);
  uid_t uid = SCARG(args, uid);
  gid_t gid = SCARG(args, gid);
  int flag = SCARG(args, flag);
  int error;

  char *path = kmalloc(M_TEMP, PATH_MAX, 0);

  if ((error = copyinstr(u_path, path, PATH_MAX, NULL)))
    goto end;

  klog("fchownat(%d, \"%s\", %d, %d)", fd, path, uid, uid);

  error = do_fchownat(p, fd, path, uid, gid, flag);

end:
  kfree(M_TEMP, path);
  return error;
}

static int sys_sched_yield(proc_t *p, void *args, register_t *res) {
  klog("sched_yield()");
  thread_yield();
  return 0;
}

static int sys_statvfs(proc_t *p, statvfs_args_t *args, register_t *res) {
  int error;
  const char *u_path = SCARG(args, path);
  statvfs_t *u_buf = SCARG(args, buf);
  statvfs_t buf;

  char *path = kmalloc(M_TEMP, PATH_MAX, 0);

  if ((error = copyinstr(u_path, path, PATH_MAX, NULL)))
    goto end;

  klog("statvfs(\"%s\", %p)", path, u_buf);

  if (!(error = do_statvfs(p, path, &buf)))
    error = copyout_s(buf, u_buf);

end:
  kfree(M_TEMP, path);
  return error;
}

static int sys_fstatvfs(proc_t *p, fstatvfs_args_t *args, register_t *res) {
  int error;
  int fd = SCARG(args, fd);
  statvfs_t *u_buf = SCARG(args, buf);
  statvfs_t buf;

  klog("fstatvfs(%d, %p)", fd, u_buf);

  if (!(error = do_fstatvfs(p, fd, &buf)))
    error = copyout_s(buf, u_buf);

  return error;
}

static int sys_getgroups(proc_t *p, getgroups_args_t *args, register_t *res) {
  int ngroups = SCARG(args, ngroups);
  gid_t *ugidset = SCARG(args, gidset);
  int pngroups = p->p_cred.cr_ngroups;

  klog("getgroups(%d, %p)", ngroups, ugidset);

  if (ngroups == 0) {
    /* just return number of groups */
    *res = pngroups;
    return 0;
  }

  if (ngroups < pngroups)
    return EINVAL;

  *res = pngroups;
  return copyout(p->p_cred.cr_groups, ugidset, pngroups * sizeof(gid_t));
}

static int sys_setgroups(proc_t *p, setgroups_args_t *args, register_t *res) {
  int error = 0;
  int ungroups = SCARG(args, ngroups);
  const gid_t *ugidset = SCARG(args, gidset);

  klog("setgroups(%d, %p)", ungroups, ugidset);

  /* too many groups */
  if (ungroups < 0 || ungroups > NGROUPS_MAX)
    return EINVAL;

  gid_t *gidset = kmalloc(M_TEMP, ungroups * sizeof(gid_t), M_NOWAIT);

  /* if we set 0 groups kmalloc returns NULL, but it is not an error */
  if (ungroups > 0 && gidset == NULL)
    return ENOMEM;

  if (!(error = copyin(ugidset, gidset, ungroups * sizeof(gid_t))))
    error = do_setgroups(p, ungroups, gidset);

  kfree(M_TEMP, gidset);
  return error;
}

static int sys_setsid(proc_t *p, void *args, register_t *res) {
  int error;

  klog("setsid()");

  if ((error = session_enter(p)))
    return error;

  *res = p->p_pid;
  return 0;
}

static int sys_getsid(proc_t *p, getsid_args_t *args, register_t *res) {
  pid_t pid = SCARG(args, pid);
  sid_t sid;
  int error;

  if (pid < 0)
    return EINVAL;

  if (pid == 0)
    pid = p->p_pid;

  klog("getsid(%d)", pid);

  if (!(error = proc_getsid(pid, &sid)))
    *res = sid;

  return error;
}

static int sys_getpriority(proc_t *p, getpriority_args_t *args,
                           register_t *res) {
  /* TODO(fzdob): this is only simple stub to avoid erasing these syscall from
   * userspace programs */
  (void)args;
  *res = 0;
  return 0;
}

static int sys_setpriority(proc_t *p, setpriority_args_t *args,
                           register_t *res) {
  /* TODO(fzdob): this is only simple stub to avoid erasing these syscall from
   * userspace programs */
  (void)args;
  *res = 0;
  return 0;
}

static int sys_setuid(proc_t *p, setuid_args_t *args, register_t *res) {
  uid_t uid = SCARG(args, uid);
  return do_setuid(p, uid);
}

static int sys_seteuid(proc_t *p, seteuid_args_t *args, register_t *res) {
  uid_t euid = SCARG(args, euid);
  return do_seteuid(p, euid);
}

static int sys_setreuid(proc_t *p, setreuid_args_t *args, register_t *res) {
  uid_t ruid = SCARG(args, ruid);
  uid_t euid = SCARG(args, euid);
  return do_setreuid(p, ruid, euid);
}

static int sys_setgid(proc_t *p, setgid_args_t *args, register_t *res) {
  gid_t gid = SCARG(args, gid);
  return do_setgid(p, gid);
}

static int sys_setegid(proc_t *p, setegid_args_t *args, register_t *res) {
  gid_t egid = SCARG(args, egid);
  return do_setegid(p, egid);
}

static int sys_setregid(proc_t *p, setregid_args_t *args, register_t *res) {
  gid_t rgid = SCARG(args, rgid);
  gid_t egid = SCARG(args, egid);
  return do_setregid(p, rgid, egid);
}

static int sys_getlogin(proc_t *p, getlogin_args_t *args, register_t *res) {
  char *namebuf = SCARG(args, namebuf);
  size_t buflen = SCARG(args, buflen);
  char login_tmp[LOGIN_NAME_MAX];

  klog("getlogin(%p, %zu)", namebuf, buflen);

  WITH_MTX_LOCK (&all_proc_mtx)
    memcpy(login_tmp, p->p_pgrp->pg_session->s_login, sizeof(login_tmp));

  return copyout(login_tmp, namebuf, min(buflen, sizeof(login_tmp)));
}

static int sys_setlogin(proc_t *p, setlogin_args_t *args, register_t *res) {
  char *name = SCARG(args, name);
  char login_tmp[LOGIN_NAME_MAX];
  int error;

  klog("setlogin(%p)", name);

  error = copyinstr(name, login_tmp, sizeof(login_tmp), NULL);
  if (error)
    return (error == ENAMETOOLONG ? EINVAL : error);

  return do_setlogin(login_tmp);
}

static int sys_posix_openpt(proc_t *p, posix_openpt_args_t *args,
                            register_t *res) {

  int flags = SCARG(args, flags);

  klog("posix_openpt(0x%x)", flags);

  return do_posix_openpt(p, flags, res);
}

static int sys_futimens(proc_t *p, futimens_args_t *args, register_t *res) {
  int fd = SCARG(args, fd);
  const timespec_t *u_times = SCARG(args, times);
  timespec_t times[2];
  int error;

  klog("futimens(%d, %x)", fd, u_times);

  if (u_times != NULL) {
    if ((error = copyin_s(u_times, times)))
      return error;
  }

  return do_futimens(p, fd, u_times == NULL ? NULL : times);
}

static int sys_utimensat(proc_t *p, utimensat_args_t *args, register_t *res) {
  int fd = SCARG(args, fd);
  const char *u_path = SCARG(args, path);
  const timespec_t *u_times = SCARG(args, times);
  int flag = SCARG(args, flag);
  timespec_t times[2];
  int error;

  char *path = kmalloc(M_TEMP, PATH_MAX, 0);

  if ((error = copyinstr(u_path, path, PATH_MAX, NULL)))
    goto end;

  klog("utimensat(%d, \"%s\", %x, %d)", fd, path, u_times, flag);

  if (u_times != NULL) {
    if ((error = copyin_s(u_times, times)))
      goto end;
  }

  error = do_utimensat(p, fd, path, u_times == NULL ? NULL : times, flag);

end:
  kfree(M_TEMP, path);
  return error;
}

static int sys_readv(proc_t *p, readv_args_t *args, register_t *res) {
  int fd = SCARG(args, fd);
  const iovec_t *u_iov = SCARG(args, iov);
  int iovcnt = SCARG(args, iovcnt);
  size_t len;
  int error;

  if (iovcnt <= 0 || iovcnt > IOV_MAX)
    return EINVAL;

  const size_t iov_size = sizeof(iovec_t) * iovcnt;
  iovec_t *k_iov = kmalloc(M_TEMP, iov_size, 0);

  if ((error = copyin(u_iov, k_iov, iov_size)) ||
      (error = iovec_length(k_iov, iovcnt, &len)))
    goto end;

  uio_t uio = UIO_VECTOR_USER(UIO_READ, k_iov, iovcnt, len);
  error = do_read(p, fd, &uio);
  *res = len - uio.uio_resid;

end:
  kfree(M_TEMP, k_iov);
  return error;
}

static int sys_writev(proc_t *p, writev_args_t *args, register_t *res) {
  int fd = SCARG(args, fd);
  const iovec_t *u_iov = SCARG(args, iov);
  int iovcnt = SCARG(args, iovcnt);
  size_t len;
  int error;

  if (iovcnt <= 0 || iovcnt > IOV_MAX)
    return EINVAL;

  const size_t iov_size = sizeof(iovec_t) * iovcnt;
  iovec_t *k_iov = kmalloc(M_TEMP, iov_size, 0);

  if ((error = copyin(u_iov, k_iov, iov_size)) ||
      (error = iovec_length(k_iov, iovcnt, &len)))
    goto end;

  uio_t uio = UIO_VECTOR_USER(UIO_WRITE, k_iov, iovcnt, len);
  error = do_write(p, fd, &uio);
  *res = len - uio.uio_resid;

end:
  kfree(M_TEMP, k_iov);
  return error;
}

static int sys_sigpending(proc_t *p, sigpending_args_t *args, register_t *res) {
  sigset_t *u_set = SCARG(args, set);
  int error;

  klog("sigpending(%p)", u_set);

  sigset_t k_set;

  if ((error = do_sigpending(p, &k_set)))
    return error;

  return copyout_s(k_set, u_set);
}

static int sys_getitimer(proc_t *p, getitimer_args_t *args, register_t *res) {
  int which = SCARG(args, which);
  struct itimerval *u_tval = SCARG(args, val);
  int error;

  klog("getitimer(%p)", u_tval);

  struct itimerval tval;

  if ((error = do_getitimer(p, which, &tval)))
    return error;

  return copyout_s(tval, u_tval);
}

static int sys_setitimer(proc_t *p, setitimer_args_t *args, register_t *res) {
  int which = SCARG(args, which);
  struct itimerval *u_tval = SCARG(args, val);
  struct itimerval *u_oval = SCARG(args, oval);
  int error;

  klog("setitimer(%p, %p)", u_tval, u_oval);

  struct itimerval tval, oval;
  if ((error = copyin_s(u_tval, tval)))
    return error;

  if ((error = do_setitimer(p, which, &tval, u_oval ? &oval : NULL)))
    return error;

  if (u_oval)
    error = copyout_s(oval, u_oval);

  return error;
}

static int sys_kqueue1(proc_t *p, kqueue1_args_t *args, register_t *res) {
  int flags = SCARG(args, flags);
  int fd, error;

  klog("kqueue1(%d)", flags);

  error = do_kqueue1(p, flags, &fd);
  *res = fd;

  return error;
}

static int sys_kevent(proc_t *p, kevent_args_t *args, register_t *res) {
  int kq = SCARG(args, kq);
  const struct kevent *u_changelist = SCARG(args, changelist);
  size_t nchanges = SCARG(args, nchanges);
  struct kevent *u_eventlist = SCARG(args, eventlist);
  size_t nevents = SCARG(args, nevents);
  const struct timespec *u_timeout = SCARG(args, timeout);

  kevent_t *changelist = kmalloc(M_TEMP, nchanges * sizeof(kevent_t), 0);
  kevent_t *eventlist = kmalloc(M_TEMP, nevents * sizeof(kevent_t), 0);
  timespec_t timeout;
  int error, nret;

  klog("kevent(%d, %p, %u, %p, %u, %p)", kq, u_changelist, nchanges,
       u_eventlist, nevents, u_timeout);

  if ((error = copyin(u_changelist, changelist, nchanges * sizeof(kevent_t))))
    goto end;

  if ((error = copyin_s(u_timeout, timeout)))
    goto end;

  if ((error = do_kevent(p, kq, changelist, nchanges, eventlist, nevents,
                         &timeout, &nret)))
    goto end;

  if ((error = copyout(eventlist, u_eventlist, nret * sizeof(kevent_t))))
    goto end;

  *res = nret;

end:
  kfree(M_TEMP, changelist);
  kfree(M_TEMP, eventlist);
  return error;
}