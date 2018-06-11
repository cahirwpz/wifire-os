#ifndef _SYS_SLEEPQ_H_
#define _SYS_SLEEPQ_H_

#include <common.h>
#include <queue.h>

typedef struct thread thread_t;
typedef struct sleepq sleepq_t;

typedef enum {
  SLP_WKP_REG, /* regular wakeup
                * keep it as the first entry (because of flags below) */
  SLP_WKP_INT  /* thread interrupted */
} slp_wakeup_t;

#define SLPF_OF_WKP(w) (1 << ((w)-1))

typedef uint8_t sleep_flags_t;
#define SLPF_INT SLPF_OF_WKP(SLP_WKP_INT)

/*! \file sleepq.h */

/*! \brief Initializes sleep queues.
 *
 * \warning To be called only from early kernel initialization! */
void sleepq_init(void);

/*! \brief Allocates sleep queue entry. */
sleepq_t *sleepq_alloc(void);

/*! \brief Deallocates sleep queue entry. */
void sleepq_destroy(sleepq_t *sq);

/*! \brief Blocks the current thread until it is awakened from its sleep queue.
 *
 * \param wchan unique sleep queue identifier
 * \param waitpt caller associated with sleep action
 */
void sleepq_wait(void *wchan, const void *waitpt);

// TODO description
slp_wakeup_t sleepq_wait_flg(void *wchan, const void *waitpt,
                             sleep_flags_t flags);

/*! \brief Wakes up highest priority thread waiting on \a wchan.
 *
 * \param wchan unique sleep queue identifier
 */
bool sleepq_signal(void *wchan);

// TODO description
bool sleepq_signal_thread(thread_t *td, slp_wakeup_t reason);

/*! \brief Resume all threads sleeping on \a wchan.
 *
 * \param wchan unique sleep queue identifier
 */
bool sleepq_broadcast(void *wchan);

#endif /* !_SYS_SLEEPQ_H_ */
