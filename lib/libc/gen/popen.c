/*	$NetBSD: popen.c,v 1.36 2019/01/24 18:01:38 christos Exp $	*/

/*
 * Copyright (c) 1988, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software written by Ken Arnold and
 * published in UNIX Review, Vol. 6, No. 8.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/wait.h>
#include <sys/socket.h>

#include <assert.h>
#include <errno.h>
#include <paths.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "env.h"

static struct pid {
  struct pid *next;
  FILE *fp;
#ifdef _REENTRANT
  int fd;
#endif
  pid_t pid;
} * pidlist;

#ifdef _REENTRANT
static mutex_t pidlist_mutex = MUTEX_INITIALIZER;
#define MUTEX_LOCK()                                                           \
  do {                                                                         \
    if (__isthreaded)                                                          \
      mutex_lock(&pidlist_mutex);                                              \
  } while (/*CONSTCOND*/ 0)
#define MUTEX_UNLOCK()                                                         \
  do {                                                                         \
    if (__isthreaded)                                                          \
      mutex_unlock(&pidlist_mutex);                                            \
  } while (/*CONSTCOND*/ 0)
#else
#define MUTEX_LOCK() __nothing
#define MUTEX_UNLOCK() __nothing
#endif

static struct pid *pdes_get(int *pdes, const char **type) {
  struct pid *cur;
  int flags = strchr(*type, 'e') ? O_CLOEXEC : 0;
  int serrno;

  if (strchr(*type, '+')) {
    int stype = flags ? (SOCK_STREAM | SOCK_CLOEXEC) : SOCK_STREAM;
    *type = "r+";
    if (socketpair(AF_LOCAL, stype, 0, pdes) < 0)
      return NULL;
  } else {
    *type = strrchr(*type, 'r') ? "r" : "w";
    if (pipe2(pdes, flags) == -1)
      return NULL;
  }

  if ((cur = malloc(sizeof(*cur))) != NULL)
    return cur;
  serrno = errno;
  (void)close(pdes[0]);
  (void)close(pdes[1]);
  errno = serrno;
  return NULL;
}

static void pdes_child(int *pdes, const char *type) {
  struct pid *old;

  /* POSIX.2 B.3.2.2 "popen() shall ensure that any streams
     from previous popen() calls that remain open in the
     parent process are closed in the new child process. */
  for (old = pidlist; old; old = old->next)
#ifdef _REENTRANT
    (void)close(old->fd); /* don't allow a flush */
#else
    (void)close(fileno(old->fp)); /* don't allow a flush */
#endif

  if (type[0] == 'r') {
    (void)close(pdes[0]);
    if (pdes[1] != STDOUT_FILENO) {
      (void)dup2(pdes[1], STDOUT_FILENO);
      (void)close(pdes[1]);
    }
    if (type[1] == '+')
      (void)dup2(STDOUT_FILENO, STDIN_FILENO);
  } else {
    (void)close(pdes[1]);
    if (pdes[0] != STDIN_FILENO) {
      (void)dup2(pdes[0], STDIN_FILENO);
      (void)close(pdes[0]);
    }
  }
}

static void pdes_parent(int *pdes, struct pid *cur, pid_t pid,
                        const char *type) {
  FILE *iop;

  /* Parent; assume fdopen can't fail. */
  if (*type == 'r') {
    iop = fdopen(pdes[0], type);
#ifdef _REENTRANT
    cur->fd = pdes[0];
#endif
    (void)close(pdes[1]);
  } else {
    iop = fdopen(pdes[1], type);
#ifdef _REENTRANT
    cur->fd = pdes[1];
#endif
    (void)close(pdes[0]);
  }

  /* Link into list of file descriptors. */
  cur->fp = iop;
  cur->pid = pid;
  cur->next = pidlist;
  pidlist = cur;
}

static void pdes_error(int *pdes, struct pid *cur) {
  free(cur);
  (void)close(pdes[0]);
  (void)close(pdes[1]);
}

FILE *popen(const char *cmd, const char *type) {
  struct pid *cur;
  int pdes[2], serrno;
  pid_t pid;

  _DIAGASSERT(cmd != NULL);
  _DIAGASSERT(type != NULL);

  if ((cur = pdes_get(pdes, &type)) == NULL)
    return NULL;

  MUTEX_LOCK();
  (void)__readlockenv();
  switch (pid = vfork()) {
    case -1: /* Error. */
      serrno = errno;
      (void)__unlockenv();
      MUTEX_UNLOCK();
      pdes_error(pdes, cur);
      errno = serrno;
      return NULL;
      /* NOTREACHED */
    case 0: /* Child. */
      pdes_child(pdes, type);
      execl(_PATH_BSHELL, "sh", "-c", cmd, NULL);
      _exit(127);
      /* NOTREACHED */
  }
  (void)__unlockenv();

  pdes_parent(pdes, cur, pid, type);

  MUTEX_UNLOCK();

  return cur->fp;
}

FILE *popenve(const char *cmd, char *const *argv, char *const *envp,
              const char *type) {
  struct pid *cur;
  int pdes[2], serrno;
  pid_t pid;

  _DIAGASSERT(cmd != NULL);
  _DIAGASSERT(type != NULL);

  if ((cur = pdes_get(pdes, &type)) == NULL)
    return NULL;

  MUTEX_LOCK();
  switch (pid = vfork()) {
    case -1: /* Error. */
      serrno = errno;
      MUTEX_UNLOCK();
      pdes_error(pdes, cur);
      errno = serrno;
      return NULL;
      /* NOTREACHED */
    case 0: /* Child. */
      pdes_child(pdes, type);
      execve(cmd, argv, envp);
      _exit(127);
      /* NOTREACHED */
  }

  pdes_parent(pdes, cur, pid, type);

  MUTEX_UNLOCK();

  return cur->fp;
}

/*
 * pclose --
 *	Pclose returns -1 if stream is not associated with a `popened' command,
 *	if already `pclosed', or waitpid returns an error.
 */
int pclose(FILE *iop) {
  struct pid *cur, *last;
  int pstat;
  pid_t pid;

  _DIAGASSERT(iop != NULL);

  MUTEX_LOCK();

  /* Find the appropriate file pointer. */
  for (last = NULL, cur = pidlist; cur; last = cur, cur = cur->next)
    if (cur->fp == iop)
      break;
  if (cur == NULL) {
    MUTEX_UNLOCK();
    errno = ESRCH;
    return -1;
  }

  (void)fclose(iop);

  /* Remove the entry from the linked list. */
  if (last == NULL)
    pidlist = cur->next;
  else
    last->next = cur->next;

  MUTEX_UNLOCK();

  do {
    pid = waitpid(cur->pid, &pstat, 0);
  } while (pid == -1 && errno == EINTR);

  free(cur);

  return pid == -1 ? -1 : pstat;
}
