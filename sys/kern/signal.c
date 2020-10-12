#define KL_LOG KL_SIGNAL
#include <sys/klog.h>
#include <sys/mimiker.h>
#include <sys/libkern.h>
#include <sys/signal.h>
#include <sys/thread.h>
#include <sys/errno.h>
#include <sys/sleepq.h>
#include <sys/proc.h>
#include <sys/wait.h>
#include <sys/sched.h>

/*!\brief Signal properties.
 *
 * Non-mutually-exclusive properties can be bitwise-ORed together.
 * \note Properties that have set bits in common with #SA_DEFACT_MASK
 * are mutually exclusive and denote the default action of the signal. */
/* clang-format off */
typedef enum {
  SA_IGNORE = 0x1,
  SA_KILL = 0x2,
  SA_CONT = 0x3, /* Continue a stopped process */
  SA_STOP = 0x4, /* Stop a process */
  SA_CANTMASK = 0x8
} sigprop_t;

/*!\brief Mask used to extract the default action from a sigprop_t. */
#define SA_DEFACT_MASK 0x7

/* clang-format off */
static const sigprop_t sig_properties[NSIG] = {
  [SIGHUP] = SA_KILL,
  [SIGINT] = SA_KILL,
  [SIGILL] = SA_KILL,
  [SIGABRT] = SA_KILL,
  [SIGFPE] = SA_KILL,
  [SIGSEGV] = SA_KILL,
  [SIGKILL] = SA_KILL | SA_CANTMASK,
  [SIGTERM] = SA_KILL,
  [SIGSTOP] = SA_STOP | SA_CANTMASK,
  [SIGCONT] = SA_CONT,
  [SIGCHLD] = SA_IGNORE,
  [SIGUSR1] = SA_KILL,
  [SIGUSR2] = SA_KILL,
  [SIGBUS] = SA_KILL,
};

static const sigset_t cantmask = {__sigmask(SIGKILL) | __sigmask(SIGSTOP)};

static const char *sig_name[NSIG] = {
  [SIGHUP] = "SIGHUP",
  [SIGINT] = "SIGINT",
  [SIGILL] = "SIGILL",
  [SIGABRT] = "SIGABRT",
  [SIGFPE] = "SIGFPE",
  [SIGSEGV] = "SIGSEGV",
  [SIGKILL] = "SIGKILL",
  [SIGTERM] = "SIGTERM",
  [SIGSTOP] = "SIGSTOP",
  [SIGCONT] = "SIGCONT",
  [SIGCHLD] = "SIGCHLD",
  [SIGUSR1] = "SIGUSR1",
  [SIGUSR2] = "SIGUSR2",
  [SIGBUS] = "SIGBUS",
};
/* clang-format on */

/* Default action for a signal. */
static sigprop_t defact(signo_t sig) {
  assert(sig <= NSIG);
  return sig_properties[sig] & SA_DEFACT_MASK;
}

int do_sigaction(signo_t sig, const sigaction_t *act, sigaction_t *oldact) {
  proc_t *p = proc_self();

  if (sig >= NSIG)
    return EINVAL;

  if (sig_properties[sig] & SA_CANTMASK)
    return EINVAL;

  WITH_PROC_LOCK(p) {
    if (oldact != NULL)
      memcpy(oldact, &p->p_sigactions[sig], sizeof(sigaction_t));
    if (act != NULL)
      memcpy(&p->p_sigactions[sig], act, sizeof(sigaction_t));
  }

  return 0;
}

static signo_t sig_pending(thread_t *td) {
  proc_t *p = td->td_proc;

  assert(p != NULL);
  assert(mtx_owned(&p->p_lock));

  sigset_t unblocked = td->td_sigpend;
  __sigminusset(&td->td_sigmask, &unblocked);

  signo_t ret = __sigfindset(&unblocked);
  assert(ret < NSIG);
  return ret;
}

