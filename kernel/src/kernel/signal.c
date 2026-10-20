#include <kernel/signal.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/syscall.h>
#include <kernel/timesys.h>
#include <kernel/wait.h>
#include <kernel/time.h>
#include <kernel/spinlock.h>
#include <kernel/log.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/interrupts.h>
#include <lib/string.h>
#include <mm/memory.h>
#include <mm/vmm.h>
#include <mm/uaccess.h>
#include <arch/x86_64/fpu.h>

#define RED_ZONE       128
#define SYSCALL_VECTOR 0x80
#define NO_RESTART     ((uint64_t)-1)

#define USER_RFLAGS_MASK 0xCD5ull
#define RFLAGS_FIXED     0x202ull

struct linux_sigcontext {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, eflags;
    uint16_t cs, gs, fs, ss;
    uint64_t err, trapno, oldmask, cr2;
    uint64_t fpstate;
    uint64_t reserved[8];
};

struct linux_ucontext {
    uint64_t uc_flags;
    uint64_t uc_link;
    uint64_t ss_sp;
    int32_t ss_flags;
    int32_t ss_pad;
    uint64_t ss_size;
    struct linux_sigcontext mc;
    uint64_t sigmask;
};

struct sigframe {
    uint64_t pretcode;
    struct linux_ucontext uc;
    struct user_siginfo info;
};

#define RFLAGS_DF 0x400ull
#define RFLAGS_TF 0x100ull
#define RFLAGS_RF 0x10000ull

static const struct sig_src kernel_src = { SI_KERNEL, 0, 0, 0, 0 };

static const char *const names[NSIG] = {
    [SIGHUP] = "SIGHUP",     [SIGINT] = "SIGINT",       [SIGQUIT] = "SIGQUIT",
    [SIGILL] = "SIGILL",     [SIGTRAP] = "SIGTRAP",     [SIGABRT] = "SIGABRT",
    [SIGBUS] = "SIGBUS",     [SIGFPE] = "SIGFPE",       [SIGKILL] = "SIGKILL",
    [SIGUSR1] = "SIGUSR1",   [SIGSEGV] = "SIGSEGV",     [SIGUSR2] = "SIGUSR2",
    [SIGPIPE] = "SIGPIPE",   [SIGALRM] = "SIGALRM",     [SIGTERM] = "SIGTERM",
    [SIGSTKFLT] = "SIGSTKFLT", [SIGCHLD] = "SIGCHLD",   [SIGCONT] = "SIGCONT",
    [SIGSTOP] = "SIGSTOP",   [SIGTSTP] = "SIGTSTP",     [SIGTTIN] = "SIGTTIN",
    [SIGTTOU] = "SIGTTOU",   [SIGURG] = "SIGURG",       [SIGXCPU] = "SIGXCPU",
    [SIGXFSZ] = "SIGXFSZ",   [SIGVTALRM] = "SIGVTALRM", [SIGPROF] = "SIGPROF",
    [SIGWINCH] = "SIGWINCH", [SIGIO] = "SIGIO",         [SIGPWR] = "SIGPWR",
    [SIGSYS] = "SIGSYS",
};

const char *signal_name(int sig)
{
    if (sig > 0 && sig < NSIG && names[sig])
        return names[sig];
    return "?";
}

static bool default_ignores(int sig)
{
    return sig == SIGCHLD || sig == SIGURG || sig == SIGWINCH || sig == SIGCONT || sig == SIGSTOP ||
           sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU;
}

void signal_init_process(struct process *p)
{
    p->sig_pending = 0;
    p->alarm_at = 0;
    memset(p->sig_handlers, 0, SIG_TABLE_SIZE);
}

void signal_reset_on_exec(struct process *p)
{
    uint64_t flags = spin_lock_irqsave(&p->tlock);
    for (int i = 0; i < NSIG - 1; i++) {
        if (p->sig_handlers[i].sa_handler != SIG_IGN)
            p->sig_handlers[i].sa_handler = SIG_DFL;
        p->sig_handlers[i].sa_flags = 0;
        p->sig_handlers[i].sa_mask = 0;
    }
    spin_unlock_irqrestore(&p->tlock, flags);
}

