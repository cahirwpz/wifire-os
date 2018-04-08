#include <pool.h>
#include <thread.h>
#include <spinlock.h>
#include <sched.h>
#include <turnstile.h>
#include <queue.h>

#define TC_TABLESIZE 256 /* Must be power of 2. */
#define TC_MASK (TC_TABLESIZE - 1)
#define TC_SHIFT 8
#define TC_HASH(wc)                                                            \
  ((((uintptr_t)(wc) >> TC_SHIFT) ^ (uintptr_t)(wc)) & TC_MASK)
#define TC_LOOKUP(wc) &turnstile_chains[TC_HASH(wc)]

TAILQ_HEAD(tailq_thread, thread);

typedef struct turnstile {
  spinlock_t ts_lock;            /* spinlock for this turnstile */
  LIST_ENTRY(turnstile) ts_hash; /* link on turnstile chain or ts_free list */
  LIST_ENTRY(turnstile)
  ts_link; /* link on td_contested (turnstiles attached
            * to locks that a thread owns) */
  LIST_HEAD(, turnstile)
  ts_free;                           /* free turnstiles left by threads
                                      * blocked on this turnstile */
  struct tailq_thread ts_blocked[2]; /* blocked threads sorted by
                                      * decreasing active priority */
  struct tailq_thread ts_pending;    /* threads awakened and waiting to be
                                      * put on run queue */
  void *ts_wchan;                    /* waiting channel */
  thread_t *ts_owner;                /* who owns the lock */
} turnstile_t;

typedef struct turnstile_chain {
  spinlock_t tc_lock;
  LIST_HEAD(, turnstile) tc_turnstiles;
} turnstile_chain_t;

spinlock_t td_contested_lock;
static turnstile_chain_t turnstile_chains[TC_TABLESIZE];

static pool_t P_TURNSTILE;

static thread_t *turnstile_first_waiter(turnstile_t *);
static int turnstile_adjust_thread(turnstile_t *ts, thread_t *td);

static void turnstile_ctor(turnstile_t *ts) {
  ts->ts_lock = SPINLOCK_INITIALIZER();
  LIST_INIT(&ts->ts_free);
  TAILQ_INIT(&ts->ts_blocked[TS_EXCLUSIVE_QUEUE]);
  TAILQ_INIT(&ts->ts_blocked[TS_SHARED_QUEUE]);
  TAILQ_INIT(&ts->ts_pending);
  ts->ts_wchan = NULL;
  ts->ts_owner = NULL;
}

void turnstile_init(void) {
  for (int i = 0; i < TC_TABLESIZE; i++) {
    turnstile_chain_t *tc = &turnstile_chains[i];
    LIST_INIT(&tc->tc_turnstiles);
    tc->tc_lock = SPINLOCK_INITIALIZER();
  }

  td_contested_lock = SPINLOCK_INITIALIZER();

  /* we FreeBSD tu jest coś takiego:
   * LIST_INIT(thread0.td_owned);
   * thread0.td_turnstile = NULL;
   * nie wiem, czy u nas też powinno
  */

  P_TURNSTILE = pool_create("turnstile", sizeof(turnstile_t),
                            (pool_ctor_t)turnstile_ctor, NULL);
}

turnstile_t *turnstile_alloc(void) {
  return pool_alloc(P_TURNSTILE, 0);
}

void turnstile_destroy(turnstile_t *ts) {
  pool_free(P_TURNSTILE, ts);
}

/* Adjusts thread's position on ts_blocked queue after its priority
 * has been changed.
 * Return values:
 * - 1  -- moved towards the end of the list
 * - 0  -- didn't move
 * - -1 -- moved towards the head of the list
 */
