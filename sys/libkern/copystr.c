/* Taken from NetBSD: sys/lib/libkern/copystr.c */

#include <sys/libkern.h>
#include <sys/errno.h>
#include <sys/mimiker.h>

/*
 * int copystr(const void *kfaddr, void *kdaddr, size_t len, size_t *done)
 *
 * Copy a NIL-terminated string, at most maxlen characters long.  Return the
 * number of characters copied (including the NIL) in *done.  If the
 * string is too long, return ENAMETOOLONG; else return 0.
 *
 * IMPORTANT!
 * This function cannot modify stack and caller-saved registers and must be
 * a leaf function. It cannot call KASAN functions and contain regular prologue
 * and epilogue that set up frame pointer. Hence it cannot be compiled with
 * KASAN and we need to compile it with -fomit-frame-pointer flag.
 *
 * If you plan to change this function look at machine dependent implementation
 * of copyerr.
 */
__no_sanitize __no_instrument_function int
copystr(const void *kfaddr, void *kdaddr, size_t len, size_t *done) {
  const char *src = kfaddr;
  char *dst = kdaddr;
  size_t i;

  for (i = 0; i < len; i++) {
    if ((*dst++ = *src++) == '\0') {
      if (done)
        *done = i + 1;
      return 0;
    }
  }

  if (done)
    *done = i;

  return ENAMETOOLONG;
}
