#include <sys/klog.h>
#include <sys/thread.h>

void thread_entry_setup(thread_t *td __unused, entry_fn_t target __unused,
                        void *arg __unused) {
  panic("not implemented!");
}