int do_sigprocmask(int how, const sigset_t *set, sigset_t *oset) {
  proc_t *proc = proc_self();
  thread_t *td = proc->p_thread;
  assert(mtx_owned(&proc->p_lock));

  sigset_t *const mask = &td->td_sigmask;

  if (oset != NULL)
    *oset = *mask;

  if (set == NULL)
    return 0;

  sigset_t nset = *set;
  __sigminusset(&cantmask, &nset);

  if (how == SIG_BLOCK) {
    __sigplusset(&nset, mask);
    return 0;
  }

  if (how == SIG_UNBLOCK) {
    __sigminusset(&nset, mask);
  } else if (how == SIG_SETMASK) {
    *mask = nset;
  } else {
    return EINVAL;
  }

  if (sig_pending(td)) {
    WITH_SPIN_LOCK (td->td_lock)
      td->td_flags |= TDF_NEEDSIGCHK;
  }

  return 0;
}

int do_sigsuspend(proc_t *p, const sigset_t *mask) {
  thread_t *td = thread_self();
  assert(td->td_proc == p);

  assert((td->td_pflags & TDP_OLDSIGMASK) == 0);
  td->td_oldsigmask = td->td_sigmask;
  td->td_pflags |= TDP_OLDSIGMASK;

  WITH_PROC_LOCK(p) {
    do_sigprocmask(SIG_SETMASK, mask, NULL);

    /*
     * We want the sleep to be interrupted only if there's an actual signal
     * to be handled, but _sleepq_wait() returns immediately if TDF_NEEDSIGCHK
     * is set (without checking whether there's an actual pending signal),
     * so we clear the flag here if there are no real pending signals.
     */
    WITH_SPIN_LOCK (td->td_lock) {
      if (sig_pending(td))
        td->td_flags |= TDF_NEEDSIGCHK;
      else
        td->td_flags &= ~TDF_NEEDSIGCHK;
    }
  }

  int error;
  error = sleepq_wait_intr(&td->td_sigmask, "sigsuspend()");
  assert(error = EINTR);

  return EINTR;
}

/*
 * NOTE: This is a very simple implementation! Unimplemented features:
 * - Thread tracing and debugging
 * - Multiple threads in a process
 * These limitations (plus the fact that we currently have very little thread
 * states) make the logic of sending a signal very simple!
 */
void sig_kill(proc_t *p, signo_t sig) {
  assert(p != NULL);
  assert(mtx_owned(all_proc_mtx));
  assert(mtx_owned(&p->p_lock));
  assert(sig < NSIG);

  /* Zombie processes shouldn't accept any signals. */
  if (p->p_state == PS_ZOMBIE)
    return;

  thread_t *td = p->p_thread;

  sig_t handler = p->p_sigactions[sig].sa_handler;
  bool continued = sig == SIGCONT || sig == SIGKILL;

  /* If the signal is ignored, don't even bother posting it,
   * unless it's waking up a stopped process. */
  if (p->p_state == PS_STOPPED && continued) {
    p->p_state = PS_NORMAL;
    p->p_flags &= ~PF_STOPPED;
    p->p_flags |= PF_CONTINUED;
    cv_broadcast(&p->p_parent->p_waitcv);
  } else if (handler == SIG_IGN ||
             (defact(sig) == SA_IGNORE && handler == SIG_DFL)) {
    return;
  }

  /* If stopping or continuing,
   * remove pending signals with the opposite effect. */
  if (sig == SIGSTOP)
    __sigdelset(&td->td_sigpend, SIGCONT);

  if (sig == SIGCONT) {
    __sigdelset(&td->td_sigpend, SIGSTOP);

    /* In case of SIGCONT, make it pending only if the process catches it. */
    if (handler != SIG_IGN && handler != SIG_DFL)
      __sigaddset(&td->td_sigpend, SIGCONT);
  } else {
    /* Every other signal is marked as pending. */
    __sigaddset(&td->td_sigpend, sig);
  }

  /* Don't wake up the target thread if it blocks the signal being sent.
   * Exception: SIGCONT wakes up stopped threads even if it's blocked. */
  if (__sigismember(&td->td_sigmask, sig)) {
    if (continued)
      WITH_SPIN_LOCK (td->td_lock)
        if (td_is_stopped(td))
          sched_wakeup(td, 0);
  } else {
    spin_lock(td->td_lock);
    td->td_flags |= TDF_NEEDSIGCHK;
    /* If the thread is sleeping interruptibly (!), wake it up, so that it
     * continues execution and the signal gets delivered soon. */
    if (td_is_interruptible(td)) {
      /* XXX Maybe TDF_NEEDSIGCHK should be protected by a different lock? */
      spin_unlock(td->td_lock);
      sleepq_abort(td); /* Locks & unlocks td_lock */
      return;
    }
    if (td_is_stopped(td) && continued)
      sched_wakeup(td, 0);
    spin_unlock(td->td_lock);
  }
}

