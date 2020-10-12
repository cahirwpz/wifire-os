#include <sys/mimiker.h>
#include <sys/pool.h>
#include <sys/spinlock.h>
#include <sys/sched.h>
#include <sys/turnstile.h>
#include <sys/queue.h>

#define TC_TABLESIZE 256 /* Must be power of 2. */
#define TC_MASK (TC_TABLESIZE - 1)
#define TC_SHIFT 8
#define TC_HASH(wc)                                                            \
  ((((uintptr_t)(wc) >> TC_SHIFT) ^ (uintptr_t)(wc)) & TC_MASK)
#define TC_LOOKUP(wc) &turnstile_chains[TC_HASH(wc)]

typedef TAILQ_HEAD(td_queue, thread) td_queue_t;
typedef LIST_HEAD(ts_list, turnstile) ts_list_t;

/* Possible turnstile ts states:
 * - FREE_UNBLOCKED:
 *   > ts is owned by some unblocked thread td
 *   > td->td_turnstile is equal to ts
 *   > ts->ts_wchan is equal to NULL
 *
 * - FREE_BLOCKED:
 *   > ts formerly owned by a thread td
 *   > td is now blocked on mutex mtx
 *   > td was not the first one to block on mtx
 *   > ts->ts_wchan is equal to NULL
 *
 * - USED_BLOCKED:
 *   > ts formerly owned by a thread td
 *   > td is now blocked on mutex mtx
 *   > td was the first one to block on mtx
 *   > ts->ts_wchan is equal to &mtx
 *   > other threads blocked on mtx are appended to ts->ts_blocked
 */
typedef enum { FREE_UNBLOCKED, FREE_BLOCKED, USED_BLOCKED } ts_state_t;

typedef struct turnstile {
  union {
    LIST_ENTRY(turnstile) ts_chain_link; /* link on turnstile chain */
    LIST_ENTRY(turnstile) ts_free_link;  /* link on ts_free list */
  };
  LIST_ENTRY(turnstile) ts_contested_link; /* link on td_contested */
  /* free turnstiles left by threads blocked on this turnstile */
  ts_list_t ts_free;
  /* blocked threads sorted by decreasing active priority */
  td_queue_t ts_blocked;
  void *ts_wchan;      /* waiting channel */
  thread_t *ts_owner;  /* who owns the lock */
  ts_state_t ts_state; /* state of turnstile */
} turnstile_t;

typedef struct turnstile_chain {
  spin_t tc_lock;
  ts_list_t tc_turnstiles;
} turnstile_chain_t;

static turnstile_chain_t turnstile_chains[TC_TABLESIZE];

static void turnstile_ctor(turnstile_t *ts) {
  LIST_INIT(&ts->ts_free);
  TAILQ_INIT(&ts->ts_blocked);
  ts->ts_wchan = NULL;
  ts->ts_owner = NULL;
  ts->ts_state = FREE_UNBLOCKED;
}

void init_turnstile(void) {
  for (int i = 0; i < TC_TABLESIZE; i++) {
    turnstile_chain_t *tc = &turnstile_chains[i];
    tc->tc_lock = SPIN_INITIALIZER(0);
    LIST_INIT(&tc->tc_turnstiles);
  }
}

static POOL_DEFINE(P_TURNSTILE, "turnstile", sizeof(turnstile_t));

turnstile_t *turnstile_alloc(void) {
  turnstile_t *ts = pool_alloc(P_TURNSTILE, M_ZERO);
  turnstile_ctor(ts);
  return ts;
}

void turnstile_destroy(turnstile_t *ts) {
  pool_free(P_TURNSTILE, ts);
}

static void adjust_thread_forward(turnstile_t *ts, thread_t *td) {
  assert(ts->ts_state == USED_BLOCKED);
  thread_t *next = td;

  while (TAILQ_NEXT(next, td_blockedq) != NULL &&
         prio_gt(TAILQ_NEXT(next, td_blockedq)->td_prio, td->td_prio))
    next = TAILQ_NEXT(next, td_blockedq);

  if (next != td) {
    TAILQ_REMOVE(&ts->ts_blocked, td, td_blockedq);
    TAILQ_INSERT_AFTER(&ts->ts_blocked, next, td, td_blockedq);
  }
}

static void adjust_thread_backward(turnstile_t *ts, thread_t *td) {
  assert(ts->ts_state == USED_BLOCKED);
  thread_t *prev = td;

  while (TAILQ_PREV(prev, td_queue, td_blockedq) != NULL &&
         prio_lt(TAILQ_PREV(prev, td_queue, td_blockedq)->td_prio, td->td_prio))
    prev = TAILQ_PREV(prev, td_queue, td_blockedq);

  if (prev != td) {
    TAILQ_REMOVE(&ts->ts_blocked, td, td_blockedq);
    TAILQ_INSERT_BEFORE(prev, td, td_blockedq);
  }
}