// TODO consider locks
static int turnstile_adjust_thread(turnstile_t *ts, thread_t *td) {
  thread_t *n = TAILQ_NEXT(td, td_turnstilesq);
  thread_t *p = TAILQ_PREV(td, tailq_thread, td_turnstilesq);

  int moved = 0; // 1 - forward, 0 - not moved, -1 - backward
  int queue = td->td_turnstileq_ind;

  if (n != NULL && n->td_prio > td->td_prio) {
    moved = 1;
    TAILQ_REMOVE(&ts->ts_blocked[queue], td, td_turnstilesq);

    while (n != NULL && n->td_prio > td->td_prio) {
      n = TAILQ_NEXT(n, td_turnstilesq);
    }

    if (n != NULL)
      TAILQ_INSERT_BEFORE(n, td, td_turnstilesq);
    else
      TAILQ_INSERT_TAIL(&ts->ts_blocked[queue], td, td_turnstilesq);
  }

  if (p != NULL && p->td_prio < td->td_prio) {
    assert(moved == 0);
    moved = -1;
    TAILQ_REMOVE(&ts->ts_blocked[queue], td, td_turnstilesq);

    while (p != NULL && p->td_prio < td->td_prio) {
      p = TAILQ_PREV(p, tailq_thread, td_turnstilesq);
    }

    if (p != NULL)
      TAILQ_INSERT_AFTER(&ts->ts_blocked[queue], p, td, td_turnstilesq);
    else
      TAILQ_INSERT_HEAD(&ts->ts_blocked[queue], td, td_turnstilesq);
  }

  return moved;
}

/* Walks the chain of turnstiles and their owners to propagate the priority
 * of the thread being blocked to all the threads holding locks that have to
 * release their locks before this thread can run again.
 */
/*static*/ void propagate_priority(thread_t *td) {
  // TODO jakaś blokada na td?
  turnstile_t *ts = td->td_blocked;
  td_prio_t prio = td->td_prio;
  spin_acquire(&ts->ts_lock);

  while (1) {
    td = ts->ts_owner;
    if (td == NULL) {
      /* read lock with no owner */
      spin_release(&ts->ts_lock);
      return;
    }

    spin_release(&ts->ts_lock);

    if (td->td_prio >= prio) {
      // thread_unlock(td); -- to robi spin_release(&sched_lock);
      // ale my nie mamy sched_lock
      return;
    }

    sched_lend_prio(td, prio);

    /* lock holder is in runq or running */
    if (td->td_state == TDS_READY || td->td_state == TDS_RUNNING) {
      assert(td->td_blocked == NULL);
      // thread_unlock(td);
      return;
    }

    ts = td->td_blocked;
    // z jakiegoś powodu zakładamy tutaj, że td jest zalokowany na locku
    // a jeżeli śpi to deadlock i chcemy się wywalić
    assert(ts != NULL);

    /* resort td on the list if needed */
    if (!turnstile_adjust_thread(ts, td)) {
      /* td is in fact not blocked on any lock */
      spin_release(&ts->ts_lock);
      return;
    }
  }
}

/* td - blocked thread (on some turnstile)
 * Gotta:
 * - sort the list on which =td= is (=ts->ts_blocked=)
 * - check the priority of the thread owning the lock
 *   - we have to change it only if =td= was the first waiter and
 *     isn't anymore or wasn't then but now is
 */
// TODO consider locks
void turnstile_adjust(thread_t *td, td_prio_t oldprio) {
  turnstile_t *ts = td->td_blocked;

  // we FreeBSD jest jakieś zamieszanie odnośnie td->td_turnstile != NULL,
  // bo cośtam cośtam SMP. Chyba się nie przejmujemy.

  thread_t *first = turnstile_first_waiter(ts);
  turnstile_adjust_thread(ts, td);
  thread_t *new_first = turnstile_first_waiter(ts);

  if (first == new_first)
    return;
  else {
    // we FreeBSD poprawiają priorytet jedynie, jeśli nowy jest lepszy
    // (nie obniżają już pożyczonych priorytetów)

    // If =new_first= isn't =first= then =td= is either of them.
    // If =td='s priority was increased then it's =new_first= and
    // we only have to propagate the new (higher) priority
    if (td->td_prio > oldprio) {
      propagate_priority(new_first);
    } else {
      // patrz wyżej
      // TODO? remove the old lent priority and propagate the new one
    }
  }
}

void turnstile_claim(turnstile_t *ts) {
  // TODO
}

void turnstile_wait(turnstile_t *ts, thread_t *owner, int queue) {
  // TODO
}

int turnstile_signal(turnstile_t *ts, int queue) {
  // TODO
  return 42;
}

void turnstile_broadcast(turnstile_t *ts, int queue) {
  // TODO
}

void turnstile_unpend(turnstile_t *ts, int owner_type) {
  // TODO
  // lol, owner_type jest nieużywane we FreeBSD
}

