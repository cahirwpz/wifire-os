#define KL_LOG KL_THREAD
#include <sys/klog.h>
#include <sys/libkern.h>
#include <sys/mimiker.h>
#include <sys/pool.h>
#include <sys/malloc.h>
#include <sys/thread.h>
#include <sys/pcpu.h>
#include <sys/sched.h>
#include <sys/proc.h>
#include <sys/sleepq.h>
#include <sys/filedesc.h>
#include <sys/turnstile.h>
#include <sys/kmem.h>
#include <sys/context.h>

static POOL_DEFINE(P_THREAD, "thread", sizeof(thread_t));

typedef TAILQ_HEAD(, thread) thread_list_t;

static MTX_DEFINE(threads_lock, 0);
static thread_list_t all_threads = TAILQ_HEAD_INITIALIZER(all_threads);
static thread_list_t zombie_threads = TAILQ_HEAD_INITIALIZER(zombie_threads);

/* FTTB such a primitive method of creating new TIDs will do. */
static tid_t make_tid(void) {
  static volatile tid_t tid = 1;
  SCOPED_NO_PREEMPTION();
  return tid++;
}

static alignas(PAGESIZE) uint8_t _stack0[KSTACK_SIZE];

/* Thread Zero is initially running with interrupts disabled! */
thread_t thread0 = {
  .td_name = "thread0",
  .td_tid = 0,
  .td_prio = 255,
  .td_base_prio = 255,
  .td_proc = &proc0,
  .td_state = TDS_RUNNING,
  .td_idnest = 1,
  .td_pdnest = 1,
  .td_kstack = KSTACK_INIT(_stack0, KSTACK_SIZE),
};

/* Initializes Thread Zero (first thread in the system). */
void init_thread0(void) {
  thread_t *td = &thread0;

  cv_init(&td->td_waitcv, "thread waiters");
  td->td_sleepqueue = sleepq_alloc();
  td->td_turnstile = turnstile_alloc();
  sigpend_init(&td->td_sigpend);
  LIST_INIT(&td->td_contested);

  WITH_MTX_LOCK (&threads_lock)
    TAILQ_INSERT_TAIL(&all_threads, td, td_all);
}

void thread_reap(void) {
  thread_list_t zombies;

  WITH_MTX_LOCK (&threads_lock) {
    zombies = zombie_threads;
    TAILQ_INIT(&zombie_threads);
  }

  thread_t *td, *next;
  TAILQ_FOREACH_SAFE (td, &zombies, td_zombieq, next)
    thread_delete(td);
}

thread_t *thread_create(const char *name, void (*fn)(void *), void *arg,
                        prio_t prio) {
  /* Firstly recycle some threads to free up memory. */
  thread_reap();

  thread_t *td = pool_alloc(P_THREAD, M_ZERO);

  td->td_tid = make_tid();
  td->td_state = TDS_INACTIVE;

  td->td_prio = prio;
  td->td_base_prio = prio;

  td->td_lock = kmalloc(M_TEMP, sizeof(spin_t), M_ZERO);
  spin_init(td->td_lock, 0);

  cv_init(&td->td_waitcv, "thread waiters");
  LIST_INIT(&td->td_contested);
  bzero(&td->td_slpcallout, sizeof(callout_t));

  td->td_name = kstrndup(M_STR, name, TD_NAME_MAX);
  kstack_init(&td->td_kstack, kmem_alloc(KSTACK_SIZE, M_ZERO), KSTACK_SIZE);

  td->td_sleepqueue = sleepq_alloc();
  td->td_turnstile = turnstile_alloc();

  sigpend_init(&td->td_sigpend);

  thread_entry_setup(td, fn, arg);

  /* From now on, you must use locks on new thread structure. */
  WITH_MTX_LOCK (&threads_lock)
    TAILQ_INSERT_TAIL(&all_threads, td, td_all);

  klog("Thread %ld {%p} has been created", td->td_tid, td);

  return td;
}

void thread_delete(thread_t *td) {
  assert(td_is_dead(td));
  assert(td->td_sleepqueue != NULL);
  assert(td->td_turnstile != NULL);

  klog("Freeing up thread %ld {%p}", td->td_tid, td);

  WITH_MTX_LOCK (&threads_lock)
    TAILQ_REMOVE(&all_threads, td, td_all);

  kmem_free(td->td_kstack.stk_base, KSTACK_SIZE);

  callout_drain(&td->td_slpcallout);
  sleepq_destroy(td->td_sleepqueue);
  turnstile_destroy(td->td_turnstile);
  sigpend_destroy(&td->td_sigpend);
  kfree(M_STR, td->td_name);
  kfree(M_TEMP, td->td_lock);
  pool_free(P_THREAD, td);
}

__no_instrument_kgprof thread_t *thread_self(void) {
  return PCPU_GET(curthread);
}

/* For now this is only a stub
 * NOTE: this procedure must NOT access the thread's process state
 * in ANY way, as the process might have already been reaped. */
__noreturn void thread_exit(void) {
  thread_t *td = thread_self();

  klog("Thread %ld {%p} has finished", td->td_tid, td);

  /* Thread must not exit while having interrupts disabled! However, we can't
   * use assert here, because assert also calls thread_exit. Thus, in case this
   * condition is not met, we'll log the problem and panic! */
  if (intr_disabled())
    panic("ERROR: Thread must not exit when interrupts are disabled!");

  /*
   * Preemption must be disabled for the code below, otherwise the thread may be
   * kicked out of processor without possiblity to return and finish the job.
   *
   * The thread may get switched out as a result of acquiring locks but it
   * hardly matters as we start performing actions with both locks acquired.
   */
  preempt_disable();

  WITH_MTX_LOCK (&threads_lock) {
    spin_lock(td->td_lock); /* force threads_lock >> thread_t::td_lock order */
    TAILQ_INSERT_TAIL(&zombie_threads, td, td_zombieq);
  }

  cv_broadcast(&td->td_waitcv);
  spin_unlock(td->td_lock);

  spin_lock(td->td_lock);
  td->td_state = TDS_DEAD;
  sched_switch();

  panic("Thread %u tried to ressurect", td->td_tid);
}

void thread_join(thread_t *otd) {
  thread_t *td = thread_self();

  klog("Join %ld {%p} with %ld {%p}", td->td_tid, td, otd->td_tid, otd);

  WITH_SPIN_LOCK (otd->td_lock) {
    while (!td_is_dead(otd))
      cv_wait(&otd->td_waitcv, otd->td_lock);
  }
}

void thread_yield(void) {
  thread_t *td = thread_self();

  spin_lock(td->td_lock);
  td->td_state = TDS_READY;
  sched_switch();
}

/* It would be better to have a hash-map from tid_t to thread_t,
 * but using a list is sufficient for now. */
thread_t *thread_find(tid_t id) {
  SCOPED_MTX_LOCK(&threads_lock);

  thread_t *td;
  TAILQ_FOREACH (td, &all_threads, td_all) {
    spin_lock(td->td_lock);
    if (td->td_tid == id)
      return td;
    spin_unlock(td->td_lock);
  }
  return NULL;
}

void thread_continue(thread_t *td) {
  assert(spin_owned(td->td_lock));

  if (td->td_flags & TDF_STOPPING) {
    td->td_flags &= ~TDF_STOPPING;
  } else {
    assert(td_is_stopped(td));
    sched_wakeup(td, 0);
  }
}
