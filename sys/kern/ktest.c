#include <sys/mimiker.h>
#include <sys/kenv.h>
#include <sys/ktest.h>
#include <sys/malloc.h>
#include <sys/libkern.h>
#include <sys/interrupt.h>
#include <sys/time.h>
#include <sys/callout.h>

#define KTEST_MAX_NO 1024
#define KTEST_FAIL(args...)                                                    \
  do {                                                                         \
    kprintf(args);                                                             \
    ktest_atomically_print_failure();                                          \
  } while (0)

/* Linker set that stores all kernel tests. */
SET_DECLARE(tests, test_entry_t);
/* Stores currently running test data. */
static test_entry_t *current_test = NULL;
/* A null-terminated array of pointers to the tested test list. */
static test_entry_t *autorun_tests[KTEST_MAX_NO] = {NULL};
/* Callout used to detect test timeout. */
static callout_t ktest_callout;
/* Memory pool used by tests. */
KMALLOC_DEFINE(M_TEST, "test framework");

/* This flag is set to 1 when a kernel test is in progress, and 0 otherwise. */
static int ktest_test_running_flag = 0;

/* The initial seed, as set from command-line. */
static unsigned ktest_seed = 0;
static unsigned ktest_repeat = 1; /* Number of repetitions of each test. */
static unsigned seed = 0;         /* Current seed */

/* If we get interrupted while printing out the [TEST_PASSED] string, the
 * monitor process might not find the pattern it's looking for. */
static void ktest_atomically_print_success(void) {
  SCOPED_INTR_DISABLED();
  kprintf(TEST_PASSED_STRING);
}

static void ktest_atomically_print_failure(void) {
  SCOPED_INTR_DISABLED();
  kprintf(TEST_FAILED_STRING);
}

static __noreturn void
ktest_print_failure_info_and_panic(bool timeout, test_entry_t *failed_test) {
  SCOPED_INTR_DISABLED();
  kprintf(timeout ? TEST_TIMEOUT_STRING : TEST_FAILED_STRING);
  if (autorun_tests[0]) {
    kprintf("%s while running multiple tests.\n",
            timeout ? "Timeout" : "Failure");
    for (test_entry_t **ptr = autorun_tests; *ptr != NULL; ptr++) {
      test_entry_t *t = *ptr;
      kprintf("  %s", t->test_name);
      if (t == failed_test) {
        kprintf("  <---- %s\n", timeout ? "TIMEOUT" : "FAILED");
        break;
      } else {
        kprintf("\n");
      }
    }
    kprintf("The seed used for this test order was: %u. Start kernel with "
            "`test=all seed=%u repeat=%u` to reproduce this test case.\n",
            ktest_seed, ktest_seed, ktest_repeat);
  } else {
    kprintf("%s while running single test: \"%s\"\n",
            timeout ? "Timeout" : "Failure", failed_test->test_name);
  }
  panic("Halting kernel on %s test.\n", timeout ? "timed out" : "failed");
}

static __noreturn void ktest_failure(void) {
  if (current_test == NULL)
    panic("current_test == NULL in ktest_failure! This is most likely a bug in "
          "the test framework!\n");
  ktest_print_failure_info_and_panic(false, current_test);
}

void ktest_failure_hook(void) {
  if (ktest_test_running_flag)
    ktest_failure();
}

static test_entry_t *find_test_by_name_with_len(const char *test, size_t len) {
  test_entry_t **ptr;
  SET_FOREACH (ptr, tests) {
    if (strlen((*ptr)->test_name) == len &&
        strncmp((*ptr)->test_name, test, len) == 0) {
      return *ptr;
    }
  }
  return NULL;
}

static __unused test_entry_t *find_test_by_name(const char *test) {
  return find_test_by_name_with_len(test, strlen(test));
}

/* The time limit for a single test in clock ticks. */
#define KTEST_SINGLE_TEST_TIMEOUT (5 * CLK_TCK) /* 5 seconds */

static void ktest_timeout(void *arg) {
  ktest_print_failure_info_and_panic(true, arg);
}

static void ktest_setup_callout(test_entry_t *t) {
  callout_setup_relative(&ktest_callout, KTEST_SINGLE_TEST_TIMEOUT,
                         ktest_timeout, t);
}

