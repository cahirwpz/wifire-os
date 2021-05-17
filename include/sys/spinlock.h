#ifndef _SYS_SPINLOCK_H_
#define _SYS_SPINLOCK_H_

#include <stdbool.h>
#include <sys/mimiker.h>
#include <sys/_lock.h>

typedef struct thread thread_t;

/*! \file spinlock.h */

/*! \brief Special purpose synchronization primitive.
 *
 * Spin lock should be used to synchronize kernel code wrt. interrupt contexts.
 *
 * Acquiring spin lock enters critical section, thus disables interrupts and
 * thread preemption.
 *
 * \attention It is forbidden to change context while holding spin lock!
 * \todo How to enforce the condition given above?
 *
 * \note Spin lock must be released by its owner!
 * \todo During transition from single-core to multi-core architecture spin lock
 *       will be used for interprocessor synchronization as well.
 */
typedef struct spin {
  lk_attr_t s_attr;           /*!< lock attributes */
  volatile unsigned s_count;  /*!< counter for recursive spinlock */
  volatile thread_t *s_owner; /*!< stores address of the owner */
  const void *s_lockpt;       /*!< place where the lock was acquired */
} spin_t;

#define SPIN_INITIALIZER(spinname, recursive)                                  \
  (spin_t) {                                                                   \
    .s_attr = (recursive) | LK_TYPE_SPIN                                       \
  }

#define SPIN_DEFINE(spinname, recursive)                                       \
  spin_t spinname = SPIN_INITIALIZER(spinname, recursive);

/*! \brief Initializes spin lock.
 *
 * \note Every spin lock has to be initialized before it is used. */
void spin_init(spin_t *s, lk_attr_t attr);

/*! \brief Makes spin lock unusable for further locking.
 *
 * \todo Not implemented yet. */
#define spin_destroy(m)

/*! \brief Check if calling thread is the owner of \a s. */
bool spin_owned(spin_t *s) __no_instrument_kgprof;

/*! \brief Acquires the spin lock (with custom \a waitpt) */
void _spin_lock(spin_t *s, const void *waitpt) __no_instrument_kgprof;

/*! \brief Acquire the spin lock.
 *
 * \note On single core architecture it impossible to block on spinlock.
 */
static inline void spin_lock(spin_t *s) {
  _spin_lock(s, __caller(0));
}

/*! \brief Release the spin lock. */
void spin_unlock(spin_t *s) __no_instrument_kgprof;

DEFINE_CLEANUP_FUNCTION(spin_t *, spin_unlock);

/*! \brief Acquire spin lock and release it when leaving current scope.
 *
 * You may safely leave the scope by using `break` or `return`.
 *
 * \warning Do not call `noreturn` functions before you leave the scope!
 */
#define SCOPED_SPIN_LOCK(spin_p)                                               \
  SCOPED_STMT(spin_t, spin_lock, CLEANUP_FUNCTION(spin_unlock), spin_p)

/*! \brief Enter scope with spin lock acquired.
 *
 * Releases the spin lock when leaving the scope.
 *
 * \sa SCOPED_SPINLOCK
 */
#define WITH_SPIN_LOCK(spin_p)                                                 \
  WITH_STMT(spin_t, spin_lock, CLEANUP_FUNCTION(spin_unlock), spin_p)

#endif /* !_SYS_SPINLOCK_H_ */
