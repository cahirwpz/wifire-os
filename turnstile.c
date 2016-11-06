#include <stdc.h>
#include <sched.h>
#include <mutex.h>
#include <thread.h>

mtx_sleep_t mtx;
mtx_sleep_t mtx1;
mtx_sleep_t mtx2;
volatile int32_t value;

thread_t *td1;
thread_t *td2;
thread_t *td3;
thread_t *td4;
thread_t *td5;

void mtx_test_main() {
  for (size_t i = 0; i < 20000; i++) {
    mtx_sleep_lock(&mtx);
    value++;
    kprintf("%s: %ld\n", thread_self()->td_name, (long)value);
    mtx_sleep_unlock(&mtx);
  }
  while (1)
    ;
}

void mtx_test() {
  mtx_sleep_init(&mtx);
  mtx_sleep_init(&mtx);
  td1 = thread_create("td1", mtx_test_main);
  td2 = thread_create("td2", mtx_test_main);
  td3 = thread_create("td3", mtx_test_main);
  td4 = thread_create("td4", mtx_test_main);
  td5 = thread_create("td5", mtx_test_main);
  sched_add(td1);
  sched_add(td2);
  sched_add(td3);
  sched_add(td4);
  sched_add(td5);
  sched_run();
}

void deadlock_main1() {
  mtx_sleep_lock(&mtx1);
  for (size_t i = 0; i < 400000; i++)
    ;
  mtx_sleep_lock(&mtx2);
  kprintf("no deadlock!");
  mtx_sleep_unlock(&mtx2);
  mtx_sleep_unlock(&mtx1);
}

void deadlock_main2() {
  mtx_sleep_lock(&mtx2);
  for (size_t i = 0; i < 400000; i++)
    ;
  mtx_sleep_lock(&mtx1);
  kprintf("no deadlock!");
  mtx_sleep_unlock(&mtx1);
  mtx_sleep_unlock(&mtx2);
}

void deadlock_test() {
  mtx_sleep_init(&mtx1);
  mtx_sleep_init(&mtx2);
  td1 = thread_create("td1", deadlock_main1);
  td2 = thread_create("td2", deadlock_main2);
  sched_add(td1);
  sched_add(td2);
  sched_run();
  while (1)
    kprintf("elo!\n");
}

int main() {
  mtx_test();
  return 0;
}