void signal_copy_handlers(struct process *dst, struct process *src)
{
    uint64_t flags = spin_lock_irqsave(&src->tlock);
    memcpy(dst->sig_handlers, src->sig_handlers, SIG_TABLE_SIZE);
    spin_unlock_irqrestore(&src->tlock, flags);
}

static bool ignored(struct process *p, int sig)
{
    if (sig == SIGKILL)
        return false;
    uint64_t action = SIGACT(p, sig)->sa_handler;
    return action == SIG_IGN || (action == SIG_DFL && default_ignores(sig));
}

static bool blocked_by_all(struct process *p, int sig)
{
    if (!p->threads)
        return false;
    for (struct thread *t = p->threads; t; t = t->proc_next)
        if (!((t->sig_blocked | t->sig_waiting) & SIGMASK(sig)))
            return false;
    return true;
}

static void wake_thread(struct thread *t)
{
    uint64_t flags = irq_save();
    sched_runq_lock();
    wait_unlink(t);
    sched_make_ready(t);
    sched_runq_unlock();
    irq_restore(flags);
}

static void wake_receiver_locked(struct process *p, int sig)
{
    struct thread *target = NULL;
    struct thread *me = thread_current();
    for (struct thread *t = p->threads; t; t = t->proc_next) {
        if (t->sig_blocked & SIGMASK(sig))
            continue;
        if (!target || t == me)
            target = t;
    }
    if (target)
        wake_thread(target);
}

void signal_retarget(struct process *p, uint64_t sigs)
{
    while (sigs) {
        uint64_t flags = spin_lock_irqsave(&p->tlock);
        wake_receiver_locked(p, __builtin_ctzll(sigs) + 1);
        spin_unlock_irqrestore(&p->tlock, flags);
        sigs &= sigs - 1;
    }
}

static void set_mask(struct thread *self, uint64_t mask)
{
    struct process *p = self->process;
    mask &= ~SIG_UNBLOCKABLE;
    if (!p) {
        self->sig_blocked = mask;
        return;
    }
    uint64_t flags = spin_lock_irqsave(&p->tlock);
    uint64_t before = self->sig_blocked;
    self->sig_blocked = mask;
    uint64_t moved = __atomic_load_n(&p->sig_pending, __ATOMIC_SEQ_CST) & mask & ~before;
    spin_unlock_irqrestore(&p->tlock, flags);
    signal_retarget(p, moved);
}

int signal_send_info(struct process *p, int sig, const struct sig_src *src)
{
    if (sig <= 0 || sig >= NSIG)
        return -EINVAL;
    if (p->state == PROC_ZOMBIE)
        return 0;
    uint64_t flags = spin_lock_irqsave(&p->tlock);
    if (ignored(p, sig) && !blocked_by_all(p, sig)) {
        spin_unlock_irqrestore(&p->tlock, flags);
        return 0;
    }
    if (!(p->sig_pending & SIGMASK(sig)))
        p->sig_src[sig - 1] = *src;
    __atomic_fetch_or(&p->sig_pending, SIGMASK(sig), __ATOMIC_SEQ_CST);
    wake_receiver_locked(p, sig);
    spin_unlock_irqrestore(&p->tlock, flags);
    return 0;
}

int signal_send(struct process *p, int sig)
{
    return signal_send_info(p, sig, &kernel_src);
}

