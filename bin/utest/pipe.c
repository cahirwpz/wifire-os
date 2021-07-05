#include <assert.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>

#include <signal.h>
#include <string.h>

#include <sys/types.h>
#include <sys/wait.h>

#include "utest.h"
#include "util.h"

static sig_atomic_t signal_delivered;

static void handler(int signo) {
  if (signo == SIGPIPE) {
    assert(signo == SIGPIPE);
    signal_delivered = 1;
  }
  if (signo == SIGALRM) {
  }
}

int test_pipe_parent_signaled(void) {
  int pipe_fd[2];
  signal_delivered = 0;
  signal(SIGPIPE, handler);

  /* creating pipe */
  int pipe2_ret = pipe2(pipe_fd, 0);
  assert(pipe2_ret == 0);

  /* forking */
  pid_t child_pid = fork();
  assert(child_pid >= 0);

  if (child_pid == 0) { /* child */
    close(pipe_fd[1]);  /* closing write end of pipe */
    close(pipe_fd[0]);  /* closing read end of pipe */
    exit(EXIT_SUCCESS);
  }

  /* parent */
  close(pipe_fd[0]); /* closing read end of pipe */

  /* Sync with end of child execution */
  wait_for_child_exit(child_pid, EXIT_SUCCESS);

  /* This is supposed to trigger SIGPIPE and return EPIPE */
  ssize_t write_ret = write(pipe_fd[1], "hello world\n", 12);
  assert(write_ret == -1);
  assert(errno == EPIPE);
  assert(signal_delivered);

  return 0;
}

int test_pipe_child_signaled(void) {
  int pipe_fd[2];
  signal_delivered = 0;

  /* set up SIGUSR1 so it's not lethal for my child */
  signal_setup(SIGUSR1);

  /* creating pipe */
  int pipe2_ret = pipe2(pipe_fd, 0);
  assert(pipe2_ret == 0);

  /* forking */
  pid_t child_pid = fork();
  assert(child_pid >= 0);

  if (child_pid == 0) { /* child */
    signal(SIGPIPE, handler);

    close(pipe_fd[0]);        /* closing read end of pipe */
    wait_for_signal(SIGUSR1); /* now we know that other end is closed */

    /* This is supposed to trigger SIGPIPE and return EPIPE */
    ssize_t write_ret = write(pipe_fd[1], "hello world\n", 12);
    assert(write_ret == -1);
    assert(errno == EPIPE);
    assert(signal_delivered);

    exit(EXIT_SUCCESS);
  }

  /* parent */
  close(pipe_fd[1]); /* closing write end of pipe */
  close(pipe_fd[0]); /* closing read end of pipe */

  /* send SIGUSR1 informing that parent closed both ends of pipe */
  kill(child_pid, SIGUSR1);

  /* I really want child to finish, not just change it's state.
   * so i don't use wait_for_child_exit
   */
  int wstatus = 1;
  do {
    ssize_t waitpid_ret = waitpid(child_pid, &wstatus, 0);
    assert(waitpid_ret == child_pid);
  } while (!WIFEXITED(wstatus));

  assert(WEXITSTATUS(wstatus) == EXIT_SUCCESS);

  return 0;
}

int test_pipe_blocking_flag_manipulation(void) {
  int pipe_fd[2];

  /* creating pipe */
  int pipe2_ret = pipe2(pipe_fd, O_NONBLOCK);
  assert(pipe2_ret == 0);

  /* check if flag is set */
  assert(fcntl(pipe_fd[0], F_GETFL) & O_NONBLOCK);
  assert(fcntl(pipe_fd[1], F_GETFL) & O_NONBLOCK);

  /* unset same flag for read end */
  int read_flagset_with_block = fcntl(pipe_fd[0], F_GETFL);
  assert(read_flagset_with_block > 0);
  fcntl(pipe_fd[0], F_SETFL, read_flagset_with_block & ~O_NONBLOCK);
  /* unset same flag for write end */
  int write_flagset_with_block = fcntl(pipe_fd[1], F_GETFL);
  assert(write_flagset_with_block > 0);
  fcntl(pipe_fd[1], F_SETFL, write_flagset_with_block & ~O_NONBLOCK);

  /* check if flag is not set */
  assert(!(fcntl(pipe_fd[0], F_GETFL) & O_NONBLOCK));
  assert(!(fcntl(pipe_fd[1], F_GETFL) & O_NONBLOCK));

  close(pipe_fd[0]);
  close(pipe_fd[1]);

  return 0;
}

