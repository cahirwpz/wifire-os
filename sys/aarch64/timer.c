#define KL_LOG KL_TIME
#include <sys/klog.h>
#include <sys/timer.h>
#include <aarch64/armreg.h>
#include <sys/interrupt.h>
#include <sys/bus.h>
#include <sys/devclass.h>

#define CNTCTL_ENABLE 1
#define CNTCTL_DISABLE 0

typedef struct arm_timer_state {
  resource_t *irq_res;
  timer_t timer;
  uint64_t step;
} arm_timer_state_t;

static int arm_timer_start(timer_t *tm, unsigned flags __unused,
                           const bintime_t start __unused,
                           const bintime_t period) {
  arm_timer_state_t *state = ((device_t *)tm->tm_priv)->state;
  state->step = bintime_mul(period, tm->tm_frequency).sec;

  WITH_INTR_DISABLED {
    uint64_t count = READ_SPECIALREG(cntpct_el0);
    WRITE_SPECIALREG(cntp_cval_el0, count + state->step);
    WRITE_SPECIALREG(cntp_ctl_el0, CNTCTL_ENABLE);
  }

  return 0;
}

static int arm_timer_stop(timer_t *tm) {
  WRITE_SPECIALREG(cntp_ctl_el0, CNTCTL_DISABLE);
  return 0;
}

static bintime_t arm_timer_gettime(timer_t *tm) {
  uint64_t count = READ_SPECIALREG(cntpct_el0);
  bintime_t res = bintime_mul(tm->tm_min_period, (uint32_t)count);
  bintime_t high_bits = bintime_mul(tm->tm_min_period, (uint32_t)(count >> 32));
  bintime_add_frac(&res, (high_bits.frac << 32));
  res.sec += (high_bits.sec << 32) + (high_bits.frac >> 32);
  return res;
}

static intr_filter_t arm_timer_intr(void *data /* device_t* */) {
  arm_timer_state_t *state = ((device_t *)data)->state;

  tm_trigger(&state->timer);

  /*
   * https://developer.arm.com/docs/ddi0595/h/aarch64-system-registers/cntp_cval_el0
   */
  uint64_t prev = READ_SPECIALREG(cntp_cval_el0);
  WRITE_SPECIALREG(cntp_cval_el0, prev + state->step);

  return IF_FILTERED;
}

static int arm_timer_probe(device_t *dev) {
  /* (cahir) so far we don't have better way to associate driver with device for
   * buses which do not automatically enumerate their children. */
  return (dev->unit == 0);
}

static int arm_timer_attach(device_t *dev) {
  arm_timer_state_t *state = dev->state;

  uint64_t freq = READ_SPECIALREG(cntfrq_el0);

  /* Save link to timer device. */
  state->timer = (timer_t){
    .tm_name = "arm-cpu-timer",
    .tm_flags = TMF_PERIODIC,
    .tm_quality = 0,
    .tm_start = arm_timer_start,
    .tm_stop = arm_timer_stop,
    .tm_gettime = arm_timer_gettime,
    .tm_priv = dev,
    .tm_frequency = freq,
    .tm_min_period = HZ2BT(freq),
    .tm_max_period = bintime_mul(HZ2BT(freq), 1LL << 30),
  };

  state->irq_res = device_take_irq(dev, 0, RF_ACTIVE);

  tm_register(&state->timer);

  bus_intr_setup(dev, state->irq_res, arm_timer_intr, NULL, dev,
                 "ARM CPU timer");

  return 0;
}

static driver_t arm_timer = {
  .desc = "ARM CPU timer driver",
  .size = sizeof(arm_timer_state_t),
  .pass = FIRST_PASS,
  .probe = arm_timer_probe,
  .attach = arm_timer_attach,
};

DEVCLASS_ENTRY(root, arm_timer);
