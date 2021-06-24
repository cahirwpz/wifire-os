#define KL_LOG KL_TIME
#include <sys/klog.h>
#include <mips/m32c0.h>
#include <mips/config.h>
#include <sys/bus.h>
#include <sys/devclass.h>
#include <sys/device.h>
#include <sys/interrupt.h>
#include <sys/timer.h>

typedef struct mips_timer_state {
  uint32_t sec;         /* seconds passed after timer initialization */
  uint32_t cntr_modulo; /* counter since initialization modulo its frequency */
  uint32_t period_cntr; /* number of counter ticks in a period */
  uint32_t last_count_lo;     /* used to detect counter overflow */
  volatile timercntr_t count; /* last written value of counter reg. (64 bits) */
  volatile timercntr_t compare; /* last read value of compare reg. (64 bits) */
  timer_t timer;
  resource_t *irq_res;
} mips_timer_state_t;

static intr_filter_t mips_timer_intr(void *data);
static int mips_timer_start(timer_t *tm, unsigned flags, const bintime_t start,
                            const bintime_t period);
static int mips_timer_stop(timer_t *tm);
static bintime_t mips_timer_gettime(timer_t *tm);

static uint64_t read_count(mips_timer_state_t *state) {
  SCOPED_INTR_DISABLED();
  state->count.lo = mips32_getcount();

  /* detect hardware counter overflow */
  if (state->count.lo < state->last_count_lo) {
    state->count.hi++;
  }
  state->cntr_modulo += state->count.lo - state->last_count_lo;

  if (state->cntr_modulo >= state->timer.tm_frequency) {
    state->cntr_modulo -= state->timer.tm_frequency;
    state->sec++;
  }
  assert(state->cntr_modulo < state->timer.tm_frequency);

  state->last_count_lo = state->count.lo;
  return state->count.val;
}

static int set_next_tick(mips_timer_state_t *state) {
  SCOPED_INTR_DISABLED();
  int ticks = 0;

  /* calculate next value of compare register based on timer period */
  do {
    state->compare.val += state->period_cntr;
    mips32_setcompare(state->compare.lo);
    (void)read_count(state);
    ticks++;
  } while (state->compare.val <= state->count.val);

  return ticks;
}

static intr_filter_t mips_timer_intr(void *data) {
  device_t *dev = data;
  mips_timer_state_t *state = dev->state;
  /* TODO(cahir): can we tell scheduler that clock ticked more than once? */
  (void)set_next_tick(state);
  tm_trigger(&state->timer);
  return IF_FILTERED;
}

static int mips_timer_start(timer_t *tm, unsigned flags, const bintime_t start,
                            const bintime_t period) {
  assert(flags & TMF_PERIODIC);
  assert(!(flags & TMF_ONESHOT));

  device_t *dev = tm->tm_priv;
  mips_timer_state_t *state = dev->state;
  state->period_cntr = bintime_mul(period, tm->tm_frequency).sec;
  state->compare.val = read_count(state);

  set_next_tick(state);
  return 0;
}

static inline int mips_timer_stop(timer_t *tm) {
  mips32_setcompare(0xffffffff);
  return 0;
}

static bintime_t mips_timer_gettime(timer_t *tm) {
  device_t *dev = tm->tm_priv;
  mips_timer_state_t *state = dev->state;
  uint32_t sec, ticks;
  WITH_INTR_DISABLED {
    read_count(state);
    sec = state->sec;
    ticks = state->cntr_modulo;
  }
  bintime_t bt = bintime_mul(tm->tm_min_period, ticks);
  bt.sec += sec;
  return bt;
}

static int mips_timer_probe(device_t *dev) {
  /* TODO(cahir) match device with driver on FDT basis */
  return dev->unit == 0;
}

static int mips_timer_attach(device_t *dev) {
  mips_timer_state_t *state = dev->state;

  mips32_setcount(0);

  state->sec = 0;
  state->cntr_modulo = 0;
  state->last_count_lo = 0;
  state->irq_res = device_take_irq(dev, 0, RF_ACTIVE);

  mips_timer_stop(&state->timer);

  state->timer = (timer_t){
    .tm_name = "mips-cpu-timer",
    .tm_flags = TMF_PERIODIC,
    .tm_quality = 200,
    .tm_frequency = CPU_FREQ,
    .tm_min_period = HZ2BT(CPU_FREQ),
    .tm_max_period = BINTIME(((1LL << 32) - 1) / (double)CPU_FREQ),
    .tm_start = mips_timer_start,
    .tm_stop = mips_timer_stop,
    .tm_gettime = mips_timer_gettime,
    .tm_priv = dev,
  };

  tm_register(&state->timer);

  bus_intr_setup(dev, state->irq_res, mips_timer_intr, NULL, dev,
                 "MIPS CPU timer");

  return 0;
}

static driver_t cpu_mips_timer = {
  .desc = "MIPS CPU timer driver",
  .size = sizeof(mips_timer_state_t),
  .pass = FIRST_PASS,
  .probe = mips_timer_probe,
  .attach = mips_timer_attach,
};

DEVCLASS_ENTRY(root, cpu_mips_timer);