int signal_send_thread_info(struct thread *t, int sig, const struct sig_src *src, bool tlock_held)
{
    struct process *p = t->process;
    if (sig <= 0 || sig >= NSIG)
        return -EINVAL;
    if (!p || p->state == PROC_ZOMBIE)
        return 0;
    uint64_t flags = 0;
    if (!tlock_held)
        flags = spin_lock_irqsave(&p->tlock);
    bool blocked = (t->sig_blocked & SIGMASK(sig)) != 0;
    bool kept = ((t->sig_blocked | t->sig_waiting) & SIGMASK(sig)) != 0;
    if (!ignored(p, sig) || kept) {
        if (!(t->sig_pending & SIGMASK(sig)))
            t->sig_src[sig - 1] = *src;
        __atomic_fetch_or(&t->sig_pending, SIGMASK(sig), __ATOMIC_SEQ_CST);
        if (!blocked)
            wake_thread(t);
    }
    if (!tlock_held)
        spin_unlock_irqrestore(&p->tlock, flags);
    return 0;
}

int signal_send_thread(struct thread *t, int sig)
{
    return signal_send_thread_info(t, sig, &kernel_src, false);
}

bool signal_pending_current(void)
{
    struct thread *t = thread_current();
    struct process *p = t ? t->process : NULL;
    if (!p)
        return false;
    if (t->kill_me)
        return true;
    return ((p->sig_pending | t->sig_pending) & ~t->sig_blocked) != 0;
}

static int dequeue(struct process *p, struct thread *t, uint64_t allowed, struct sig_src *src)
{
    uint64_t flags = spin_lock_irqsave(&p->tlock);
    uint64_t ready = (t->sig_pending | p->sig_pending) & allowed;
    int sig = 0;
    if (ready) {
        sig = (ready & SIGMASK(SIGKILL)) ? SIGKILL : __builtin_ctzll(ready) + 1;
        if (t->sig_pending & SIGMASK(sig)) {
            *src = t->sig_src[sig - 1];
            __atomic_fetch_and(&t->sig_pending, ~SIGMASK(sig), __ATOMIC_SEQ_CST);
        } else {
            *src = p->sig_src[sig - 1];
            __atomic_fetch_and(&p->sig_pending, ~SIGMASK(sig), __ATOMIC_SEQ_CST);
        }
    }
    spin_unlock_irqrestore(&p->tlock, flags);
    return sig;
}

static struct user_sigaction get_action(struct process *p, int sig)
{
    uint64_t flags = spin_lock_irqsave(&p->tlock);
    struct user_sigaction sa = *SIGACT(p, sig);
    spin_unlock_irqrestore(&p->tlock, flags);
    return sa;
}

static bool user_code_addr(uint64_t a)
{
    return a < USER_ADDR_MAX;
}

static void fill_info(struct user_siginfo *info, int sig, const struct sig_src *src)
{
    memset(info, 0, sizeof(*info));
    info->si_signo = sig;
    info->si_code = src->code;
    bool fault = sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE;
    if (sig == SIGCHLD && (src->code == CLD_EXITED || src->code == CLD_KILLED)) {
        info->word[0] = src->pid;
        info->word[1] = (uint32_t)src->status;
    } else if (fault && src->code > 0) {
        info->word[0] = src->addr;
    } else {
        info->word[0] = src->pid;
    }
}

