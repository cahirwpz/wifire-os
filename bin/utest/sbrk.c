#include "utest.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#ifdef __mips__
#define TOO_MUCH 0x80000000
#endif

#ifdef __aarch64__
#define TOO_MUCH 0x4000000000000000L
#endif

#ifdef __amd64__
#define TOO_MUCH 0x0 /* TODO(MichalBlk): fix me. */
#endif

static void *sbrk_orig = NULL;

/* Note that sbrk returns old brk value */

static void sbrk_good(void) {
  /* Initial sbrk */
  char *a1 = sbrk(10);
  /* Test write access. */
  memset(a1, 1, 10);
  /* Expand sbrk a little bit. */
  char *a2 = sbrk(40);
  assert(a2 == a1 + 10);
  /* And again. */
  char *a3 = sbrk(50);
  assert(a3 == a2 + 40);

  /* Shrink to a2 */
  char *a4 = sbrk(-90);
  assert((char *)sbrk(0) == a2);
  /* And back to a4 */
  sbrk(90);
  assert((char *)sbrk(0) == a4);

  /* Now expand it a lot, much more than page size. */
  char *a5 = sbrk(0x5000);
  assert(a5 == a3 + 50);
  /* Test write access. */
  memset(a5, -1, 0x5000);
  /* See that previous data is unmodified. */
  assert(*(a1 + 5) == 1);

  /* Free all you got */
  sbrk(-1 * (10 + 40 + 50 + 0x5000));
  char *a6 = sbrk(0);
  assert(a1 == a6);
}

static void sbrk_bad(void) {
  void *b0 = sbrk(0);
  /* Attempt to move sbrk before original start. */
  sbrk((sbrk_orig - b0) - 0x10000);
  assert(errno == EINVAL);
  char *b1 = sbrk(0);
  assert(b1 == (char *)b0);

  /* Attempt to move sbrk to far */
  sbrk(TOO_MUCH);
  assert(errno == ENOMEM);
  char *b2 = sbrk(0);
  assert(b2 == b1);
}

/* Causes SIGSEGV, don't call it here */
int test_sbrk_sigsegv(void) {
  /* Make sure memory just above sbrk has just been used and freed */
  void *unaligned = sbrk(0);
  /* Align to page size */
  sbrk(((intptr_t)unaligned + 0xfff) & -0x1000);

  void *ptr = sbrk(0x2000);
  sbrk(-0x2000);

  /* Try to access freed memory. It should raise SIGSEGV */
  int data = *((volatile int *)(ptr + 0x1000));
  (void)data;
  return 1;
}

int test_sbrk(void) {
  sbrk_orig = sbrk(0);
  assert(sbrk_orig != NULL);

  sbrk_bad();
  sbrk_good();
  sbrk_bad();
  return 0;
}