int sig_check(thread_t *td) {
  proc_t *p = td->td_proc;

  assert(p != NULL);
  assert(mtx_owned(&p->p_lock));

  signo_t sig = NSIG;
  while (true) {
    sig = sig_pending(td);
    if (sig == 0) {
      /* No pending signals, signal checking done. */
      WITH_SPIN_LOCK (td->td_lock)
        td->td_flags &= ~TDF_NEEDSIGCHK;
      return 0;
    }
    __sigdelset(&td->td_sigpend, sig);

    sig_t handler = p->p_sigactions[sig].sa_handler;

    if (handler == SIG_IGN)
      continue;
    if (handler == SIG_DFL && defact(sig) == SA_IGNORE)
      continue;

    /* If we reached here, then the signal has to be posted. */
    return sig;
  }

  __unreachable();
}

void sig_post(signo_t sig) {
  thread_t *td = thread_self();
  proc_t *p = proc_self();

  assert(p != NULL);
  assert(mtx_owned(all_proc_mtx));
  assert(mtx_owned(&p->p_lock));

  sigaction_t *sa = &p->p_sigactions[sig];

  assert(sa->sa_handler != SIG_IGN);

  if (sa->sa_handler == SIG_DFL && defact(sig) == SA_KILL) {
    /* Terminate this thread as result of a signal. */
    mtx_unlock(all_proc_mtx);
    sig_exit(td, sig);
    __unreachable();
  }

  if (sa->sa_handler == SIG_DFL && defact(sig) == SA_STOP) {
    klog("Stopping thread %lu in process PID(%d)", td->td_tid, p->p_pid);
    p->p_state = PS_STOPPED;
    p->p_flags &= ~PF_CONTINUED;
    p->p_flags |= PF_STOPPED;
    cv_broadcast(&p->p_parent->p_waitcv);
    proc_unlock(p);
    WITH_MTX_LOCK (&p->p_parent->p_lock)
      sig_kill(p->p_parent, SIGCHLD);
    mtx_unlock(all_proc_mtx);
    proc_lock(p);
    if (p->p_state == PS_STOPPED) {
      spin_lock(td->td_lock);
      td->td_state = TDS_STOPPED;
      /* We're holding a spinlock, so we can't be preempted here. */
      /* Release locks before switching out! */
      proc_unlock(p);
      sched_switch();
    } else {
      proc_unlock(p);
    }
    /* Reacquire locks in correct order! */
    mtx_lock(all_proc_mtx);
    proc_lock(p);
    return;
  }

  klog("Post signal %s (handler %p) to thread %lu in process PID(%d)",
       sig_name[sig], sa->sa_handler, td->td_tid, p->p_pid);

  sigset_t *return_mask;
  if (td->td_pflags & TDP_OLDSIGMASK) {
    return_mask = &td->td_oldsigmask;
    td->td_pflags &= ~TDP_OLDSIGMASK;
  } else {
    return_mask = &td->td_sigmask;
  }

  /* Normally the `sig_post` would have more to do, but our signal
   * implementation is very limited for now. All `sig_post` has to do is to
   * pass `sa` to platform-specific `sig_send`. */
  sig_send(sig, return_mask, sa);

  /* Set handler's signal mask. */
  __sigplusset(&sa->sa_mask, &td->td_sigmask);
  __sigaddset(&td->td_sigmask, sig);
  __sigminusset(&cantmask, &td->td_sigmask);
}

__noreturn void sig_exit(thread_t *td, signo_t sig) {
  klog("PID(%d) terminated due to signal %s ", td->td_proc->p_pid,
       sig_name[sig]);
  proc_exit(MAKE_STATUS_SIG_TERM(sig));
}

int do_sigreturn(void) {
  return sig_return();
}
