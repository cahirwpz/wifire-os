#ifndef _SYS_TIMER_H_
#define _SYS_TIMER_H_

#include <sys/queue.h>
#include <sys/time.h>

typedef struct timer timer_t;
typedef TAILQ_HEAD(timer_head, timer) timer_list_t;

typedef int (*tm_start_t)(timer_t *tm, unsigned flags, const bintime_t start,
                          const bintime_t period);
typedef int (*tm_stop_t)(timer_t *tm);

/*! \brief Type of function for fetching current time.
 *
 * \warning This routine cannot log messages, cause it's used by klog! */
typedef bintime_t (*tm_gettime_t)(timer_t *tm);

/*! \brief Callback function type.
 *
 * \warning It will be called in interrupt context! */
typedef void (*tm_event_cb_t)(timer_t *tm, void *arg);

/* There are some flags used by implementation that are not listed here! */
#define TMF_ONESHOT 0x0001    /*!< triggers callback once */
#define TMF_PERIODIC 0x0002   /*!< triggers callback on regular basis */
#define TMF_STABLE 0x0004     /*!< run with fixed frequency */
#define TMF_TYPEMASK 0x0007   /*!< don't use other bits! */
#define TMF_TIMESOURCE 0x0008 /*!< choose this timer as time source */

typedef struct timer {
  TAILQ_ENTRY(timer) tm_link; /*!< entry on list of all timers */
  const char *tm_name;        /*!< name of the timer */
  unsigned tm_flags;          /*!< TMF_* flags */
  uint32_t tm_frequency;      /*!< base frequency of the timer */
  bintime_t tm_min_period;    /*!< valid only for TMF_PERIODIC */
  bintime_t tm_max_period;    /*!< same as above */
  tm_start_t tm_start;        /*!< makes timer operational */
  tm_stop_t tm_stop;          /*!< ceases timer from generating new events */
  tm_event_cb_t tm_event_cb;  /*!< callback called when timer triggers */
  tm_gettime_t tm_gettime;    /*!< fetches current time from the timer */
  void *tm_arg;               /*!< an argument for callback */
  void *tm_priv;              /*!< private data (usually device_t *) */
} timer_t;

static inline bintime_t tm_gettime(timer_t *tm) {
  return tm->tm_gettime(tm);
}

/*! \brief  Used to set/change the system boottime */
void tm_setclock(const bintime_t *bt);
/*! \brief Used by a driver to make timer device available to the system. */
int tm_register(timer_t *tm);
/*! \brief Called when unloading a timer driver. */
int tm_deregister(timer_t *tm);

/*! \brief Find a timer according to criteria and reserve for exclusive use.
 *
 * \arg name specifies name of the timer (can be NULL)
 * \arg flags specifies timer capabilities
 */
timer_t *tm_reserve(const char *name, unsigned flags);
/*! \brief Releases a timer if it's not needed anymore. */
int tm_release(timer_t *tm);

/*! \brief Prepares timer to call event trigger callback. */
int tm_init(timer_t *tm, tm_event_cb_t event, void *arg);
/*! \brief Configures timer to trigger callback(s). */
int tm_start(timer_t *tm, unsigned flags, const bintime_t start,
             const bintime_t period);
/*! \brief Stops timer from triggering a callback. */
int tm_stop(timer_t *tm);
/*! \brief Used by interrupt filter routine to trigger a callback. */
void tm_trigger(timer_t *tm);

/*! \brief Select timer used as a main time source (for binuptime, etc.) */
void tm_select(timer_t *tm);

/*! \brief TODO !!! */
void tm_calibrate(void);

#endif /* !_SYS_TIMER_H_ */
