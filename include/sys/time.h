#ifndef _SYS_TIME_H_
#define _SYS_TIME_H_

#include <sys/types.h>

#define CLK_TCK 1000 /* system clock ticks per second 1[tick] = 1[ms]  */

typedef struct tm {
  int tm_sec;          /* seconds after the minute [0-61] */
  int tm_min;          /* minutes after the hour [0-59] */
  int tm_hour;         /* hours since midnight [0-23] */
  int tm_mday;         /* day of the month [1-31] */
  int tm_mon;          /* months since January [0-11] */
  int tm_year;         /* years since 1900 */
  int tm_wday;         /* days since Sunday [0-6] */
  int tm_yday;         /* days since January 1 [0-365] */
  int tm_isdst;        /* Daylight Savings Time flag */
  long tm_gmtoff;      /* offset from UTC in seconds */
  const char *tm_zone; /* timezone abbreviation */
} tm_t;

typedef struct timeval {
  time_t tv_sec;       /* seconds */
  suseconds_t tv_usec; /* microseconds */
} timeval_t;

typedef struct timespec {
  time_t tv_sec; /* seconds */
  long tv_nsec;  /* and nanoseconds */
} timespec_t;

typedef struct bintime {
  time_t sec;    /* second */
  uint64_t frac; /* a fraction of second */
} bintime_t;

#define _BINTIME_SEC(fp) ((time_t)(int64_t)(fp))
#define _BINTIME_FRAC(fp) ((uint64_t)((fp - (int64_t)(fp)) * (1ULL << 63) * 2))

#define BINTIME(fp)                                                            \
  (bintime_t) {                                                                \
    .sec = _BINTIME_SEC(fp), .frac = _BINTIME_FRAC(fp)                         \
  }

#define HZ2BT(hz)                                                              \
  (bintime_t) {                                                                \
    .sec = 0, .frac = ((1ULL << 63) / (hz)) << 1                               \
  }

/* Returns seconds after EPOCH */
time_t tm2sec(tm_t *tm);

static inline systime_t bt2st(const bintime_t *bt) {
  return bt->sec * CLK_TCK +
         (((uint64_t)CLK_TCK * (uint32_t)(bt->frac >> 32)) >> 32);
}

static inline void bt2ts(const bintime_t *bt, timespec_t *ts) {
  ts->tv_sec = bt->sec;
  ts->tv_nsec = (1000000000ULL * (uint32_t)(bt->frac >> 32)) >> 32;
}

static inline void bt2tv(const bintime_t *bt, timeval_t *tv) {
  tv->tv_sec = bt->sec;
  tv->tv_usec = (1000000ULL * (uint32_t)(bt->frac >> 32)) >> 32;
}

/* Operations on timevals. */
#define timerclear(tvp) (tvp)->tv_sec = (tvp)->tv_usec = 0L
#define timerisset(tvp) ((tvp)->tv_sec || (tvp)->tv_usec)
#define timercmp(tvp, uvp, cmp)                                                \
  (((tvp)->tv_sec == (uvp)->tv_sec) ? ((tvp)->tv_usec cmp(uvp)->tv_usec)       \
                                    : ((tvp)->tv_sec cmp(uvp)->tv_sec))
#define timeradd(tvp, uvp, vvp)                                                \
  {                                                                            \
    (vvp)->tv_sec = (tvp)->tv_sec + (uvp)->tv_sec;                             \
    (vvp)->tv_usec = (tvp)->tv_usec + (uvp)->tv_usec;                          \
    if ((vvp)->tv_usec >= 1000000) {                                           \
      (vvp)->tv_sec++;                                                         \
      (vvp)->tv_usec -= 1000000;                                               \
    }                                                                          \
  }
#define timersub(tvp, uvp, vvp)                                                \
  {                                                                            \
    (vvp)->tv_sec = (tvp)->tv_sec - (uvp)->tv_sec;                             \
    (vvp)->tv_usec = (tvp)->tv_usec - (uvp)->tv_usec;                          \
    if ((vvp)->tv_usec < 0) {                                                  \
      (vvp)->tv_sec--;                                                         \
      (vvp)->tv_usec += 1000000;                                               \
    }                                                                          \
  }

static inline void tv2ts(const timeval_t *tv, timespec_t *ts) {
  ts->tv_sec = tv->tv_sec;
  ts->tv_nsec = tv->tv_usec * 1000;
}

/* Operations on bintime. */
#define bintime_cmp(a, b, cmp)                                                 \
  (((a)->sec == (b)->sec) ? (((a)->frac)cmp((b)->frac))                        \
                          : (((a)->sec)cmp((b)->sec)))
#define bintime_isset(bt) ((bt)->sec || (bt)->frac)

static inline void bintime_add_frac(bintime_t *bt, uint64_t x) {
  uint64_t old_frac = bt->frac;
  bt->frac += x;
  if (old_frac > bt->frac)
    bt->sec++;
}