/* Adjusts thread's position on ts_blocked queue after its priority
 * has been changed. */
static void adjust_thread(turnstile_t *ts, thread_t *td, prio_t oldprio) {
  assert(ts->ts_state == USED_BLOCKED);
  assert(td_is_blocked(td));

  if (prio_gt(td->td_prio, oldprio))
    adjust_thread_backward(ts, td);
  else if (prio_lt(td->td_prio, oldprio))
    adjust_thread_forward(ts, td);
}

/* \note Acquires td_lock! */
static thread_t *acquire_owner(turnstile_t *ts) {
  assert(ts->ts_state == USED_BLOCKED);
  thread_t *td = ts->ts_owner;
  assert(td != NULL); /* Turnstile must have an owner. */
  if (!spin_owned(td->td_lock))
    spin_lock(td->td_lock);
  assert(!td_is_sleeping(td)); /* You must not sleep while holding a mutex. */
  return td;
}

/* Walks the chain of turnstiles and their owners to propagate the priority
 * of td to all the threads holding locks that have to be released before
 * td can run again. */
static void propagate_priority(thread_t *td) {
  turnstile_t *ts = td->td_blocked;
  prio_t prio = td->td_prio;

  td = acquire_owner(ts);

  /* Walk through blocked threads. */
  while (prio_lt(td->td_prio, prio) && !td_is_ready(td) && !td_is_running(td)) {
    assert(td != thread_self()); /* Deadlock. */
    assert(td_is_blocked(td));

    prio_t oldprio = td->td_prio;
    sched_lend_prio(td, prio);

    ts = td->td_blocked;
    assert(ts != NULL);
    assert(ts->ts_state == USED_BLOCKED);

    /* Resort td on the blocked list if needed. */
    adjust_thread(ts, td, oldprio);
    spin_unlock(td->td_lock);

    td = acquire_owner(ts);
  }

  /* Possibly finish at a running/runnable thread. */
  if (prio_lt(td->td_prio, prio) && (td_is_ready(td) || td_is_running(td))) {
    sched_lend_prio(td, prio);
    assert(td->td_blocked == NULL);
  }

  // spin_unlock(td->td_lock);
}

void turnstile_adjust(thread_t *td, prio_t oldprio) {
  assert(spin_owned(td->td_lock));
  assert(td_is_blocked(td));

  turnstile_t *ts = td->td_blocked;
  assert(ts != NULL);
  assert(ts->ts_state == USED_BLOCKED);

  adjust_thread(ts, td, oldprio);

  /* If td got higher priority and it is at the head of ts_blocked,
   * propagate its priority. */
  if (td == TAILQ_FIRST(&ts->ts_blocked) && prio_gt(td->td_prio, oldprio))
    propagate_priority(td);
}

static void switch_away(turnstile_t *ts, const void *waitpt) {
  assert(ts->ts_state == USED_BLOCKED);
  thread_t *td = thread_self();

  spin_lock(td->td_lock);
  td->td_turnstile = NULL;
  td->td_blocked = ts;
  td->td_wchan = ts->ts_wchan;
  td->td_waitpt = waitpt;
  td->td_state = TDS_BLOCKED;
  propagate_priority(td);
  sched_switch();
}

/* Give back turnstiles from ts_free to threads blocked on ts_blocked.
 *
 * As there are more threads on ts_blocked than turnstiles on ts_free (by one),
 * one thread instead of getting some turnstile from ts_free will get ts. */
static void give_back_turnstiles(turnstile_t *ts) {
  assert(ts != NULL);
  assert(ts->ts_state == USED_BLOCKED);
  assert(ts->ts_owner == thread_self());

  thread_t *td;
  TAILQ_FOREACH (td, &ts->ts_blocked, td_blockedq) {
    turnstile_t *ts_for_td;
    if (LIST_EMPTY(&ts->ts_free)) {
      assert(TAILQ_NEXT(td, td_blockedq) == NULL);
      ts_for_td = ts;

      assert(ts_for_td->ts_state == USED_BLOCKED);
      assert(ts_for_td->ts_wchan != NULL);

      ts_for_td->ts_wchan = NULL;
      LIST_REMOVE(ts_for_td, ts_chain_link);
    } else {
      ts_for_td = LIST_FIRST(&ts->ts_free);

      assert(ts_for_td->ts_state == FREE_BLOCKED);
      assert(ts_for_td->ts_wchan == NULL);

      LIST_REMOVE(ts_for_td, ts_free_link);
    }

    ts_for_td->ts_state = FREE_UNBLOCKED;
    td->td_turnstile = ts_for_td;
  }
}

/* Walks td_contested list of thread_self(), counts maximum priority of
 * threads locked on us, and calls sched_unlend_prio. */
