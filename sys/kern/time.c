#include <sys/mutex.h>
#include <sys/signal.h>
#include <sys/errno.h>
#include <sys/sleepq.h>
#include <sys/time.h>
#include <sys/callout.h>
#include <sys/proc.h>
#include <sys/klog.h>
#include <limits.h>

int do_clock_gettime(clockid_t clk, timespec_t *tp) {
  bintime_t bin;
  switch (clk) {
    case CLOCK_REALTIME:
      bin = bintime();
      break;
    case CLOCK_MONOTONIC:
      bin = binuptime();
      break;
    default:
      return EINVAL;
  }
  bt2ts(&bin, tp);
  return 0;
}

systime_t ts2hz(const timespec_t *ts) {
  if (ts->tv_sec < 0 || (ts->tv_sec == 0 && ts->tv_nsec == 0))
    return 0;

  if (ts->tv_sec > UINT_MAX / CLK_TCK)
    return UINT_MAX;

  const int pow9 = 1000000000;
  int tick = pow9 / CLK_TCK;

  /* Round up the number of ticks */
  long nsectck = (ts->tv_nsec + tick - 1) / tick;
  systime_t ticks = ts->tv_sec * CLK_TCK;

  if (ticks > UINT_MAX - nsectck - 1)
    return UINT_MAX;

  /* Add 1 for the current tick to expire */
  return ticks + nsectck + 1;
}

static bool timespec_invalid(const timespec_t *ts) {
  return ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000L || ts->tv_sec < 0;
}

static bool timeval_invalid(const timeval_t *tv) {
  return tv->tv_usec < 0 || tv->tv_usec >= 1000000 || tv->tv_sec < 0;
}

static int ts2timo(clockid_t clock_id, int flags, timespec_t *ts,
                   systime_t *timo, timespec_t *start) {
  int error;
  *timo = 0;

  if (timespec_invalid(ts) || (flags & ~TIMER_ABSTIME))
    return EINVAL;

  if ((error = do_clock_gettime(clock_id, start)))
    return error;

  if (flags & TIMER_ABSTIME)
    timespecsub(ts, start, ts);

  if ((ts->tv_sec == 0 && ts->tv_nsec == 0) || ts->tv_sec < 0)
    return ETIMEDOUT;

  *timo = ts2hz(ts);

  return 0;
}

int do_clock_nanosleep(clockid_t clk, int flags, timespec_t *rqtp,
                       timespec_t *rmtp) {
  /* rmt - remaining time, rqt - requested time, p - pointer */
  timespec_t rmt_start, rmt_end, rmt;
  systime_t timo;
  int error, error2;

  if ((error = ts2timo(clk, flags, rqtp, &timo, &rmt_start))) {
    if (error == ETIMEDOUT)
      goto timedout;
    return error;
  }

  do {
    error = sleepq_wait_timed((void *)(&rmt_start), __caller(0), timo);
    if (error == ETIMEDOUT)
      goto timedout;

    if ((error2 = do_clock_gettime(clk, &rmt_end)))
      return error2;

    if (flags == TIMER_ABSTIME) {
      timespecsub(rqtp, &rmt_end, &rmt);
    } else {
      timespecsub(&rmt_end, &rmt_start, &rmt);
      timespecsub(rqtp, &rmt, &rmt);
    }
    if (rmt.tv_sec < 0)
      timespecclear(&rmt);

    if (rmtp)
      *rmtp = rmt;
    if (error)
      return error;

    timo = ts2hz(&rmt);
  } while (timo > 0);

  return 0;

timedout:
  if (rmtp)
    rmtp->tv_sec = rmtp->tv_nsec = 0;
  return 0;
}

time_t tm2sec(tm_t *t) {
  if (t->tm_year < 70)
    return 0;

  const int32_t year_scale_s = 31536000, day_scale_s = 86400,
                hour_scale_s = 3600, min_scale_s = 60;
  time_t res = 0;
  static const int month_in_days[13] = {0,   31,  59,  90,  120, 151,
                                        181, 212, 243, 273, 304, 334};

  res += (time_t)month_in_days[t->tm_mon] * day_scale_s;
  /* Extra days from leap years which already past (EPOCH) */
  res += (time_t)((t->tm_year + 1900) / 4 - (1970) / 4) * day_scale_s;
  res -= (time_t)((t->tm_year + 1900) / 100 - (1970) / 100) * day_scale_s;
  res += (time_t)((t->tm_year + 1900) / 400 - (1970) / 400) * day_scale_s;

  /* If actual year is a leap year and the leap day passed */
  if (t->tm_mon > 1 && (t->tm_year % 4) == 0 &&
      ((t->tm_year % 100) != 0 || (t->tm_year + 1900) % 400) == 0)
    res += day_scale_s;

  /* (t.tm_mday - 1) - cause days are in range [1-31] */
  return (time_t)(t->tm_year - 70) * year_scale_s +
         (t->tm_mday - 1) * day_scale_s + t->tm_hour * hour_scale_s +
         t->tm_min * min_scale_s + t->tm_sec + res;
}

