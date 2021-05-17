#define KL_LOG KL_SIGNAL
#include <sys/klog.h>
#include <sys/signal.h>
#include <sys/ucontext.h>

int sig_send(signo_t sig __unused, sigset_t *mask __unused,
             sigaction_t *sa __unused, ksiginfo_t *ksi __unused) {
  panic("not implemented!");
}

void sig_trap(ctx_t *ctx __unused, signo_t sig __unused) {
  panic("not implemented!");
}