static void unlend_self(turnstile_t *ts) {
  assert(ts != NULL);

  thread_t *td = thread_self();
  assert(ts->ts_owner == td);

  prio_t prio = prio_uthread(255);

  ts->ts_owner = NULL;
  LIST_REMOVE(ts, ts_contested_link);

  turnstile_t *ts_owned;
  LIST_FOREACH (ts_owned, &td->td_contested, ts_contested_link) {
    assert(ts_owned->ts_owner == td);
    prio_t p = TAILQ_FIRST(&ts_owned->ts_blocked)->td_prio;
    if (prio_gt(p, prio))
      prio = p;
  }

  WITH_SPIN_LOCK (td->td_lock)
    sched_unlend_prio(td, prio);
}

static void wakeup_blocked(td_queue_t *blocked_threads) {
  while (!TAILQ_EMPTY(blocked_threads)) {
    thread_t *td = TAILQ_FIRST(blocked_threads);
    TAILQ_REMOVE(blocked_threads, td, td_blockedq);

    WITH_SPIN_LOCK (td->td_lock) {
      assert(td_is_blocked(td));
      td->td_blocked = NULL;
      td->td_wchan = NULL;
      td->td_waitpt = NULL;
      sched_wakeup(td, 0);
    }
  }
}

/* Looks for turnstile associated with wchan in turnstile chains and returns
 * it or NULL if no turnstile is found in chains. */
static turnstile_t *turnstile_lookup(void *wchan) {
  turnstile_chain_t *tc = TC_LOOKUP(wchan);
  turnstile_t *ts;
  LIST_FOREACH (ts, &tc->tc_turnstiles, ts_chain_link) {
    assert(ts->ts_state == USED_BLOCKED);
    if (ts->ts_wchan == wchan)
      return ts;
  }
  return NULL;
}

turnstile_t *turnstile_take(void *wchan) {
  assert(preempt_disabled());

  turnstile_t *ts = turnstile_lookup(wchan);

  if (ts != NULL)
    return ts;

  thread_t *td = thread_self();
  ts = td->td_turnstile;
  assert(ts != NULL);
  ts->ts_wchan = wchan;
  return ts;
}

void turnstile_give(turnstile_t *ts) {
  assert(preempt_disabled());

  thread_t *td = thread_self();
  if (ts == td->td_turnstile)
    ts->ts_wchan = NULL;
}

void turnstile_wait(turnstile_t *ts, thread_t *owner, const void *waitpt) {
  assert(preempt_disabled());
  assert(ts != NULL);

  thread_t *td = thread_self();

  if (ts != td->td_turnstile) {
    /* Hang off thread's turnstile from ts_free list as we're not the first
     * thread to block on waiting channel. */
    assert(ts->ts_state == USED_BLOCKED);

    TAILQ_INSERT_HEAD(&ts->ts_blocked, td, td_blockedq);
    adjust_thread_forward(ts, td);

    turnstile_t *nts = td->td_turnstile;
    td->td_turnstile = NULL;

    assert(owner == ts->ts_owner);
    assert(nts != NULL);
    assert(nts->ts_state == FREE_UNBLOCKED);

    nts->ts_state = FREE_BLOCKED;
    LIST_INSERT_HEAD(&ts->ts_free, nts, ts_free_link);
  } else {
    /* Provide thread's own turnstile to be used as head of list of threads
     * blocked on given waiting channel. */
    assert(TAILQ_EMPTY(&ts->ts_blocked));
    assert(LIST_EMPTY(&ts->ts_free));
    assert(ts->ts_owner == NULL);
    assert(ts->ts_wchan != NULL);
    assert(ts->ts_state == FREE_UNBLOCKED);

    ts->ts_owner = owner;

    turnstile_chain_t *tc = TC_LOOKUP(ts->ts_wchan);
    LIST_INSERT_HEAD(&owner->td_contested, ts, ts_contested_link);
    LIST_INSERT_HEAD(&tc->tc_turnstiles, ts, ts_chain_link);
    TAILQ_INSERT_TAIL(&ts->ts_blocked, td, td_blockedq);

    ts->ts_state = USED_BLOCKED;
  }

  switch_away(ts, waitpt);
}

void turnstile_broadcast(void *wchan) {
  assert(preempt_disabled());

  turnstile_t *ts = turnstile_lookup(wchan);

  assert(ts != NULL);
  assert(ts->ts_state == USED_BLOCKED);
  assert(ts->ts_owner == thread_self());
  assert(!TAILQ_EMPTY(&ts->ts_blocked));

  give_back_turnstiles(ts);
  unlend_self(ts);
  wakeup_blocked(&ts->ts_blocked);

  assert(ts->ts_state == FREE_UNBLOCKED);
}
