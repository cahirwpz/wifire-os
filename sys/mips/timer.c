#define KL_LOG KL_TIME
#include <sys/klog.h>
#include <mips/m32c0.h>
#include <mips/config.h>
#include <mips/interrupt.h>
#include <sys/interrupt.h>
#include <sys/time.h>
#include <sys/timer.h>

/* XXX Should the timer use driver framework? */

typedef union {
  /* assumes little endian order */
  struct {
    uint32_t lo;
    uint32_t hi;
  };
  uint64_t val;
} counter_t;

typedef struct mips_timer_state {
  uint32_t period_cntr;       /* number of counter ticks in a period */
  uint32_t last_count_lo;     /* used to detect counter overflow */
  volatile counter_t count;   /* last written value of counter reg. (64 bits) */
  volatile counter_t compare; /* last read value of compare reg. (64 bits) */
  intr_handler_t intr_handler;
} mips_timer_state_t;

static intr_filter_t mips_timer_intr(void *data);
static int mips_timer_start(timer_t *tm, unsigned flags, const bintime_t start,
                            const bintime_t period);
static int mips_timer_stop(timer_t *tm);
static bintime_t mips_timer_gettime(timer_t *tm);

static timer_t mips_timer;

static mips_timer_state_t mips_timer_state = {
  .intr_handler =
    INTR_HANDLER_INIT(mips_timer_intr, NULL, &mips_timer, "MIPS CPU timer", 0),
};

static timer_t mips_timer = {
  .tm_name = "mips-cpu-timer",
  .tm_flags = TMF_PERIODIC,
  .tm_frequency = CPU_FREQ,
  .tm_min_period = BINTIME(1 / (double)CPU_FREQ),
  .tm_max_period = BINTIME(((1LL << 32) - 1) / (double)CPU_FREQ),
  .tm_start = mips_timer_start,
  .tm_stop = mips_timer_stop,
  .tm_gettime = mips_timer_gettime,
  .tm_priv = &mips_timer_state,
};

static uint64_t read_count(mips_timer_state_t *state) {
  SCOPED_INTR_DISABLED();
  state->count.lo = mips32_get_c0(C0_COUNT);
  /* detect hardware counter overflow */
  if (state->count.lo < state->last_count_lo)
    state->count.hi++;
  state->last_count_lo = state->count.lo;
  return state->count.val;
}

static int set_next_tick(mips_timer_state_t *state) {
  SCOPED_INTR_DISABLED();
  int ticks = 0;

  /* calculate next value of compare register based on timer period */
  do {
    state->compare.val += state->period_cntr;
    mips32_set_c0(C0_COMPARE, state->compare.lo);
    (void)read_count(state);
    ticks++;
  } while (state->compare.val <= state->count.val);

  return ticks;
}

static mips_timer_state_t *state_of(timer_t *tm) {
  return tm->tm_priv;
}

static intr_filter_t mips_timer_intr(void *data) {
  mips_timer_state_t *state = state_of(data);
  /* TODO(cahir): can we tell scheduler that clock ticked more than once? */
  set_next_tick(state);
  tm_trigger(data);
  return IF_FILTERED;
}

static int mips_timer_start(timer_t *tm, unsigned flags, const bintime_t start,
                            const bintime_t period) {
  assert(flags & TMF_PERIODIC);
  assert(!(flags & TMF_ONESHOT));

  mips_timer_state_t *state = state_of(tm);

  state->period_cntr = bintime_mul(period, tm->tm_frequency).sec;
  state->compare.val = read_count(state);
  state->last_count_lo = state->count.lo;
  set_next_tick(state);
  mips_intr_setup(&state->intr_handler, MIPS_HWINT5);
  return 0;
}

static int mips_timer_stop(timer_t *tm) {
  mips_timer_state_t *state = state_of(tm);
  mips_intr_teardown(&state->intr_handler);
  return 0;
}

static bintime_t mips_timer_gettime(timer_t *tm) {
  uint64_t count = read_count(state_of(tm));
  uint64_t sec = count / tm->tm_frequency;
  uint32_t frac = count % tm->tm_frequency;
  bintime_t bt = bintime_mul(HZ2BT(tm->tm_frequency), frac);
  bt.sec += sec;
  return bt;
}

void init_mips_timer(void) {
  tm_register(&mips_timer);
  tm_select(&mips_timer);
}
