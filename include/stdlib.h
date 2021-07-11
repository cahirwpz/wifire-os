/*	$NetBSD: stdlib.h,v 1.121 2019/01/05 09:16:46 maya Exp $	*/

/*-
 * Copyright (c) 1990, 1993
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
 *
 *	@(#)stdlib.h	8.5 (Berkeley) 5/19/95
 */

#ifndef _STDLIB_H_
#define _STDLIB_H_

#include <sys/types.h>
#include <_locale.h>

typedef struct {
  int quot; /* quotient */
  int rem;  /* remainder */
} div_t;

typedef struct {
  long quot; /* quotient */
  long rem;  /* remainder */
} ldiv_t;

#define EXIT_FAILURE 1
#define EXIT_SUCCESS 0

#define RAND_MAX 0x7fffffff

__BEGIN_DECLS
__noreturn void _Exit(int);
__noreturn void abort(void);
__constfunc int abs(int);
int atexit(void (*)(void));
double atof(const char *);
int atoi(const char *);
long atol(const char *);
#ifndef __BSEARCH_DECLARED
#define __BSEARCH_DECLARED
/* also in search.h */
void *bsearch(const void *, const void *, size_t, size_t,
              int (*)(const void *, const void *));
#endif /* __BSEARCH_DECLARED */
void *calloc(size_t, size_t);
__noreturn void exit(int);
void free(void *);
char *getenv(const char *);
int putenv(char *);
void *malloc(size_t);
void qsort(void *, size_t, size_t, int (*)(const void *, const void *));
int rand(void);
void *realloc(void *, size_t);
void *reallocarray(void *, size_t, size_t);
int reallocarr(void *, size_t, size_t);
void srand(unsigned);
double strtod(const char *__restrict, char **__restrict);
float strtof(const char *__restrict, char **__restrict);
long double strtold(const char *__restrict, char **__restrict);
long strtol(const char *__restrict, char **__restrict, int);
long long int strtoll(const char *__restrict, char **__restrict, int);
unsigned long strtoul(const char *__restrict, char **__restrict, int);
unsigned long long int strtoull(const char *__restrict, char **__restrict, int);
int system(const char *);

const char *getprogname(void) __constfunc;
void setprogname(const char *);

quad_t qabs(quad_t);
quad_t strtoq(const char *__restrict, char **__restrict, int);
u_quad_t strtouq(const char *__restrict, char **__restrict, int);

long long strsuftoll(const char *, const char *, long long, long long);
long long strsuftollx(const char *, const char *, long long, long long, char *,
                      size_t);

/* The Open Group Base Specifications, Issue 6; IEEE Std 1003.1-2001 (POSIX) */
int setenv(const char *, const char *, int);
int unsetenv(const char *);

int posix_memalign(void **, size_t, size_t);

/*
 * X/Open Portability Guide >= Issue 4 Version 2
 */

long random(void);
void srandom(unsigned int);

char *mkdtemp(char *);
int mkstemp(char *);
char *mktemp(char *);

double strtod_l(const char *__restrict, char **__restrict, locale_t);
float strtof_l(const char *__restrict, char **__restrict, locale_t);
long double strtold_l(const char *__restrict, char **__restrict, locale_t);
long strtol_l(const char *__restrict, char **__restrict, int, locale_t);
unsigned long strtoul_l(const char *__restrict, char **__restrict, int,
                        locale_t);

int mblen(const char *, size_t);
int mblen_l(const char *, size_t, locale_t);
size_t mbstowcs(wchar_t *__restrict, const char *__restrict, size_t);
size_t mbstowcs_l(wchar_t *__restrict, const char *__restrict, size_t,
                  locale_t);
int wctomb(char *, wchar_t);
int wctomb_l(char *, wchar_t, locale_t);
int mbtowc(wchar_t *__restrict, const char *__restrict, size_t);
int mbtowc_l(wchar_t *__restrict, const char *__restrict, size_t, locale_t);
size_t wcstombs(char *__restrict, const wchar_t *__restrict, size_t);
size_t wcstombs_l(char *__restrict, const wchar_t *__restrict, size_t,
                  locale_t);

/*
 * Extensions made by POSIX relative to C.
 */
char *realpath(const char *__restrict, char *__restrict);
int grantpt(int);
int unlockpt(int);
char *ptsname(int);
int posix_openpt(int flags);

/*
 * Implementation-defined extensions
 */
int ptsname_r(int fildes, char *buf, size_t buflen);

#define alloca(size) __builtin_alloca(size)

int mergesort(void *, size_t, size_t, int (*)(const void *, const void *));

char *getbsize(int *, long *);

void mi_vector_hash(const void *__restrict, size_t, uint32_t, uint32_t[3]);

#define HN_DECIMAL 0x01
#define HN_NOSPACE 0x02
#define HN_B 0x04
#define HN_DIVISOR_1000 0x08

#define HN_GETSCALE 0x10
#define HN_AUTOSCALE 0x20

int humanize_number(char *, size_t, int64_t, const char *, int, int);
int dehumanize_number(const char *, int64_t *);

__END_DECLS

#endif /* !_STDLIB_H_ */
