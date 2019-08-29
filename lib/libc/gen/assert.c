/*	$NetBSD: assert.c,v 1.18 2017/05/15 16:09:09 christos Exp $	*/

/*-
 * Copyright (c) 1992, 1993
 *	The Regents of the University of California.  All rights reserved.
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
#include <sys/types.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "extern.h"

static int fmtassert(char *buf, size_t len, const char *file, int line,
                     const char *function, const char *failedexpr) {
  return snprintf_ss(buf, len,
                     "assertion \"%s\" failed: file \"%s\", line %d%s%s%s\n",
                     failedexpr, file, line, function ? ", function \"" : "",
                     function ? function : "", function ? "\"" : "");
}

void __assert(const char *file, int line, const char *function,
              const char *failedexpr) {
  char buf[1024];
  int l = fmtassert(buf, sizeof(buf), file, line, function, failedexpr);
  if (l < 0)
    abort();
  (void)write(STDERR_FILENO, buf, (size_t)l);
  abort();
}

enum {
  DIAGASSERT_ABORT = 1 << 0,
  DIAGASSERT_STDERR = 1 << 1,
  DIAGASSERT_SYSLOG = 1 << 2
};

static int diagassert_flags = -1;

void __diagassert(const char *file, int line, const char *function,
                  const char *failedexpr) {
  char buf[1024];

  if (diagassert_flags == -1) {
    char *p;

    diagassert_flags = DIAGASSERT_SYSLOG;

    for (p = getenv("LIBC_DIAGASSERT"); p && *p; p++) {
      switch (*p) {
        case 'a':
          diagassert_flags |= DIAGASSERT_ABORT;
          break;
        case 'A':
          diagassert_flags &= ~DIAGASSERT_ABORT;
          break;
        case 'e':
          diagassert_flags |= DIAGASSERT_STDERR;
          break;
        case 'E':
          diagassert_flags &= ~DIAGASSERT_STDERR;
          break;
        case 'l':
          diagassert_flags |= DIAGASSERT_SYSLOG;
          break;
        case 'L':
          diagassert_flags &= ~DIAGASSERT_SYSLOG;
          break;
      }
    }
  }

  fmtassert(buf, sizeof(buf), file, line, function, failedexpr);
  if (diagassert_flags & DIAGASSERT_STDERR) {
    char ebuf[1024];
    int l = snprintf_ss(ebuf, sizeof(ebuf), "%s: %s\n", getprogname(), buf);
    if (l == -1)
      abort();
    (void)write(STDERR_FILENO, ebuf, (size_t)l);
  }
  if (diagassert_flags & DIAGASSERT_ABORT)
    abort();
}