// TODO consider locks
void turnstile_disown(turnstile_t *ts) {
  thread_t *td = ts->ts_owner;

  // TODO who should be the new owner of the turnstile?
  // Do we just leave it NULL so that calling procedure will handle it?
  // Or should the next thread acquiring the lock do something?
  // NOTE (probably the last one?)
  ts->ts_owner = NULL;

  LIST_REMOVE(ts, ts_link);

  td_prio_t new_prio = td->td_base_prio;

  turnstile_t *owned;
  LIST_FOREACH(owned, &td->td_contested, ts_link) {
    // NOTE one waiter must exist because otherwise the turnstile wouldn't exist
    new_prio = max(new_prio, turnstile_first_waiter(owned)->td_prio);
  }

  sched_unlend_prio(td, new_prio);
}

/* assumes that we own td_contested_lock */
/*static*/ void turnstile_setowner(turnstile_t *ts, thread_t *owner) {
  assert(spin_owned(&td_contested_lock));
  assert(ts->ts_owner == NULL);

  /* a shared lock might not have an owner */
  if (owner == NULL)
    return;

  ts->ts_owner = owner;
  LIST_INSERT_HEAD(&owner->td_contested, ts, ts_link);
}

void turnstile_chain_lock(void *wchan) {
  turnstile_chain_t *tc = TC_LOOKUP(wchan);
  spin_acquire(&tc->tc_lock);
}

void turnstile_chain_unlock(void *wchan) {
  turnstile_chain_t *tc = TC_LOOKUP(wchan);
  spin_release(&tc->tc_lock);
}

/* Return a pointer to the thread waiting on this turnstile with the
 * most important priority or NULL if the turnstile has no waiters.
 */
static thread_t *turnstile_first_waiter(turnstile_t *ts) {
  thread_t *std = TAILQ_FIRST(&ts->ts_blocked[TS_SHARED_QUEUE]);
  thread_t *xtd = TAILQ_FIRST(&ts->ts_blocked[TS_EXCLUSIVE_QUEUE]);
  if (xtd == NULL || (std != NULL && std->td_prio > xtd->td_prio))
    return std;
  return xtd;
}

/* gets turnstile associated with wchan
 * and acquires tc_lock and ts_lock */
turnstile_t *turnstile_trywait(void *wchan) {
  turnstile_chain_t *tc = TC_LOOKUP(wchan);
  spin_acquire(&tc->tc_lock);

  turnstile_t *ts;
  LIST_FOREACH(ts, &tc->tc_turnstiles, ts_hash) {
    if (ts->ts_wchan == wchan) {
      spin_acquire(&ts->ts_lock);
      return ts;
    }
  }

  ts = thread_self()->td_turnstile;
  assert(ts != NULL);
  spin_acquire(&ts->ts_lock);

  assert(ts->ts_wchan == NULL);
  ts->ts_wchan = wchan;

  return ts;
}
/*
static thread_t *turnstile_first_waiter(turnstile_t *ts) {
  const int queues_count = 2;
  int queues[queues_count] = {TS_EXCLUSIVE_QUEUE, TS_SHARED_QUEUE};

  thread_t *found = NULL;

  for (int i = 0; i < queues_count; i++) {
    thread_t local_first = TAILQ_FIRST(&ts->ts_blocked[queues[i]]);

    if (local_first != NULL &&
        (found == NULL || local_first->td_prio < found->td_prio))
      found = local_first;
  }

  return found;
}
*/

/* cancels turnstile_trywait and releases ts_lock and tc_lock */
void turnstile_cancel(turnstile_t *ts) {
  spin_release(&ts->ts_lock);
  void *wchan = ts->ts_wchan;
  if (ts == thread_self()->td_turnstile)
    ts->ts_wchan = NULL;

  turnstile_chain_t *tc = TC_LOOKUP(wchan);
  spin_release(&tc->tc_lock);
}

/* looks for turnstile associated with wchan in turnstile chains
 * assuming that we own tc_lock and returns NULL is no turnstile
 * is found in chain;
 * the function acquires ts_lock */
turnstile_t *turnstile_lookup(void *wchan) {
  turnstile_chain_t *tc = TC_LOOKUP(wchan);
  assert(spin_owned(&tc->tc_lock));

  turnstile_t *ts;
  LIST_FOREACH(ts, &tc->tc_turnstiles, ts_hash) {
    if (ts->ts_wchan == wchan) {
      spin_acquire(&ts->ts_lock);
      return ts;
    }
  }
  return NULL;
}
