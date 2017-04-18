#include <common.h>
#include <limits.h>
#include <console.h>
#include <linker_set.h>

static console_t *cn;

static void dummy_init(console_t *dev __unused) {
}
static void dummy_putc(console_t *dev __unused, int c __unused) {
}
static int dummy_getc(console_t *dev __unused) {
  return 0;
}

CONSOLE_DEFINE(dummy, -10);

void cn_init() {
  SET_DECLARE(cn_table, console_t);
  int prio = INT_MIN;
  console_t **ptr;
  SET_FOREACH(ptr, cn_table) {
    console_t *cn_ = *ptr;
    cn_->cn_init(cn_);
    if (prio < cn_->cn_prio) {
      prio = cn_->cn_prio;
      cn = cn_;
    }
  }
}

void cn_putc(int c) {
  cn->cn_putc(cn, c);
}

int cn_getc() {
  return cn->cn_getc(cn);
}

int cn_puts(const char *str) {
  int n = 0;
  while (*str) {
    cn_putc(*str++);
    n++;
  }
  cn_putc('\n');
  return n + 1;
}

int cn_write(const char *str, unsigned n) {
  while (n--)
    cn_putc(*str++);
  return n;
}
