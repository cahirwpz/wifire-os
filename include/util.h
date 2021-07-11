#ifndef _UTIL_H_
#define _UTIL_H_

#include <sys/types.h>

__BEGIN_DECLS
struct termios;
struct winsize;

char *strpct(char *, size_t, uintmax_t, uintmax_t, size_t);
char *strspct(char *, size_t, intmax_t, intmax_t, size_t);
time_t parsedate(const char *, const time_t *, const int *);

void logwtmp(const char *, const char *, const char *);

int openpty(int *, int *, char *, struct termios *, struct winsize *);

int login_tty(int);

/* Error checked functions */
void (*esetfunc(void (*)(int, const char *, ...)))(int, const char *, ...);
size_t estrlcpy(char *, const char *, size_t);
size_t estrlcat(char *, const char *, size_t);
char *estrdup(const char *);
char *estrndup(const char *, size_t);
intmax_t estrtoi(const char *, int, intmax_t, intmax_t);
uintmax_t estrtou(const char *, int, uintmax_t, uintmax_t);
void *ecalloc(size_t, size_t);
void *emalloc(size_t);
void *erealloc(void *, size_t);
void ereallocarr(void *, size_t, size_t);
int easprintf(char **__restrict, const char *__restrict, ...)
  __printflike(2, 3);
int evasprintf(char **__restrict, const char *__restrict, __va_list)
  __printflike(2, 0);

int raise_default_signal(int sig);
__END_DECLS

#endif /* !_UTIL_H_ */
