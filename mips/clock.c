#include <common.h>
#include <mips/config.h>
#include <mips/mips.h>
#include <mips/clock.h>
#include <interrupt.h>
#include <bus.h>
#include <callout.h>
#include <mutex.h>
#include <sched.h>
#include <sysinit.h>

/* This counter is incremented every millisecond. */
static volatile realtime_t mips_clock_ms;

static void mips_clock_intr() {
  uint32_t compare = mips32_get_c0(C0_COMPARE);
  uint32_t count = mips32_get_c0(C0_COUNT);
  int32_t diff = compare - count;

  /* Should not happen. Potentially spurious interrupt. */
  if (diff > 0)
    return;

  /* This loop is necessary, because sometimes we may miss some ticks. */
  while (diff < TICKS_PER_MS) {
    compare += TICKS_PER_MS;
    mips_clock_ms++;
    diff = compare - count;
  }

  /* Set compare register. */
  mips32_set_c0(C0_COMPARE, compare);

  clock(mips_clock_ms);
}

static INTR_HANDLER_DEFINE(mips_clock_intr_handler, NULL, mips_clock_intr, NULL,
                           "MIPS counter clock", 0);

extern device_t *rootdev;

void mips_clock_init() {
  mips32_set_c0(C0_COUNT, 0);
  mips32_set_c0(C0_COMPARE, TICKS_PER_MS);

  mips_clock_ms = 0;

  bus_intr_setup(rootdev, 7, mips_clock_intr_handler);
}

SYSINIT_ADD(mips_clock, mips_clock_init, DEPS("callout", "sched"));