static bool push_sigframe(struct interrupt_frame *f, int sig, const struct user_sigaction *sa,
                          const struct sig_src *src)
{
    if (!sa->sa_restorer || !user_code_addr(sa->sa_restorer) || !user_code_addr(sa->sa_handler))
        return false;

    uint64_t fp = ALIGN_DOWN(f->rsp - RED_ZONE - FPU_STATE_SIZE, 64);
    uint64_t sp = ALIGN_DOWN(fp - sizeof(struct sigframe), 16) - 8;
    if (fp > f->rsp || sp > fp)
        return false;

    struct thread *self = thread_current();
    uint64_t oldmask = self->restore_mask ? self->saved_mask : self->sig_blocked;
    struct sigframe sf;
    memset(&sf, 0, sizeof(sf));
    sf.pretcode = sa->sa_restorer;
    struct linux_sigcontext *mc = &sf.uc.mc;
    mc->r8 = f->r8;
    mc->r9 = f->r9;
    mc->r10 = f->r10;
    mc->r11 = f->r11;
    mc->r12 = f->r12;
    mc->r13 = f->r13;
    mc->r14 = f->r14;
    mc->r15 = f->r15;
    mc->rdi = f->rdi;
    mc->rsi = f->rsi;
    mc->rbp = f->rbp;
    mc->rbx = f->rbx;
    mc->rdx = f->rdx;
    mc->rax = f->rax;
    mc->rcx = f->rcx;
    mc->rsp = f->rsp;
    mc->rip = f->rip;
    mc->eflags = f->rflags;
    mc->cs = (uint16_t)f->cs;
    mc->ss = (uint16_t)f->ss;
    mc->trapno = f->int_no;
    mc->err = f->int_no == SYSCALL_VECTOR ? 0 : f->err_code;
    mc->oldmask = oldmask;
    mc->cr2 = sig == SIGSEGV ? src->addr : 0;
    mc->fpstate = fp;
    sf.uc.sigmask = oldmask;
    fill_info(&sf.info, sig, src);
    uint8_t fpu[FPU_STATE_SIZE] ALIGNED(16);
    fpu_save(fpu);
    if (copy_to_user((void *)fp, fpu, sizeof(fpu)) < 0 || copy_to_user((void *)sp, &sf, sizeof(sf)) < 0)
        return false;

    fpu_reset_current();
    self->restore_mask = false;
    self->restart_kind = RESTART_NONE;
    uint64_t mask = self->sig_blocked | sa->sa_mask;
    if (!(sa->sa_flags & SA_NODEFER))
        mask |= SIGMASK(sig);
    set_mask(self, mask);
    f->rsp = sp;
    f->rip = sa->sa_handler;
    f->rdi = (uint64_t)sig;
    f->rsi = sp + __builtin_offsetof(struct sigframe, info);
    f->rdx = sp + __builtin_offsetof(struct sigframe, uc);
    f->rax = 0;
    f->rflags &= ~(RFLAGS_DF | RFLAGS_TF | RFLAGS_RF);
    return true;
}

static bool restartable(const struct interrupt_frame *f)
{
    return f->int_no == SYSCALL_VECTOR && f->err_code != NO_RESTART;
}

static bool restart_code(int64_t r)
{
    return r == -ERESTARTSYS || r == -ERESTARTNOINTR || r == -ERESTARTNOHAND || r == -ERESTART_RESTARTBLOCK;
}

static void restart_syscall(struct interrupt_frame *f)
{
    f->rax = f->err_code;
    f->rip -= 2;
}

static void restart_for_handler(struct interrupt_frame *f, const struct user_sigaction *sa)
{
    if (!restartable(f))
        return;
    int64_t r = (int64_t)f->rax;
    if (r == -ERESTARTNOINTR || (r == -ERESTARTSYS && (sa->sa_flags & SA_RESTART)))
        restart_syscall(f);
    else if (restart_code(r))
        f->rax = (uint64_t)-EINTR;
    if (r == -ERESTART_RESTARTBLOCK)
        thread_current()->restart_kind = RESTART_NONE;
}

static void restart_without_handler(struct interrupt_frame *f)
{
    if (!restartable(f))
        return;
    int64_t r = (int64_t)f->rax;
    if (r == -ERESTART_RESTARTBLOCK) {
        f->rax = SYS_RESTART_SYSCALL;
        f->rip -= 2;
    } else if (restart_code(r)) {
        restart_syscall(f);
    }
}

