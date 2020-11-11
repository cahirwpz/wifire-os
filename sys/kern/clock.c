#define KL_LOG KL_TIME
#include <sys/callout.h>
#include <sys/sched.h>
#include <sys/mimiker.h>
#include <sys/klog.h>
#include <sys/timer.h>

static systime_t now = 0;
static timer_t *clock = NULL;

systime_t getsystime(void) {
  return now;
}

static void clock_cb(timer_t *tm, void *arg) {
  bintime_t bin = binuptime();
  now = bt2st(&bin);
  callout_process(now);
  sched_clock();
}

void init_clock(void) {
  clock = tm_reserve(NULL, TMF_PERIODIC);
  if (clock == NULL)
    panic("Missing suitable timer for maintenance of system clock!");
  tm_init(clock, clock_cb, NULL);
  if (tm_start(clock, TMF_PERIODIC | TMF_TIMESOURCE, (bintime_t){},
               HZ2BT(CLK_TCK)))
    panic("Failed to start system clock!");
  klog("System clock uses \'%s\' hardware timer.", clock->tm_name);

  tm_calibrate();
}
