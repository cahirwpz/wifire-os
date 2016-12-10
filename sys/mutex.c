#include <stdc.h>
#include <sync.h>
#include <mutex.h>
#include <thread.h>

#define MTX_UNOWNED 0

bool mtx_owned(mtx_t *mtx) {
  return (mtx->mtx_state == (uint32_t)thread_self());
}

void mtx_init(mtx_t *m, unsigned type) {
  m->m_owner = NULL;
  m->m_count = 0;
  m->m_type = type;
  turnstile_init(&m->m_turnstile);
}

void mtx_lock(mtx_t *m) {
  if (mtx_owned(m)) {
    assert(m->m_type & MT_RECURSE);
    m->m_count++;
    return;
  }

  cs_enter();
  while (m->m_owner != NULL)
    turnstile_wait(&m->m_turnstile);
  m->m_owner = thread_self();
  cs_leave();
}

void mtx_unlock(mtx_t *m) {
  assert(mtx_owned(m));

  if (m->m_count > 0) {
    m->m_count--;
    return;
  }

  cs_enter();
  m->m_owner = NULL;
  turnstile_signal(&m->m_turnstile);
  cs_leave();
}
