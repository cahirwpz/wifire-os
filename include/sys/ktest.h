#ifndef _SYS_KTEST_H_
#define _SYS_KTEST_H_

#ifndef KL_LOG
#define KL_LOG KL_TEST
#endif

#include <sys/linker_set.h>
#include <sys/libkern.h>
#include <sys/malloc.h>
#include <stdbool.h>

#define KTEST_NAME_MAX 50

#define KTEST_SUCCESS 0
#define KTEST_FAILURE 1

/* Signifies that a test does not return. */
#define KTEST_FLAG_NORETURN 0x01
/* Signifies that a test irreversibly breaks internal kernel state, and any
   further test done without restarting the kernel will be inconclusive. */
#define KTEST_FLAG_DIRTY 0x02
/* Indicates that a test enters usermode. */
#define KTEST_FLAG_USERMODE 0x04
/* Excludes the test from being run in auto mode. This flag is only useful for
   temporarily marking some tests while debugging the testing framework. */
#define KTEST_FLAG_BROKEN 0x08
/* Marks that the test wishes to receive a random integer as an argument. */
#define KTEST_FLAG_RANDINT 0x10

typedef struct {
  const char test_name[KTEST_NAME_MAX];
  int (*test_func)(void);
  uint32_t flags;
  uint32_t randint_max;
} test_entry_t;

__noreturn void ktest_main(const char *test);

#define KTEST_ADD(name, func, flags)                                           \
  test_entry_t name##_test = {#name, func, flags, 0};                          \
  SET_ENTRY(tests, name##_test);

#define KTEST_ADD_RANDINT(name, func, flags, max)                              \
  test_entry_t name##_test = {#name, func, flags | KTEST_FLAG_RANDINT, max};   \
  SET_ENTRY(tests, name##_test);

/* These are canonical result messages printed to standard output / UART. A
 * script running the kernel may want to grep for them. */
#define TEST_PASSED_STRING "[TEST PASSED]\n"
#define TEST_FAILED_STRING "[TEST FAILED]\n"
#define TEST_TIMEOUT_STRING "[TEST TIMEOUT]\n"

/* This function is called both by run_test, as well as ktest_assert.
 * It displays some troubleshooting info about the failing test. */
void ktest_failure_hook(void);

/*! \brief Attempt to load word from memory without crashing the kernel. */
bool try_load_word(unsigned *ptr, unsigned *val_p);

/*! \brief Attempt to store word to memory without crashing the kernel. */
bool try_store_word(unsigned *ptr, unsigned val);

/*! \brief Memory pool used by tests. */
KMALLOC_DECLARE(M_TEST);

#endif /* !_SYS_KTEST_H_ */