timespec_t nanotime(void) {
  timespec_t tp;
  bintime_t bt = bintime();
  bt2ts(&bt, &tp);
  return tp;
}

static systime_t tv2hz(const timeval_t *tv) {
  timespec_t ts;
  tv2ts(tv, &ts);
  return ts2hz(&ts);
}

static timeval_t microuptime(void) {
  timeval_t now;
  bintime_t btnow = binuptime();
  bt2tv(&btnow, &now);
  return now;
}

static void kitimer_get(proc_t *p, struct itimerval *tval) {
  assert(mtx_owned(&p->p_lock));

  timeval_t now = microuptime();
  kitimer_t *it = &p->p_itimer;

  if (timercmp(&it->kit_next, &now, <=))
    timerclear(&tval->it_value);
  else
    timersub(&it->kit_next, &now, &tval->it_value);

  tval->it_interval = it->kit_interval;
}

int do_getitimer(proc_t *p, int which, struct itimerval *tval) {
  if (which == ITIMER_PROF || which == ITIMER_VIRTUAL)
    return ENOTSUP;
  else if (which != ITIMER_REAL)
    return EINVAL;

  SCOPED_MTX_LOCK(&p->p_lock);

  kitimer_get(p, tval);

  return 0;
}

bool kitimer_stop(proc_t *p) {
  assert(mtx_owned(&p->p_lock));

  kitimer_t *it = &p->p_itimer;

  if (callout_stop(&it->kit_callout))
    return true;

  mtx_unlock(&p->p_lock);
  callout_drain(&it->kit_callout);
  mtx_lock(&p->p_lock);
  return false;
}

static void kitimer_timeout(void *arg) {
  proc_t *p = arg;

  SCOPED_MTX_LOCK(&p->p_lock);

  if (!proc_is_alive(p))
    return;

  sig_kill(p, &DEF_KSI_RAW(SIGALRM));

  kitimer_t *it = &p->p_itimer;

  if (!timerisset(&it->kit_interval)) {
    timerclear(&it->kit_next);
    return;
  }

  timeval_t next = it->kit_next;
  timeradd(&next, &it->kit_interval, &next);
  timeval_t now = microuptime();
  /* Skip missed periods. This will have the effect of compressing multiple
   * SIGALRM signals into one. */
  while (timercmp(&next, &now, <=))
    timeradd(&next, &it->kit_interval, &next);
  it->kit_next = next;

  /* Undo the `+ 1` done by ts2hz(). */
  systime_t st = tv2hz(&next) - 1;
  callout_reschedule(&it->kit_callout, st);
}

void kitimer_init(proc_t *p) {
  callout_setup(&p->p_itimer.kit_callout, kitimer_timeout, p);
}

/* The timer must have been stopped prior to calling this function. */
static void kitimer_setup(proc_t *p, const struct itimerval *itval) {
  assert(mtx_owned(&p->p_lock));

  kitimer_t *it = &p->p_itimer;
  const timeval_t *value = &itval->it_value;

  if (timerisset(value)) {
    /* Convert next to absolute time.  */
    timeval_t abs = microuptime();
    timeradd(value, &abs, &abs);
    it->kit_next = abs;
    it->kit_interval = itval->it_interval;
    /* Undo the `+ 1` done by ts2hz(). */
    systime_t st = tv2hz(&it->kit_next) - 1;
    callout_schedule_abs(&it->kit_callout, st);
  } else {
    timerclear(&it->kit_next);
    timerclear(&it->kit_interval);
  }
}

int do_setitimer(proc_t *p, int which, const struct itimerval *itval,
                 struct itimerval *oval) {
  if (which == ITIMER_PROF || which == ITIMER_VIRTUAL)
    return ENOTSUP;
  else if (which != ITIMER_REAL)
    return EINVAL;

  if (timeval_invalid(&itval->it_value) || timeval_invalid(&itval->it_interval))
    return EINVAL;

  if (tv2hz(&itval->it_value) == UINT_MAX ||
      tv2hz(&itval->it_interval) == UINT_MAX)
    return EINVAL;

  SCOPED_MTX_LOCK(&p->p_lock);

  /* We need to successfully stop the timer without dropping p_lock.  */
  while (!kitimer_stop(p))
    continue;

  if (oval) {
    /* Store old timer value. */
    kitimer_get(p, oval);
  }

  kitimer_setup(p, itval);

  return 0;
}
