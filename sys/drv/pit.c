/* Programmable Interval Timer (PIT) driver for Intel 8254 */
#include <sys/klog.h>
#include <dev/i8253reg.h>
#include <dev/isareg.h>
#include <sys/bus.h>
#include <sys/timer.h>
#include <sys/devclass.h>

typedef struct pit_state {
  resource_t *regs;
  resource_t *irq_res;
  timer_t timer;
  bool noticed_overflow; /* noticed and handled the counter overflow */
  uint16_t period_ticks; /* number of PIT ticks in full period */
  /* values since last counter read */
  uint16_t prev_ticks16; /* number of ticks */
  /* values since initialization */
  uint32_t ticks; /* number of ticks modulo TIMER_FREQ*/
  uint32_t sec;   /* seconds */
} pit_state_t;

#define inb(addr) bus_read_1(pit->regs, (addr))
#define outb(addr, val) bus_write_1(pit->regs, (addr), (val))

static inline void pit_set_frequency(pit_state_t *pit) {
  outb(TIMER_MODE, TIMER_SEL0 | TIMER_16BIT | TIMER_RATEGEN);
  outb(TIMER_CNTR0, pit->period_ticks & 0xff);
  outb(TIMER_CNTR0, pit->period_ticks >> 8);
}

static inline uint16_t pit_get_counter(pit_state_t *pit) {
  uint16_t count = 0;
  outb(TIMER_MODE, TIMER_SEL0 | TIMER_LATCH);
  count |= inb(TIMER_CNTR0);
  count |= inb(TIMER_CNTR0) << 8;

  /* PIT counter counts from n to 1, we make it ascending from 0 to n-1*/
  return pit->period_ticks - count;
}

static inline void pit_incr_ticks(pit_state_t *pit, uint16_t ticks) {
  pit->ticks += ticks;
  if (pit->ticks >= TIMER_FREQ) {
    pit->ticks -= TIMER_FREQ;
    pit->sec++;
  }
}

static void pit_update_time(pit_state_t *pit) {
  assert(intr_disabled());
  uint32_t last_ticks = pit->ticks;
  uint32_t last_sec = pit->sec;
  uint16_t now_ticks16 = pit_get_counter(pit);
  uint16_t ticks_passed = now_ticks16 - pit->prev_ticks16;
  if (pit->prev_ticks16 > now_ticks16) {
    pit->noticed_overflow = true;
    ticks_passed += pit->period_ticks;
  }

  /* We want to keep the last read counter value to detect possible future
   * overflows of our counter */
  pit->prev_ticks16 = now_ticks16;

  pit_incr_ticks(pit, ticks_passed);
  assert(last_sec < pit->sec ||
         (last_sec == pit->sec && last_ticks < pit->ticks));
  assert(pit->ticks < TIMER_FREQ);
}

static intr_filter_t pit_intr(void *data) {
  pit_state_t *pit = data;

  /* XXX: It's still possible for periods to be lost.
   * For example disabling interrupts for the whole period
   * without calling pit_gettime will lose period_ticks.
   * It is also possible that time suddenly jumps by period_ticks
   * due to the fact that pit_update_time() can't detect an overflow if
   * the current counter value is greater than the previous one, while
   * pit_intr() can thanks to the noticed_overflow flag. */
  pit_update_time(pit);
  if (!pit->noticed_overflow)
    pit_incr_ticks(pit, pit->period_ticks);
  tm_trigger(&pit->timer);
  /* It is set here to let us know in the next interrupt if we already
   * considered the overflow */
  pit->noticed_overflow = false;
  return IF_FILTERED;
}

static device_t *device_of(timer_t *tm) {
  return tm->tm_priv;
}

static int timer_pit_start(timer_t *tm, unsigned flags, const bintime_t start,
                           const bintime_t period) {
  assert(flags & TMF_PERIODIC);
  assert(!(flags & TMF_ONESHOT));

  device_t *dev = device_of(tm);
  pit_state_t *pit = dev->state;

  uint32_t counter = bintime_mul(period, TIMER_FREQ).sec;
  /* Maximal counter value which we can store in pit timer */
  assert(counter <= 0xFFFF);

  pit->period_ticks = counter;

  pit_set_frequency(pit);

  return 0;
}

static int timer_pit_stop(timer_t *tm) {
  device_t *dev = device_of(tm);
  pit_state_t *pit = dev->state;

  outb(TIMER_MODE, TIMER_SEL0 | TIMER_16BIT | TIMER_ONESHOT);

  return 0;
}

static bintime_t timer_pit_gettime(timer_t *tm) {
  device_t *dev = device_of(tm);
  pit_state_t *pit = dev->state;
  uint32_t sec, ticks;
  WITH_INTR_DISABLED {
    pit_update_time(pit);
    sec = pit->sec;
    ticks = pit->ticks;
  }
  bintime_t bt = bintime_mul(tm->tm_min_period, ticks);
  bt.sec += sec;
  return bt;
}

static int pit_attach(device_t *dev) {
  pit_state_t *pit = dev->state;

  pit->regs = device_take_ioports(dev, 0, RF_ACTIVE);
  assert(pit->regs != NULL);

  pit->sec = 0;
  pit->ticks = 0;
  pit->prev_ticks16 = 0;
  pit->noticed_overflow = false;
  pit->irq_res = device_take_irq(dev, 0, RF_ACTIVE);

  pit->timer = (timer_t){
    .tm_name = "i8254",
    .tm_flags = TMF_PERIODIC,
    .tm_quality = 100,
    .tm_frequency = TIMER_FREQ,
    .tm_min_period = HZ2BT(TIMER_FREQ),
    .tm_max_period = bintime_mul(HZ2BT(TIMER_FREQ), 65536),
    .tm_start = timer_pit_start,
    .tm_stop = timer_pit_stop,
    .tm_gettime = timer_pit_gettime,
    .tm_priv = dev,
  };

  timer_pit_stop(&pit->timer);

  tm_register(&pit->timer);

  bus_intr_setup(dev, pit->irq_res, pit_intr, NULL, pit, "i8254 timer");

  return 0;
}

static int pit_probe(device_t *dev) {
  return dev->unit == 3; /* XXX: unit 3 assigned by gt_pci */
}

static driver_t pit_driver = {
  .desc = "i8254 PIT driver",
  .size = sizeof(pit_state_t),
  .pass = FIRST_PASS,
  .attach = pit_attach,
  .probe = pit_probe,
};

DEVCLASS_ENTRY(isa, pit_driver);