static void ktest_stop_callout(void) {
  if (!callout_stop(&ktest_callout)) {
    /* The callout has been dispatched to the callout thread, so the only thing
     * we can do is wait for it to panic. */
    while (true)
      ;
  }
}

typedef int (*test_func_t)(unsigned);

/* If the test fails, run_test will not return. */
static int run_test(test_entry_t *t) {
  /* These are messages to the user, so I intentionally use kprintf instead of
   * log. */
  kprintf("# Running test \"%s\".\n", t->test_name);
  if (t->flags & KTEST_FLAG_NORETURN)
    kprintf("WARNING: This test will never return, it is not possible to "
            "automatically verify its success.\n");
  if (t->flags & KTEST_FLAG_USERMODE)
    kprintf("WARNING: This test will enter usermode.\n");
  if (t->flags & KTEST_FLAG_DIRTY)
    kprintf("WARNING: This test will break kernel state. Kernel reboot will be "
            "required to run any other test.\n");

  current_test = t;

  int result, randint = 0;
  test_func_t f = (void *)t->test_func;
  if (t->flags & KTEST_FLAG_RANDINT) {
    randint = rand_r(&seed) % t->randint_max;
    /* NOTE: Numbers generated here will be the same on each run, since test are
       started in a deterministic order. This is not a bug! In fact, it allows
       to reproduce test cases easily, just by reusing the seed.*/
    /* TODO: Low discrepancy sampling? */
  }

  ktest_setup_callout(t);
  ktest_test_running_flag = 1;
  /* If the test function doesn't take a randint argument,
   * it will just ignore it. */
  result = f(randint);
  ktest_test_running_flag = 0;
  ktest_stop_callout();

  if (result == KTEST_FAILURE)
    ktest_failure();

  return result;
}

inline static int test_is_autorunnable(test_entry_t *t) {
  return !(t->flags & KTEST_FLAG_NORETURN) && !(t->flags & KTEST_FLAG_DIRTY) &&
         !(t->flags & KTEST_FLAG_BROKEN);
}

static int test_name_compare(const void *a_, const void *b_) {
  const test_entry_t *a = *(test_entry_t **)a_;
  const test_entry_t *b = *(test_entry_t **)b_;
  return strncmp(a->test_name, b->test_name, KTEST_NAME_MAX);
}

static void print_tests(test_entry_t **tests, int count) {
  for (int i = 0; i < count; i++)
    kprintf("  %s\n", tests[i]->test_name);
}

static void run_all_tests(void) {

  /* Count the number of tests that may be run in an arbitrary order. */
  unsigned int n = 0;
  test_entry_t **ptr;
  SET_FOREACH (ptr, tests) {
    if (test_is_autorunnable(*ptr))
      n++;
  }

  int total_tests = n * ktest_repeat;
  if (total_tests > KTEST_MAX_NO + 1) {
    KTEST_FAIL("Warning: There are more kernel tests registered than there is "
               "memory available for ktest framework. Please increase "
               "KTEST_MAX_NO.\n");
    return;
  }

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
      register test_entry_t *swap = autorun_tests[i];
      autorun_tests[i] = autorun_tests[j];
      autorun_tests[j] = swap;
    }
  }

  kprintf("Found %d automatically runnable tests.\n", n);
  kprintf("Planned test order:\n");
  print_tests(autorun_tests, total_tests);

  for (i = 0; i < total_tests; i++)
    run_test(autorun_tests[i]);

  /* If we've managed to get here, it means all tests passed with no issues. */
  ktest_atomically_print_success();

  /* As the tests are usually very verbose, for user convenience let's print out
     the order of tests once again. */
  kprintf("Test order:\n");
  print_tests(autorun_tests, total_tests);
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
    test_entry_t *t = find_test_by_name_with_len(cur, len);
    int is_last = cur[len] == '\0';
    if (t) {
      if (test_is_autorunnable(t)) {
        for (unsigned r = 0; r < ktest_repeat; r++)
          run_test(t);
      } else if (is_last) {
        /* The last test can be non-autorunnable. */
        run_test(t);
      } else {
        /* We found a non-autorunnable test in the middle of the test string. */
        KTEST_FAIL("Error: test %.*s is not autorunnable."
                   "Non-autorunnable tests can only be run as the last test.\n",
                   len, cur);
        return;
      }
    } else {
      KTEST_FAIL("Error: test %.*s not found.\n", len, cur);
      return;
    }
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
  panic("Test run finished!");
}
