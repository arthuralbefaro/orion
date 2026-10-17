#include <kernel/signal.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/syscall.h>
#include <kernel/wait.h>
#include <kernel/log.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/interrupts.h>
#include <lib/string.h>
#include <mm/memory.h>
#include <mm/vmm.h>
#include <mm/uaccess.h>
#include <arch/x86_64/fpu.h>

#define SIGFRAME_MAGIC 0x4F52494F5349475Aull
#define RED_ZONE       128

struct sigframe {
    uint64_t restorer;
    uint64_t magic;
    uint64_t mask;
    struct interrupt_frame ctx;
    uint8_t fpu[FPU_STATE_SIZE] ALIGNED(16);
};

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
    for (int i = 0; i < NSIG - 1; i++) {
        if (p->sig_handlers[i].sa_handler != SIG_IGN)
            p->sig_handlers[i].sa_handler = SIG_DFL;
        p->sig_handlers[i].sa_flags = 0;
        p->sig_handlers[i].sa_mask = 0;
    }
}

static bool ignored(struct process *p, int sig)
{
    if (sig == SIGKILL)
        return false;
    uint64_t action = SIGACT(p, sig)->sa_handler;
    return action == SIG_IGN || (action == SIG_DFL && default_ignores(sig));
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

static void wake_receiver(struct process *p, int sig)
{
    uint64_t flags = spin_lock_irqsave(&p->tlock);
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
    spin_unlock_irqrestore(&p->tlock, flags);
}

void signal_retarget(struct process *p, uint64_t sigs)
{
    while (sigs) {
        wake_receiver(p, __builtin_ctzll(sigs) + 1);
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

int signal_send(struct process *p, int sig)
{
    if (sig <= 0 || sig >= NSIG)
        return -EINVAL;
    if (p->state == PROC_ZOMBIE || ignored(p, sig))
        return 0;

    __atomic_fetch_or(&p->sig_pending, SIGMASK(sig), __ATOMIC_SEQ_CST);
    wake_receiver(p, sig);
    return 0;
}

int signal_send_thread(struct thread *t, int sig)
{
    struct process *p = t->process;
    if (sig <= 0 || sig >= NSIG)
        return -EINVAL;
    if (!p || p->state == PROC_ZOMBIE || ignored(p, sig))
        return 0;
    __atomic_fetch_or(&t->sig_pending, SIGMASK(sig), __ATOMIC_SEQ_CST);
    wake_thread(t);
    return 0;
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

static int next_pending(struct process *p, struct thread *t)
{
    for (;;) {
        uint64_t ready = (t->sig_pending | p->sig_pending) & ~t->sig_blocked;
        if (!ready)
            return 0;
        int sig = (ready & SIGMASK(SIGKILL)) ? SIGKILL : __builtin_ctzll(ready) + 1;
        uint64_t *set = (t->sig_pending & SIGMASK(sig)) ? &t->sig_pending : &p->sig_pending;
        if (__atomic_fetch_and(set, ~SIGMASK(sig), __ATOMIC_SEQ_CST) & SIGMASK(sig))
            return sig;
    }
}

#define USER_RFLAGS_MASK 0xCD5ull
#define RFLAGS_FIXED     0x202ull

static bool user_code_addr(uint64_t a)
{
    return a < USER_ADDR_MAX;
}

static bool push_sigframe(struct process *p, struct interrupt_frame *f, int sig)
{
    struct user_sigaction *sa = SIGACT(p, sig);
    if (!sa->sa_restorer || !user_code_addr(sa->sa_restorer) || !user_code_addr(sa->sa_handler))
        return false;

    uint64_t sp = ALIGN_DOWN(f->rsp - RED_ZONE, 16);
    sp -= sizeof(struct sigframe);
    sp = ALIGN_DOWN(sp, 16) - 8;

    struct sigframe sf;
    sf.restorer = sa->sa_restorer;
    sf.magic = SIGFRAME_MAGIC;
    struct thread *self = thread_current();
    sf.mask = self->sig_blocked;
    sf.ctx = *f;
    fpu_save(sf.fpu);
    if (copy_to_user((void *)sp, &sf, sizeof(sf)) < 0)
        return false;

    set_mask(self, self->sig_blocked | sa->sa_mask | SIGMASK(sig));
    f->rsp = sp;
    f->rip = sa->sa_handler;
    f->rdi = (uint64_t)sig;
    return true;
}

static void deliver(struct process *p, struct thread *self, struct interrupt_frame *f)
{
    for (;;) {
        if (self->kill_me || p->group_exit)
            process_thread_leave();
        int sig = next_pending(p, self);
        if (!sig)
            return;

        uint64_t action = SIGACT(p, sig)->sa_handler;
        if (sig == SIGKILL)
            action = SIG_DFL;
        if (action == SIG_IGN)
            continue;
        if (action == SIG_DFL) {
            if (default_ignores(sig))
                continue;
            log_error("proc", "pid %u '%s' morto por sinal %d (%s)", p->pid, p->name, sig,
                      signal_name(sig));
            process_exit_signal(sig);
        }
        if (!push_sigframe(p, f, sig)) {
            log_error("proc", "pid %u '%s': pilha invalida para o sinal %d", p->pid, p->name, sig);
            process_exit_signal(SIGSEGV);
        }
        return;
    }
}

void signal_check(struct interrupt_frame *f)
{
    if ((f->cs & 3) != 3)
        return;
    struct process *p = process_current();
    if (!p)
        return;
    struct thread *self = thread_current();
    if (!self->kill_me && !p->group_exit && !((p->sig_pending | self->sig_pending) & ~self->sig_blocked))
        return;
    bool was_on = interrupts_enabled();
    if (!was_on)
        sti();
    deliver(p, self, f);
    if (!was_on)
        cli();
}

long signal_sigaction(int sig, const struct user_sigaction *act, struct user_sigaction *old)
{
    if (sig <= 0 || sig >= NSIG || sig == SIGKILL || sig == SIGSTOP)
        return -EINVAL;
    struct process *p = process_current();
    if (old)
        *old = *SIGACT(p, sig);
    if (act) {
        *SIGACT(p, sig) = *act;
        if (act->sa_handler == SIG_IGN || (act->sa_handler == SIG_DFL && default_ignores(sig)))
            __atomic_fetch_and(&p->sig_pending, ~SIGMASK(sig), __ATOMIC_SEQ_CST);
    }
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
    if (copy_from_user(&sf, (const void *)sp, sizeof(sf)) < 0 || sf.magic != SIGFRAME_MAGIC ||
        !user_code_addr(sf.ctx.rip) || !user_code_addr(sf.ctx.rsp)) {
        log_error("proc", "pid %u '%s': quadro de sinal invalido em %016lx", p->pid, p->name, sp);
        process_exit_signal(SIGSEGV);
    }

    struct interrupt_frame saved = sf.ctx;
    set_mask(thread_current(), sf.mask);
    fpu_sanitize(sf.fpu);
    fpu_restore(sf.fpu);

    saved.cs = f->cs;
    saved.ss = f->ss;
    saved.int_no = f->int_no;
    saved.err_code = f->err_code;
    saved.rflags = (saved.rflags & USER_RFLAGS_MASK) | RFLAGS_FIXED;
    *f = saved;
    return (long)f->rax;
}
