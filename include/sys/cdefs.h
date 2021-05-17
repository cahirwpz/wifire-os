#ifndef _SYS_CDEFS_H_
#define _SYS_CDEFS_H_

#include <machine/cdefs.h>

/*
 * To be used when an empty body is required like:
 *
 * #ifdef DEBUG
 * # define dprintf(a) printf(a)
 * #else
 * # define dprintf(a) __nothing
 * #endif
 *
 * We use ((void)0) instead of do {} while (0) so that it
 * works on , expressions.
 */
#define __nothing (/*LINTED*/ (void)0)

#ifndef __BEGIN_DECLS
#ifdef __cplusplus
#define __BEGIN_DECLS extern "C" {
#define __END_DECLS }
#define __static_cast(x, y) static_cast<x>(y)
#else
#define __BEGIN_DECLS
#define __END_DECLS
#define __static_cast(x, y) ((x)(y))
#endif
#endif

#define __dso_public __attribute__((__visibility__("default")))
#define __dso_hidden __attribute__((__visibility__("hidden")))

/* Generic preprocessor macros */
#define __STRING(x) #x
#define __CONCAT1(x, y) x##y
#define __CONCAT(x, y) __CONCAT1(x, y)
#define __UNIQUE(x) __CONCAT(x, __LINE__)

#define __USE(a) ((void)(a))

/* Check if source code is compiled with giver GCC version. */
#define __GNUC_PREREQ__(x, y)                                                  \
  ((__GNUC__ == (x) && __GNUC_MINOR__ >= (y)) || (__GNUC__ > (x)))

/* Wrapper for various GCC attributes */
#define __nonnull(x) __attribute__((__nonnull__(x)))
#define __section(s) __attribute__((__section__(s)))
#ifndef __unused
#define __unused __attribute__((__unused__))
#endif
#define __used __attribute__((__used__))
#define __always_inline __attribute__((always_inline))
#define __returns_twice __attribute__((__returns_twice__))
#define __noreturn __attribute__((__noreturn__))
#define __aligned(x) __attribute__((__aligned__(x)))
#define __noinline __attribute__((noinline))
#define __packed __attribute__((__packed__))
#define __warn_unused __attribute__((warn_unused_result));
#define __unreachable() __builtin_unreachable()
#define __pure __attribute__((__pure__))
#define __constfunc __attribute__((__const__))
#define __alias(x) __attribute__((alias(#x)))
#define __cleanup(func) __attribute__((__cleanup__(func)))
#define __caller(x) (__builtin_return_address(x) - 8)
#define __likely(x) __builtin_expect((x), 1)
#define __unlikely(x) __builtin_expect((x), 0)
#define __restrict restrict
#ifdef __mips__
#define __long_call __attribute__((long_call))
#else
#define __long_call
#endif
#define __transparent_union __attribute__((__transparent_union__))
#define __no_sanitize                                                          \
  __attribute__((no_sanitize("address", "thread", "undefined")))
#define __no_instrument_function __attribute__((no_instrument_function))
#define __no_instrument_kgprof __no_instrument_function

/* Clang introduces support for the fallthrough attribute in C2x. */
#ifdef __clang__
#define __fallthrough
#else
#define __fallthrough __attribute__((fallthrough))
#endif

/* Static assertion (introduced by C11 standard) */
#if !defined(static_assert) && !defined(__cplusplus)
#define static_assert _Static_assert
#endif

/*
 * Compiler-dependent macros to declare that functions take printf-like
 * or scanf-like arguments.  They are null except for versions of gcc
 * that are known to support the features properly (old versions of gcc-2
 * didn't permit keeping the keywords out of the application namespace).
 */
#define __printflike(fmtarg, firstvararg)                                      \
  __attribute__((__format__(__printf__, fmtarg, firstvararg)))
#define __scanflike(fmtarg, firstvararg)                                       \
  __attribute__((__format__(__scanf__, fmtarg, firstvararg)))
#define __format_arg(fmtarg) __attribute__((__format_arg__(fmtarg)))

#ifndef __syslog_attribute__
#define __syslog__ __printf__
#endif
#define __sysloglike(fmtarg, firstvararg)                                      \
  __attribute__((__format__(__syslog__, fmtarg, firstvararg)))

#define __strong_alias(alias, sym)                                             \
  extern __typeof(alias) alias __attribute__((__alias__(#sym)))
#define __weak_alias(alias, sym)                                               \
  __strong_alias(alias, sym) __attribute__((__weak__))

/*
 * The following macro is used to remove const cast-away warnings
 * from gcc -Wcast-qual; it should be used with caution because it
 * can hide valid errors; in particular most valid uses are in
 * situations where the API requires it, not to cast away string
 * constants. We don't use *intptr_t on purpose here and we are
 * explicit about unsigned long so that we don't have additional
 * dependencies.
 */
#define __UNCONST(a) ((void *)(unsigned long)(const void *)(a))

/*
 * The following macro is used to remove the volatile cast-away warnings
 * from gcc -Wcast-qual; as above it should be used with caution
 * because it can hide valid errors or warnings.  Valid uses include
 * making it possible to pass a volatile pointer to memset().
 * For the same reasons as above, we use unsigned long and not intptr_t.
 */
#define __UNVOLATILE(a) ((void *)(unsigned long)(volatile void *)(a))

/*
 * GNU C version 2.96 adds explicit branch prediction so that
 * the CPU back-end can hint the processor and also so that
 * code blocks can be reordered such that the predicted path
 * sees a more linear flow, thus improving cache behavior, etc.
 *
 * The following two macros provide us with a way to use this
 * compiler feature.  Use __predict_true() if you expect the expression
 * to evaluate to true, and __predict_false() if you expect the
 * expression to evaluate to false.
 *
 * A few notes about usage:
 *
 *       * Generally, __predict_false() error condition checks(unless
 *         you have some _strong_ reason to do otherwise, in which case
 *         document it), and/or __predict_true() `no-error' condition
 *         checks, assuming you want to optimize for the no-error case.
 *
 *       * Other than that, if you don't know the likelihood of atest
 *         succeeding from empirical or other `hard' evidence, don't
 *         make predictions.
 *
 *       * These are meant to be used in places that are run `a lot'.
 *         It is wasteful to make predictions in code that is run
 *         seldomly (e.g. at subsystem initialization time) as the
 *         basic block reordering that this affects can often generate
 *         larger code.
 */
#define __predict_true(exp) __builtin_expect((exp) != 0, 1)
#define __predict_false(exp) __builtin_expect((exp) != 0, 0)

/*
 * Return the natural alignment in bytes for the given type
 */
#define __alignof(__t) __alignof__(__t)

/*
 * Return the number of elements in a statically-allocated array __x.
 */
#define __arraycount(__x) (sizeof(__x) / sizeof(__x[0]))

#endif /* !_SYS_CDEFS_H */
