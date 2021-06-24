#define KL_LOG KL_TIME
#include <sys/klog.h>
#include <sys/mimiker.h>
#include <sys/libkern.h>
#include <sys/timer.h>
#include <sys/mutex.h>
#include <sys/errno.h>

static MTX_DEFINE(timers_mtx, 0);
static timer_list_t timers = TAILQ_HEAD_INITIALIZER(timers);
static timer_t *time_source = NULL;
static bintime_t boottime = BINTIME(0);

/* These flags are used internally to encode timer state.
 * Following state transitions are possible:
 *
 * \dot
 * digraph timer_state {
 *   a [label="initial", style=bold];
 *   b [label="registered"];
 *   c [label="registered ∧ reserved"];
 *   d [label="registered ∧ reserved ∧ initialized"];
 *   e [label="registered ∧ reserved ∧ initialized ∧ active"];
 *   a -> b [label="tm_register"];
 *   b -> c [label="tm_reserve"];
 *   c -> d [label="tm_init"];
 *   d -> e [label="tm_start"];
 *   e -> d [label="tm_stop"];
 *   d -> b [label="tm_release"];
 *   c -> b [label="tm_release"];
 *   b -> a [label="tm_deregister"];
 * }
 * \enddot
 */

#define TMF_ACTIVE 0x1000
#define TMF_INITIALIZED 0x2000
#define TMF_RESERVED 0x4000
#define TMF_REGISTERED 0x8000

#define is_active(tm) ((tm)->tm_flags & TMF_ACTIVE)
#define is_initialized(tm) ((tm)->tm_flags & TMF_INITIALIZED)
#define is_reserved(tm) ((tm)->tm_flags & TMF_RESERVED)
#define is_registered(tm) ((tm)->tm_flags & TMF_REGISTERED)

int tm_register(timer_t *tm) {
  if (is_registered(tm))
    return EBUSY;

  WITH_MTX_LOCK (&timers_mtx) {
    TAILQ_INSERT_TAIL(&timers, tm, tm_link);
    tm->tm_flags &= TMF_TYPEMASK;
    tm->tm_flags |= TMF_REGISTERED;
  }

  klog("Registered '%s' timer.", tm->tm_name);
  return 0;
}

int tm_deregister(timer_t *tm) {
  assert(is_registered(tm));

  if (is_reserved(tm))
    return EBUSY;

  WITH_MTX_LOCK (&timers_mtx) {
    TAILQ_REMOVE(&timers, tm, tm_link);
    tm->tm_flags &= TMF_TYPEMASK;
  }

  klog("Unregistered '%s' timer.", tm->tm_name);
  return 0;
}

timer_t *tm_reserve(const char *name, unsigned flags) {
  timer_t *found = NULL;

  WITH_MTX_LOCK (&timers_mtx) {
    timer_t *tm;
    TAILQ_FOREACH (tm, &timers, tm_link) {
      if (is_reserved(tm))
        continue;
      if (name && strcmp(tm->tm_name, name))
        continue;
      if (!(tm->tm_flags & flags))
        continue;
      if (!found || found->tm_quality < tm->tm_quality)
        found = tm;
    }
    if (found)
      found->tm_flags |= TMF_RESERVED;
  }
  return found;
}

int tm_release(timer_t *tm) {
  assert(is_reserved(tm));

  if (is_active(tm))
    return EBUSY;

  WITH_MTX_LOCK (&timers_mtx) {
    TAILQ_INSERT_TAIL(&timers, tm, tm_link);
    tm->tm_flags &= ~(TMF_INITIALIZED | TMF_RESERVED);
  }

  return 0;
}

void tm_setclock(const bintime_t *bt) {
  bintime_t bt1 = *bt, bt2;
  /* TODO: Add (spin) lock for settime */
  bt2 = binuptime();
  /* Setting boottime - this is why we subtract time elapsed since boottime */
  bintime_sub(&bt1, &bt2);
  boottime = bt1;
}

int tm_init(timer_t *tm, tm_event_cb_t event, void *arg) {
  assert(is_reserved(tm));

  if (is_active(tm))
    return EBUSY;

  tm->tm_flags |= TMF_INITIALIZED;
  tm->tm_event_cb = event;
  tm->tm_arg = arg;
  return 0;
}

int tm_start(timer_t *tm, unsigned flags, const bintime_t start,
             const bintime_t period) {
  assert(is_initialized(tm));

  if (is_active(tm))
    return EBUSY;
  if (((tm->tm_flags & flags) & TMF_TYPEMASK) == 0)
    return ENODEV;
  if (flags & TMF_PERIODIC) {
    if (bintime_cmp(&period, &tm->tm_min_period, <) ||
        bintime_cmp(&period, &tm->tm_max_period, >))
      return EINVAL;
  }

  int retval = tm->tm_start(tm, flags, start, period);
  if (retval == 0)
    tm->tm_flags |= TMF_ACTIVE;
  if (flags & TMF_TIMESOURCE)
    time_source = tm;
  return retval;
}

int tm_stop(timer_t *tm) {
  assert(is_initialized(tm));

  if (!is_active(tm))
    return 0;

  int retval = tm->tm_stop(tm);
  if (retval == 0)
    tm->tm_flags &= ~TMF_ACTIVE;
  return retval;
}

void tm_trigger(timer_t *tm) {
  assert(is_initialized(tm));
  assert(intr_disabled());

  tm->tm_event_cb(tm, tm->tm_arg);
}

bintime_t binuptime(void) {
  /* XXX: probably a race condition here */
  timer_t *tm = time_source;
  if (tm == NULL)
    return BINTIME(0);
  return tm->tm_gettime(tm);
}

bintime_t bintime(void) {
  bintime_t retval = binuptime();
  bintime_add(&retval, &boottime);
  return retval;
}