static bool deliver(struct process *p, struct thread *self, struct interrupt_frame *f)
{
    for (;;) {
        if (self->kill_me || p->group_exit)
            process_thread_leave();
        struct sig_src src;
        int sig = dequeue(p, self, ~self->sig_blocked, &src);
        if (!sig)
            return false;

        struct user_sigaction sa = get_action(p, sig);
        uint64_t action = sig == SIGKILL ? SIG_DFL : sa.sa_handler;
        if (action == SIG_IGN)
            continue;
        if (action == SIG_DFL) {
            if (default_ignores(sig))
                continue;
            log_error_limited("proc", "pid %u '%s' morto por sinal %d (%s)", p->pid, p->name, sig,
                      signal_name(sig));
            process_exit_signal(sig);
        }
        restart_for_handler(f, &sa);
        if (!push_sigframe(f, sig, &sa, &src)) {
            log_error_limited("proc", "pid %u '%s': pilha invalida para o sinal %d", p->pid, p->name, sig);
            process_exit_signal(SIGSEGV);
        }
        if (sa.sa_flags & SA_RESETHAND) {
            uint64_t flags = spin_lock_irqsave(&p->tlock);
            struct user_sigaction *cur = SIGACT(p, sig);
            if (cur->sa_handler == sa.sa_handler) {
                cur->sa_handler = SIG_DFL;
                cur->sa_flags &= ~(SA_SIGINFO | SA_RESETHAND);
            }
            spin_unlock_irqrestore(&p->tlock, flags);
        }
        return true;
    }
}

static bool has_work(struct process *p, struct thread *self)
{
    return self->kill_me || p->group_exit || ((p->sig_pending | self->sig_pending) & ~self->sig_blocked);
}

void signal_check(struct interrupt_frame *f)
{
    if ((f->cs & 3) != 3)
        return;
    struct process *p = process_current();
    if (!p)
        return;
    struct thread *self = thread_current();
    bool was_on = interrupts_enabled();
    if (has_work(p, self) || self->restore_mask) {
        if (!was_on)
            sti();
        bool delivered = false;
        for (int guard = 0; guard <= NSIG; guard++) {
            if (has_work(p, self) && deliver(p, self, f)) {
                delivered = true;
                continue;
            }
            if (delivered || !self->restore_mask)
                break;
            self->restore_mask = false;
            set_mask(self, self->saved_mask);
        }
        if (!delivered)
            restart_without_handler(f);
        if (!was_on)
            cli();
    } else {
        restart_without_handler(f);
    }
    if (!user_code_addr(f->rip) || !user_code_addr(f->rsp)) {
        log_error_limited("proc", "pid %u '%s': volta ao usuario com RIP=%016lx RSP=%016lx invalidos", p->pid, p->name,
                  f->rip, f->rsp);
        sti();
        process_exit_signal(SIGSEGV);
    }
}

uint64_t signal_pending_set(void)
{
    struct thread *t = thread_current();
    struct process *p = t->process;
    uint64_t pending = t->sig_pending | (p ? p->sig_pending : 0);
    return pending & t->sig_blocked;
}

static void sleep_for_signal(uint64_t ms)
{
    spinlock_t lock = SPINLOCK_INIT;
    struct waitqueue wq = WAITQUEUE_INIT;
    uint64_t flags = spin_lock_irqsave(&lock);
    wait_sleep_intr(&wq, &lock, ms);
    spin_unlock_irqrestore(&lock, flags);
}

long signal_suspend(uint64_t mask)
{
    struct thread *self = thread_current();
    self->saved_mask = self->sig_blocked;
    self->restore_mask = true;
    set_mask(self, mask);
    while (!signal_pending_current())
        sleep_for_signal(0);
    return -ERESTARTNOHAND;
}

long signal_timedwait(uint64_t set, struct user_siginfo *info, bool has_timeout, uint64_t ms)
{
    struct thread *self = thread_current();
    struct process *p = self->process;
    uint64_t want = set & ~SIG_UNBLOCKABLE;
    uint64_t old = self->sig_blocked;
    __atomic_store_n(&self->sig_waiting, want, __ATOMIC_SEQ_CST);
    set_mask(self, old & ~want);
    uint64_t deadline = has_timeout ? time_uptime_ms() + ms + (ms ? 1 : 0) : 0;
    long ret;
    for (;;) {
        struct sig_src src;
        int sig = dequeue(p, self, want, &src);
        if (sig) {
            fill_info(info, sig, &src);
            ret = sig;
            break;
        }
        if (self->kill_me || p->group_exit ||
            ((p->sig_pending | self->sig_pending) & ~self->sig_blocked & ~want)) {
            ret = -EINTR;
            break;
        }
        uint64_t left = 0;
        if (has_timeout) {
            uint64_t now = time_uptime_ms();
            if (now >= deadline) {
                ret = -EAGAIN;
                break;
            }
            left = deadline - now;
            if (left > 86400000)
                left = 86400000;
        }
        sleep_for_signal(left);
    }
    set_mask(self, old);
    __atomic_store_n(&self->sig_waiting, 0, __ATOMIC_SEQ_CST);
    return ret;
}