static inline bintime_t bintime_mul(const bintime_t bt, uint32_t x) {
  uint64_t p1 = (bt.frac & 0xffffffffULL) * x;
  uint64_t p2 = (bt.frac >> 32) * x + (p1 >> 32);
  return (bintime_t){.sec = bt.sec * x + (p2 >> 32),
                     .frac = (p2 << 32) | (p1 & 0xffffffffULL)};
}

static inline void bintime_add(bintime_t *bt, bintime_t *bt2) {
  bintime_add_frac(bt, bt2->frac);
  bt->sec += bt2->sec;
}

static inline void bintime_sub(bintime_t *bt, bintime_t *bt2) {
  uint64_t old_frac = bt->frac;
  bt->frac -= bt2->frac;
  if (old_frac < bt->frac)
    bt->sec--;
  bt->sec -= bt2->sec;
}

typedef enum clockid { CLOCK_MONOTONIC = 1, CLOCK_REALTIME = 2 } clockid_t;

/* Operations on timespecs. */
#define timespecclear(tsp) (tsp)->tv_sec = (time_t)((tsp)->tv_nsec = 0L)
#define timespecisset(tsp) ((tsp)->tv_sec || (tsp)->tv_nsec)
#define timespeccmp(tsp, usp, cmp)                                             \
  (((tsp)->tv_sec == (usp)->tv_sec) ? ((tsp)->tv_nsec cmp(usp)->tv_nsec)       \
                                    : ((tsp)->tv_sec cmp(usp)->tv_sec))
#define timespecadd(tsp, usp, vsp)                                             \
  {                                                                            \
    (vsp)->tv_sec = (tsp)->tv_sec + (usp)->tv_sec;                             \
    (vsp)->tv_nsec = (tsp)->tv_nsec + (usp)->tv_nsec;                          \
    if ((vsp)->tv_nsec >= 1000000000L) {                                       \
      (vsp)->tv_sec++;                                                         \
      (vsp)->tv_nsec -= 1000000000L;                                           \
    }                                                                          \
  }
#define timespecsub(tsp, usp, vsp)                                             \
  {                                                                            \
    (vsp)->tv_sec = (tsp)->tv_sec - (usp)->tv_sec;                             \
    (vsp)->tv_nsec = (tsp)->tv_nsec - (usp)->tv_nsec;                          \
    if ((vsp)->tv_nsec < 0) {                                                  \
      (vsp)->tv_sec--;                                                         \
      (vsp)->tv_nsec += 1000000000L;                                           \
    }                                                                          \
  }

/*
 * Names of the interval timers, and structure defining a timer setting.
 */
#define ITIMER_REAL 0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF 2
#define ITIMER_MONOTONIC 3

struct itimerval {
  struct timeval it_interval; /* timer interval */
  struct timeval it_value;    /* current value */
};

#define TIMER_RELTIME 0 /* relative timer */
#define TIMER_ABSTIME 1 /* absolute timer */

#ifdef _KERNEL

#include <sys/callout.h>

typedef struct proc proc_t;

/* Kernel interval timer. */
typedef struct {
  /* absolute time of nearest expiration, 0 means inactive */
  timeval_t kit_next;
  timeval_t kit_interval; /* time between expirations, 0 means non-periodic */
  callout_t kit_callout;
} kitimer_t;

/* Initialize a process's interval timer structure. */
void kitimer_init(proc_t *p);

/* Stop the interval timer associated with a process.
 * After this function returns, it's safe to free the timer structure.
 * Must be called with p->p_lock held.
 * NOTE: This function may release and re-acquire p->p_lock.
 * It returns true if it released the lock. */
bool kitimer_stop(proc_t *p);

/* Time measured from the start of system. */
bintime_t binuptime(void);

/* UTC/POSIX time */
bintime_t bintime(void);

/* System time is measured in ticks (1[ms] by default),
 * and is maintained by system clock. */
systime_t getsystime(void);

timespec_t nanotime(void);

systime_t ts2hz(const timespec_t *ts);

int do_clock_gettime(clockid_t clk, timespec_t *tp);

int do_clock_nanosleep(clockid_t clk, int flags, timespec_t *rqtp,
                       timespec_t *rmtp);

int do_getitimer(proc_t *p, int which, struct itimerval *tval);

int do_setitimer(proc_t *p, int which, const struct itimerval *itval,
                 struct itimerval *oval);

#else /* _KERNEL */

int nanosleep(const timespec_t *rqtp, timespec_t *rmtp);

int adjtime(const struct timeval *, struct timeval *);
int gettimeofday(struct timeval *__restrict, void *__restrict);
int settimeofday(const struct timeval *__restrict, const void *__restrict);

int clock_gettime(clockid_t clk, timespec_t *tp);

int clock_nanosleep(clockid_t clk, int flags, const timespec_t *rqtp,
                    timespec_t *rmtp);

int getitimer(int, struct itimerval *);
int setitimer(int, const struct itimerval *__restrict,
              struct itimerval *__restrict);

#endif /* !_KERNEL */

#endif /* !_SYS_TIME_H_ */
