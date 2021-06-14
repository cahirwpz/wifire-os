#define KL_LOG KL_INIT
#include <sys/klog.h>
#include <sys/libkern.h>
#include <sys/kmem.h>
#include <sys/vmem.h>
#include <sys/pool.h>
#include <sys/malloc.h>
#include <sys/device.h>
#include <sys/bus.h>
#include <sys/kenv.h>
#include <sys/sched.h>
#include <sys/interrupt.h>
#include <sys/sleepq.h>
#include <sys/turnstile.h>
#include <sys/thread.h>
#include <sys/proc.h>
#include <sys/filedesc.h>
#include <sys/exec.h>
#include <sys/ktest.h>
#include <sys/fcntl.h>
#include <sys/vfs.h>
#include <sys/vnode.h>
#include <sys/vm_map.h>
#include <sys/vm_physmem.h>
#include <sys/pmap.h>
#include <sys/console.h>
#include <sys/stat.h>
#include <sys/lockdep.h>
#include <sys/kcsan.h>
#include <sys/kgprof.h>
#include <sys/dtb.h>

/* This function mounts some initial filesystems. Normally this would be done by
   userspace init program. */
static void mount_fs(void) {
  proc_t *p = &proc0;
  do_mount(p, NULL, "initrd", "/");
  do_mount(p, NULL, "devfs", "/dev");
  do_mount(p, NULL, "tmpfs", "/tmp");
  do_fchmodat(p, AT_FDCWD, "/tmp", ACCESSPERMS | S_ISTXT, 0);
}

static __noreturn void start_init(__unused void *arg) {
  proc_t *p = proc_self();
  int error;

  /* [SECOND_PASS] Init devices that need extra kernel API to be functional. */
  init_devices();

  assert(p->p_pid == 1);
  error = session_enter(p);
  assert(error == 0);

  /* ... and initialize file descriptors required by the standard library. */
  int _stdin, _stdout, _stderr;
  do_open(p, "/dev/uart", O_RDONLY, 0, &_stdin);
  do_open(p, "/dev/uart", O_WRONLY, 0, &_stdout);
  do_open(p, "/dev/uart", O_WRONLY, 0, &_stderr);

  assert(_stdin == 0);
  assert(_stdout == 1);
  assert(_stderr == 2);

  char *init = kenv_get("init");
  if (init)
    kern_execve(init, kenv_get_init(), (char *[]){NULL});

  char *test = kenv_get("test");
  if (test)
    ktest_main(test);

  panic("Use init=PROGRAM to start a user-space program "
        "or test=TESTLIST to run tests.");
}

__noreturn void kernel_init(void) {
  init_pmap();
  init_vm_page();
  init_kmalloc();
  init_pool();
  init_vmem();
  init_kmem();
  init_vm_map();

  init_cons();

  /* Make dispatcher & scheduler structures ready for use. */
  init_sleepq();
  init_turnstile();
  lockdep_init();
  init_thread0();
  init_sched();

  /* With scheduler ready we can create necessary threads. */
  init_callout();
  preempt_enable();

  /* [FIRST_PASS] Initialize first timer and console devices. */
  dtb_init();
  init_devices();

  init_vfs();
  init_proc();
  init_proc0();

  /* Mount filesystems (including devfs). */
  mount_fs();

  /* Some clocks has been found during device init process,
   * so it's high time to start system clock. */
  init_clock();

  init_kgprof();

  klog("Kernel initialized!");

  init_kcsan();

  pid_t init_pid;
  do_fork(start_init, NULL, &init_pid);
  assert(init_pid == 1);

  sched_run();
}
