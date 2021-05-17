#ifndef _SYS_INTERRUPT_H_
#define _SYS_INTERRUPT_H_

#include <sys/cdefs.h>
#include <sys/queue.h>
#include <sys/spinlock.h>
#include <sys/priority.h>

typedef struct ctx ctx_t;
typedef struct device device_t;

/*! \brief Disables hardware interrupts.
 *
 * Calls to \fn intr_disable can nest, you must use the same number of calls to
 * \fn intr_enable to actually enable interrupts.
 *
 * In most scenarios threads should not switch out when interrupts are disabled.
 * However it should behave correctly, since context switch routine restores
 * interrupts state of target thread.
 *
 * If you need to disable preemption refer to \fn preempt_disable.
 *
 * \sa preempt_disable()
 */
void intr_disable(void) __no_instrument_kgprof;

/*! \brief Enables interrupts. */
void intr_enable(void) __no_instrument_kgprof;

/*! \brief Checks if interrupts are disabled now. */
bool intr_disabled(void) __no_instrument_kgprof;

/* Two following functions are workaround to make interrupt disabling work with
 * scoped and with statement. */
static inline void __intr_disable(void *data) {
  intr_disable();
}

static inline void __intr_enable(void *data) {
  intr_enable();
}

#define SCOPED_INTR_DISABLED()                                                 \
  SCOPED_STMT(void, __intr_disable, __intr_enable, NULL)

#define WITH_INTR_DISABLED WITH_STMT(void, __intr_disable, __intr_enable, NULL)

typedef struct intr_event intr_event_t;
typedef struct intr_handler intr_handler_t;

typedef enum {
  IF_STRAY = 0,    /* this device did not trigger the interrupt */
  IF_FILTERED = 1, /* the interrupt has been handled and can be EOId */
  IF_DELEGATE = 2, /* the handler should be run in private thread */
} intr_filter_t;

/*
 * The filter routine is run in primary interrupt context and may not
 * block or use regular mutexes.  The filter may either completely
 * handle the interrupt or it may perform some of the work and
 * defer more expensive work to the regular interrupt handler.
 */
typedef intr_filter_t ih_filter_t(void *);
typedef void ih_service_t(void *);
typedef void ie_action_t(intr_event_t *);

/* Software representation of interrupt line. */
typedef struct intr_event {
  spin_t ie_lock;
  TAILQ_ENTRY(intr_event) ie_link; /* link on list of all interrupt events */
  TAILQ_HEAD(, intr_handler) ie_handlers; /* sorted by descending ih_prio */
  ie_action_t *ie_disable; /* called before ithread delegation (mask irq) */
  ie_action_t *ie_enable;  /* called after ithread delagation (unmask irq) */
  void *ie_source;         /* additional argument for actions */
  const char *ie_name;     /* individual event name */
  unsigned ie_irq;         /* physical interrupt request line number */
  thread_t *ie_ithread;    /* associated interrupt thread */
} intr_event_t;

intr_event_t *intr_event_create(void *source, int irq, ie_action_t *disable,
                                ie_action_t *enable, const char *name);
intr_handler_t *intr_event_add_handler(intr_event_t *ie, ih_filter_t *filter,
                                       ih_service_t *service, void *arg,
                                       const char *name);
void intr_event_remove_handler(intr_handler_t *ih);
void intr_event_run_handlers(intr_event_t *ie);

typedef void (*intr_root_filter_t)(ctx_t *ctx, device_t *dev, void *arg);

void intr_root_claim(intr_root_filter_t filter, device_t *dev, void *arg);
void intr_root_handler(ctx_t *ctx) __no_instrument_kgprof;

#endif /* !_SYS_INTERRUPT_H_ */