int test_pipe_write_interruptible_sleep(void) {
  int pipe_fd[2];
  pid_t child_pid;

  /* creating pipe */
  int pipe2_ret = pipe2(pipe_fd, 0);
  assert(pipe2_ret == 0);

  /* forking */
  child_pid = fork();
  assert(child_pid >= 0);

  if (child_pid == 0) { /* child */
    close(pipe_fd[0]);  /* closing read end of pipe */

    struct sigaction sa = {
      .sa_handler = handler,
      .sa_flags = 0,
    };

    sigaction(SIGALRM, &sa, NULL);

    int page_size = getpagesize();
    char data[page_size];

    for (int i = 0; i < page_size; i++) {
      data[i] = i + '0';
    }
    int bytes_wrote = 0;
    alarm(1);

    while (bytes_wrote >= 0) {
      bytes_wrote = write(pipe_fd[1], &data, sizeof(data));
    }
    assert(bytes_wrote == -1);
    assert(errno == EINTR);

    close(pipe_fd[1]); /* closing write end of pipe */
    exit(EXIT_SUCCESS);
  }

  close(pipe_fd[1]); /* closing write end of pipe */
  wait_for_child_exit(child_pid, EXIT_SUCCESS);
  close(pipe_fd[0]); /* closing read end of pipe */
  return 0;
}

int test_pipe_write_errno_eagain(void) {
  int pipe_fd[2];
  pid_t child_pid;
  int bytes_wrote = 0;

  /* creating pipe */
  int pipe2_ret = pipe2(pipe_fd, O_NONBLOCK);
  assert(pipe2_ret == 0);

  /* forking */
  child_pid = fork();
  assert(child_pid >= 0);

  if (child_pid == 0) {
    close(pipe_fd[0]); /* closing read end of pipe */

    int page_size = getpagesize();
    /* prepare varying data */
    char data[page_size];
    for (int i = 0; i < page_size; i++) {
      data[i] = i + '0';
    }

    /* overflowing pipe */
    while (bytes_wrote >= 0) {
      bytes_wrote = write(pipe_fd[1], &data, sizeof(data));
    }
    assert(bytes_wrote == -1);
    assert(errno == EAGAIN);

    close(pipe_fd[1]); /* closing write end of pipe */
    exit(EXIT_SUCCESS);
  }

  close(pipe_fd[1]); /* closing write end of pipe */
  wait_for_child_exit(child_pid, EXIT_SUCCESS);
  close(pipe_fd[0]);
  return 0;
}

int test_pipe_read_interruptible_sleep(void) {
  int pipe_fd[2];
  pid_t child_pid;
  int bytes_wrote;

  /* creating pipe */
  int pipe2_ret = pipe2(pipe_fd, 0);
  assert(pipe2_ret == 0);

  /* forking */
  child_pid = fork();
  assert(child_pid >= 0);

  if (child_pid == 0) { /* child */
    close(pipe_fd[1]);  /* closing write end of pipe */

    struct sigaction sa = {
      .sa_handler = handler,
      .sa_flags = 0,
    };

    sigaction(SIGALRM, &sa, NULL);

    char buf;
    alarm(1);

    bytes_wrote = read(pipe_fd[0], &buf, 1);

    assert(bytes_wrote == -1);
    assert(errno == EINTR);

    close(pipe_fd[0]);
    exit(EXIT_SUCCESS);
  }

  close(pipe_fd[0]); /* closing read end of pipe */
  wait_for_child_exit(child_pid, EXIT_SUCCESS);
  close(pipe_fd[1]); /* closing write end of pipe */

  return 0;
}

int test_pipe_read_errno_eagain(void) {
  int pipe_fd[2];
  pid_t child_pid;
  int bytes_wrote;

  /* creating pipe */
  int pipe2_ret = pipe2(pipe_fd, O_NONBLOCK);
  assert(pipe2_ret == 0);

  /* forking */
  child_pid = fork();
  assert(child_pid >= 0);

  if (child_pid == 0) { /* child */

    close(pipe_fd[1]); /* closing write end of pipe */

    char buf;
    bytes_wrote = read(pipe_fd[0], &buf, 1);
    assert(errno == EAGAIN);
    assert(bytes_wrote == -1);
    close(pipe_fd[0]);

    exit(EXIT_SUCCESS);
  }

  close(pipe_fd[0]); /* closing read end of pipe */
  wait_for_child_exit(child_pid, EXIT_SUCCESS);
  close(pipe_fd[1]); /* closing write end of pipe */
  return 0;
}

int test_pipe_read_return_zero(void) {
  int pipe_fd[2];
  pid_t child_pid;
  int bytes_wrote;

  /* creating pipe */
  int pipe2_ret = pipe2(pipe_fd, 0);
  assert(pipe2_ret == 0);

  /* forking */
  child_pid = fork();
  assert(child_pid >= 0);

  if (child_pid == 0) { /* child */
    close(pipe_fd[1]);  /* closing write end of pipe */

    char buf;
    bytes_wrote = read(pipe_fd[0], &buf, 1);

    assert(bytes_wrote == 0);
    assert(errno == 0);

    close(pipe_fd[0]);
    exit(EXIT_SUCCESS);
  }

  close(pipe_fd[0]); /* closing read end of pipe */
  close(pipe_fd[1]); /* closing write end of pipe */
  wait_for_child_exit(child_pid, EXIT_SUCCESS);
  return 0;
}
