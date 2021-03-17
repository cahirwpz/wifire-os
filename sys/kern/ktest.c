#define KL_LOG KL_TEST
#include <sys/klog.h>
#include <sys/mimiker.h>
#include <sys/kenv.h>
#include <sys/ktest.h>
#include <sys/malloc.h>
#include <sys/libkern.h>
#include <sys/interrupt.h>

#define KTEST_MAX_NO 1024

/* Linker set that stores all kernel tests. */
SET_DECLARE(tests, test_entry_t);
/* Stores currently running test data. */
static test_entry_t *current_test = NULL;
/* A null-terminated array of pointers to the tested test list. */
static test_entry_t *autorun_tests[KTEST_MAX_NO] = {NULL};
/* Memory pool used by tests. */
KMALLOC_DEFINE(M_TEST, "test framework");

/* The initial seed, as set from command-line. */
static unsigned ktest_seed = 0;
static unsigned ktest_repeat = 1; /* Number of repetitions of each test. */
static unsigned seed = 0;         /* Current seed */

void ktest_log_failure(void) {
  if (current_test == NULL)
    return;

  klog("Test \"%s\" failed!", current_test->test_name);
  if (autorun_tests[0])
    klog("Run `launch -d test=all seed=%u repeat=%u` to reproduce.", ktest_seed,
         ktest_repeat);
}

static __noreturn void ktest_failure(void) {
  ktest_log_failure();
  panic("Test run failed!");
}

static __noreturn void ktest_success(void) {
  panic("Test run finished!");
}

static test_entry_t *find_test(const char *test, size_t len) {
  test_entry_t **ptr;
  SET_FOREACH (ptr, tests) {
    if (strlen((*ptr)->test_name) == len &&
        strncmp((*ptr)->test_name, test, len) == 0) {
      return *ptr;
    }
  }
  return NULL;
}

typedef int (*test_func_t)(unsigned);

static void run_test(test_entry_t *t) {
  current_test = t;

  klog("Running test \"%s\".", current_test->test_name);

  test_func_t test_fn = (void *)t->test_func;
  int randint = 0;
  if (t->flags & KTEST_FLAG_RANDINT) {
    /* NOTE: Numbers generated here will be the same on each run, since test are
       started in a deterministic order. This is not a bug! In fact, it allows
       to reproduce test cases easily, just by reusing the seed.*/
    /* TODO: Low discrepancy sampling? */
    randint = rand_r(&seed) % t->randint_max;
  }

  if (test_fn(randint) == KTEST_FAILURE)
    ktest_failure();

  current_test = NULL;
}

static inline int test_is_autorunnable(test_entry_t *t) {
  return !(t->flags & KTEST_FLAG_BROKEN);
}

static int test_name_compare(const void *a_, const void *b_) {
  const test_entry_t *a = *(test_entry_t **)a_;
  const test_entry_t *b = *(test_entry_t **)b_;
  return strcmp(a->test_name, b->test_name);
}

static void run_all_tests(void) {
  /* Count the number of tests that may be run in an arbitrary order. */
  unsigned n = 0;
  test_entry_t **ptr;
  SET_FOREACH (ptr, tests) {
    if (test_is_autorunnable(*ptr))
      n++;
  }

  int total_tests = n * ktest_repeat;
  /* Check if there is enough kernel memory available for ktest framework.
   * If not then please increase KTEST_MAX_NO manually. */
  assert(total_tests <= KTEST_MAX_NO);

  /* Collect test pointers. */
  int i = 0;
  SET_FOREACH (ptr, tests) {
    if (test_is_autorunnable(*ptr))
      for (unsigned r = 0; r < ktest_repeat; r++)
        autorun_tests[i++] = *ptr;
  }
  autorun_tests[i] = NULL;

  /* Sort tests alphabetically by name, so that shuffling may be deterministic
   * and not affected by build/link order. */
  qsort(autorun_tests, total_tests, sizeof(test_entry_t *), test_name_compare);

  if (ktest_seed != 0) {
    /* Initialize LCG with seed.*/
    seed = ktest_seed;
    /* Yates-Fisher shuffle. */
    for (i = 0; i <= total_tests - 2; i++) {
      int j = i + rand_r(&seed) % (total_tests - i);
      test_entry_t *swap = autorun_tests[i];
      autorun_tests[i] = autorun_tests[j];
      autorun_tests[j] = swap;
    }
  }

  for (i = 0; i < total_tests; i++)
    run_test(autorun_tests[i]);
}

/*
 * Run the tests specified in the test string.
 * All tests except for the last one must be autorunnable.
 * If the last test is non-autorunnable, it will be executed once
 * regardless of the value of ktest_repeat.
 * All autorunnable tests with one name are executed ktest_repeat times
 * before moving on to the next test name.
 */
static void run_specified_tests(const char *test) {
  const char *cur = test;
  while (1) {
    int len = strcspn(cur, ","); /* Find first comma or end of string. */
    test_entry_t *test = find_test(cur, len);
    if (!test)
      panic("Test %.*s not found.", len, cur);
    int is_last = cur[len] == '\0';
    assert(test_is_autorunnable(test));
    for (unsigned r = 0; r < ktest_repeat; r++)
      run_test(test);
    if (is_last)
      break;        /* This was the last test name. */
    cur += len + 1; /* Skip comma. */
  }
}

__noreturn void ktest_main(const char *test) {
  /* Start by gathering command-line arguments. */
  const char *seed_str = kenv_get("seed");
  const char *repeat_str = kenv_get("repeat");
  if (seed_str)
    ktest_seed = strtoul(seed_str, NULL, 10);
  if (repeat_str)
    ktest_repeat = strtoul(repeat_str, NULL, 10);
  if (strncmp(test, "all", 3) == 0) {
    run_all_tests();
  } else {
    run_specified_tests(test);
  }

  /* If we've managed to get here, it means all tests passed with no issues. */
  ktest_success();
}