long signal_sigaction(int sig, const struct user_sigaction *act, struct user_sigaction *old)
{
    if (sig <= 0 || sig >= NSIG || sig == SIGKILL || sig == SIGSTOP)
        return -EINVAL;
    struct process *p = process_current();
    uint64_t flags = spin_lock_irqsave(&p->tlock);
    if (old)
        *old = *SIGACT(p, sig);
    if (act) {
        *SIGACT(p, sig) = *act;
        if (act->sa_handler == SIG_IGN || (act->sa_handler == SIG_DFL && default_ignores(sig))) {
            __atomic_fetch_and(&p->sig_pending, ~SIGMASK(sig), __ATOMIC_SEQ_CST);
            for (struct thread *t = p->threads; t; t = t->proc_next)
                __atomic_fetch_and(&t->sig_pending, ~SIGMASK(sig), __ATOMIC_SEQ_CST);
        }
    }
    spin_unlock_irqrestore(&p->tlock, flags);
    return 0;
}

long signal_procmask(int how, const uint64_t *set, uint64_t *old)
{
    struct thread *self = thread_current();
    if (old)
        *old = self->sig_blocked;
    if (!set)
        return 0;
    uint64_t v = *set & ~SIG_UNBLOCKABLE;
    switch (how) {
    case SIG_BLOCK:   set_mask(self, self->sig_blocked | v); break;
    case SIG_UNBLOCK: set_mask(self, self->sig_blocked & ~v); break;
    case SIG_SETMASK: set_mask(self, v); break;
    default: return -EINVAL;
    }
    return 0;
}

long signal_sigreturn(struct interrupt_frame *f)
{
    struct process *p = process_current();
    uint64_t sp = f->rsp - 8;
    struct sigframe sf;
    uint8_t fpu[FPU_STATE_SIZE] ALIGNED(16);
    const struct linux_sigcontext *mc = &sf.uc.mc;
    if (copy_from_user(&sf, (const void *)sp, sizeof(sf)) < 0 || !user_code_addr(mc->rip) ||
        !user_code_addr(mc->rsp) || (mc->fpstate & 15) ||
        copy_from_user(fpu, (const void *)mc->fpstate, sizeof(fpu)) < 0) {
        log_error_limited("proc", "pid %u '%s': quadro de sinal invalido em %016lx", p->pid, p->name, sp);
        process_exit_signal(SIGSEGV);
    }

    struct interrupt_frame r;
    memset(&r, 0, sizeof(r));
    r.r8 = mc->r8;
    r.r9 = mc->r9;
    r.r10 = mc->r10;
    r.r11 = mc->r11;
    r.r12 = mc->r12;
    r.r13 = mc->r13;
    r.r14 = mc->r14;
    r.r15 = mc->r15;
    r.rdi = mc->rdi;
    r.rsi = mc->rsi;
    r.rbp = mc->rbp;
    r.rbx = mc->rbx;
    r.rdx = mc->rdx;
    r.rax = mc->rax;
    r.rcx = mc->rcx;
    r.rsp = mc->rsp;
    r.rip = mc->rip;
    r.rflags = (mc->eflags & USER_RFLAGS_MASK) | RFLAGS_FIXED;
    r.cs = f->cs;
    r.ss = f->ss;
    r.int_no = f->int_no;
    r.err_code = NO_RESTART;

    struct thread *self = thread_current();
    self->restart_kind = RESTART_NONE;
    set_mask(self, sf.uc.sigmask);
    fpu_sanitize(fpu);
    fpu_restore(fpu);
    *f = r;
    return (long)f->rax;
}
