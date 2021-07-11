#include <sys/mutex.h>
#include <sys/kenv.h>
#include <sys/time.h>
#include <sys/libkern.h>
#include <sys/thread.h>
#include <sys/klog.h>
#include <sys/ktest.h>
#include <sys/interrupt.h>

#define KL_SIZE 1024

typedef struct klog_entry {
  bintime_t kl_timestamp;
  tid_t kl_tid;
  unsigned kl_line;
  const char *kl_file;
  klog_origin_t kl_origin;
  const char *kl_format;
  uintptr_t kl_params[6];
} klog_entry_t;

typedef struct klog {
  klog_entry_t array[KL_SIZE];
  atomic_uint mask;
  volatile unsigned first;
  volatile unsigned last;
  bool repeated;
  int prev;
} klog_t;

static klog_t klog;
static SPIN_DEFINE(klog_lock, LK_RECURSIVE);

static const char *subsystems[] = {
  [KL_SLEEPQ] = "sleepq",   [KL_CALLOUT] = "callout", [KL_INIT] = "init",
  [KL_PMAP] = "pmap",       [KL_VM] = "vm",           [KL_KMEM] = "kmem",
  [KL_VMEM] = "vmem",       [KL_LOCK] = "lock",       [KL_SCHED] = "sched",
  [KL_THREAD] = "thread",   [KL_INTR] = "intr",       [KL_DEV] = "dev",
  [KL_USB] = "usb",         [KL_VFS] = "vfs",         [KL_PROC] = "proc",
  [KL_SYSCALL] = "syscall", [KL_USER] = "user",       [KL_TEST] = "test",
  [KL_SIGNAL] = "signal",   [KL_FILESYS] = "filesys", [KL_TIME] = "time",
  [KL_FILE] = "file",       [KL_TTY] = "tty",         [KL_UNDEF] = "???",
};

void init_klog(void) {
  const char *mask = kenv_get("klog-mask");
  klog.mask = mask ? (unsigned)strtol(mask, NULL, 16) : KL_DEFAULT_MASK;
  klog.first = 0;
  klog.last = 0;
  klog.repeated = 0;
  klog.prev = -1;
}

static inline unsigned next(unsigned i) {
  return (i + 1) % KL_SIZE;
}

static void klog_entry_dump(klog_entry_t *entry) {
  if (entry->kl_origin == KL_UNDEF)
    kprintf("[%s:%d] ", entry->kl_file, entry->kl_line);
  else
    kprintf("[%s] ", subsystems[entry->kl_origin]);
  kprintf(entry->kl_format, entry->kl_params[0], entry->kl_params[1],
          entry->kl_params[2], entry->kl_params[3], entry->kl_params[4],
          entry->kl_params[5]);
  kprintf("\n");
}

void klog_append(klog_origin_t origin, const char *file, unsigned line,
                 const char *format, uintptr_t arg1, uintptr_t arg2,
                 uintptr_t arg3, uintptr_t arg4, uintptr_t arg5,
                 uintptr_t arg6) {
  if (!(KL_MASK(origin) & atomic_load(&klog.mask)))
    return;

  tid_t tid = thread_self()->td_tid;

  WITH_SPIN_LOCK (&klog_lock) {
    bintime_t now = binuptime();

    klog_entry_t *prev = (klog.prev >= 0) ? &klog.array[klog.prev] : NULL;

    /* Do not store repeating log messages, just count them. */
    if (prev) {
      bool repeats =
        (prev->kl_params[0] == arg1) && (prev->kl_params[1] == arg2) &&
        (prev->kl_params[2] == arg3) && (prev->kl_params[3] == arg4) &&
        (prev->kl_params[4] == arg5) && (prev->kl_params[5] == arg6) &&
        (prev->kl_origin == origin) && (prev->kl_file == file) &&
        (prev->kl_line == line) && (prev->kl_tid = tid);

      if (repeats) {
        if (!klog.repeated) {
          int old_prev = klog.prev;
          klog.prev = -1;
          klog_append(prev->kl_origin, prev->kl_file, prev->kl_line,
                      "Last message repeated %d times.", 0, 0, 0, 0, 0, 0);
          klog.prev = old_prev;
          klog.repeated = true;
        }

        prev = &klog.array[next(klog.prev)];
        prev->kl_timestamp = now;
        prev->kl_params[0]++;
        return;
      }

      klog.repeated = false;
    }

    klog_entry_t *entry = &klog.array[klog.last];
    *entry = (klog_entry_t){.kl_timestamp = now,
                            .kl_tid = tid,
                            .kl_line = line,
                            .kl_file = file,
                            .kl_origin = origin,
                            .kl_format = format,
                            .kl_params = {arg1, arg2, arg3, arg4, arg5, arg6}};

    klog.prev = klog.last;
    klog.last = next(klog.last);
    if (klog.first == klog.last)
      klog.first = next(klog.first);
  }
}

unsigned klog_setmask(unsigned newmask) {
  return atomic_exchange(&klog.mask, newmask);
}

void klog_dump(void) {
  klog_entry_t entry;

  while (klog.first != klog.last) {
    WITH_SPIN_LOCK (&klog_lock) {
      entry = klog.array[klog.first];
      klog.first = next(klog.first);
    }
    klog_entry_dump(&entry);
  }
}

void klog_clear(void) {
  klog.first = 0;
  klog.last = 0;
}

/*
 * @brief Permanently lock the kernel.
 *
 * We used to terminate the current thread. That's not a great way to panic,
 * since other threads will continue executing, so our panic might go unnoticed.
 */
static __noreturn void halt(void) {
  intr_disable();
  for (;;)
    continue;
}

__noreturn void klog_panic(klog_origin_t origin, const char *file,
                           unsigned line, const char *format, uintptr_t arg1,
                           uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                           uintptr_t arg5, uintptr_t arg6) {
  klog_append(origin, file, line, format, arg1, arg2, arg3, arg4, arg5, arg6);
  ktest_log_failure();
  halt();
}

__noreturn void klog_assert(klog_origin_t origin, const char *file,
                            unsigned line, const char *expr) {
  klog_append(origin, file, line, "Assertion \"%s\" failed!", (intptr_t)expr, 0,
              0, 0, 0, 0);
  ktest_log_failure();
  halt();
}
