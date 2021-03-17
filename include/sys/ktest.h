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

typedef enum test_result {
  KTEST_SUCCESS = 0,
  KTEST_FAILURE = 1,
} test_result_t;

typedef enum test_flag {
  /* Indicates that a test enters usermode. */
  KTEST_FLAG_USERMODE = 1,
  /* Excludes the test from being run in auto mode. This flag is only useful for
   * temporarily marking some tests while debugging the testing framework. */
  KTEST_FLAG_BROKEN = 2,
  /* Marks that the test wishes to receive a random integer as an argument. */
  KTEST_FLAG_RANDINT = 4,
} test_flag_t;

typedef struct {
  const char *test_name;
  int (*test_func)(void);
  test_flag_t flags;
  uint32_t randint_max;
} test_entry_t;

__noreturn void ktest_main(const char *test);

#define KTEST_ADD(name, func, flags)                                           \
  test_entry_t name##_test = {#name, func, flags, 0};                          \
  SET_ENTRY(tests, name##_test);

#define KTEST_ADD_RANDINT(name, func, flags, max)                              \
  test_entry_t name##_test = {#name, func, flags | KTEST_FLAG_RANDINT, max};   \
  SET_ENTRY(tests, name##_test);

/* This function is called both by run_test, as well as ktest_assert.
 * It displays some troubleshooting info about the failing test. */
void ktest_log_failure(void);

/*! \brief Attempt to load word from memory without crashing the kernel. */
bool try_load_word(unsigned *ptr, unsigned *val_p);

/*! \brief Attempt to store word to memory without crashing the kernel. */
bool try_store_word(unsigned *ptr, unsigned val);

/*! \brief Memory pool used by tests. */
KMALLOC_DECLARE(M_TEST);

#endif /* !_SYS_KTEST_H_ */
