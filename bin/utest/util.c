#include <stdio.h>
#include <assert.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <sys/fcntl.h>

void wait_for_child_exit(int pid, int exit_code) {
  int status;
  assert(waitpid(pid, &status, 0) == pid);
  //printf("Child %d status: %d\n", pid, status);
  assert(WIFEXITED(status));
  assert(WEXITSTATUS(status) == exit_code);
}

static void noop_handler(int signo) {
}

void signal_setup(int signo) {
  signal(signo, noop_handler);
  sigset_t mask;
  __sigemptyset(&mask);
  sigaddset(&mask, signo);
  assert(sigprocmask(SIG_BLOCK, &mask, NULL) == 0);
}

void wait_for_signal(int signo) {
  sigset_t mask;
  __sigfillset(&mask);
  sigdelset(&mask, signo);
  sigsuspend(&mask);
}

void open_pty(int *master_fd, int *slave_fd) {
  *master_fd = posix_openpt(O_NOCTTY | O_RDWR);
  assert(*master_fd >= 0);
  *slave_fd = open(ptsname(*master_fd), O_NOCTTY | O_RDWR);
  assert(*slave_fd >= 0);
}
