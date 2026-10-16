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

#define SIGFRAME_MAGIC 0x4F52494F5349475Aull
#define RED_ZONE       128

struct sigframe {
    uint64_t restorer;
    uint64_t magic;
    uint64_t mask;
    struct interrupt_frame ctx;
};

static const char *const names[NSIG] = {
    [SIGHUP] = "SIGHUP",   [SIGINT] = "SIGINT",   [SIGQUIT] = "SIGQUIT",
    [SIGILL] = "SIGILL",   [SIGABRT] = "SIGABRT", [SIGFPE] = "SIGFPE",
    [SIGKILL] = "SIGKILL", [SIGSEGV] = "SIGSEGV", [SIGPIPE] = "SIGPIPE",
    [SIGALRM] = "SIGALRM", [SIGTERM] = "SIGTERM", [SIGCHLD] = "SIGCHLD",
};

const char *signal_name(int sig)
{
    if (sig > 0 && sig < NSIG && names[sig])
        return names[sig];
    return "?";
}

static bool default_ignores(int sig)
{
    return sig == SIGCHLD;
}

void signal_init_process(struct process *p)
{
    p->sig_pending = 0;
    p->sig_blocked = 0;
    p->alarm_at = 0;
    memset(p->sig_handlers, 0, sizeof(p->sig_handlers));
}

void signal_reset_on_exec(struct process *p)
{
    for (int i = 0; i < NSIG; i++) {
        if (p->sig_handlers[i].sa_handler != SIG_IGN)
            p->sig_handlers[i].sa_handler = SIG_DFL;
        p->sig_handlers[i].sa_flags = 0;
        p->sig_handlers[i].sa_mask = 0;
    }
}

int signal_send(struct process *p, int sig)
{
    if (sig <= 0 || sig >= NSIG)
        return -EINVAL;
    if (p->state == PROC_ZOMBIE)
        return 0;

    uint64_t action = p->sig_handlers[sig].sa_handler;
    if (sig != SIGKILL) {
        if (action == SIG_IGN)
            return 0;
        if (action == SIG_DFL && default_ignores(sig))
            return 0;
    }

    uint64_t flags = irq_save();
    __atomic_fetch_or(&p->sig_pending, SIGMASK(sig), __ATOMIC_SEQ_CST);
    struct thread *t = p->thread;
    if (t) {
        sched_runq_lock();
        wait_unlink(t);
        sched_make_ready(t);
        sched_runq_unlock();
    }
    irq_restore(flags);
    return 0;
}

bool signal_pending_current(void)
{
    struct process *p = process_current();
    if (!p)
        return false;
    return (p->sig_pending & ~p->sig_blocked) != 0;
}

static int next_pending(struct process *p)
{
    uint64_t ready = p->sig_pending & ~p->sig_blocked;
    if (ready & SIGMASK(SIGKILL))
        return SIGKILL;
    for (int i = 1; i < NSIG; i++)
        if (ready & SIGMASK(i))
            return i;
    return 0;
}

static bool user_stack_ok(struct process *p, uint64_t base, uint64_t size)
{
    if (base >= USER_ADDR_MAX || size > USER_ADDR_MAX - base)
        return false;
    for (uint64_t v = PAGE_ALIGN_DOWN(base); v < base + size; v += PAGE_SIZE)
        if (!vmm_virt_to_phys_in(p->pml4_phys, v))
            return false;
    return true;
}

static bool push_sigframe(struct process *p, struct interrupt_frame *f, int sig)
{
    struct user_sigaction *sa = &p->sig_handlers[sig];
    if (!sa->sa_restorer)
        return false;

    uint64_t sp = ALIGN_DOWN(f->rsp - RED_ZONE, 16);
    sp -= sizeof(struct sigframe);
    sp = ALIGN_DOWN(sp, 16) - 8;
    if (!user_stack_ok(p, sp, sizeof(struct sigframe)))
        return false;

    struct sigframe *sf = (struct sigframe *)sp;
    sf->restorer = sa->sa_restorer;
    sf->magic = SIGFRAME_MAGIC;
    sf->mask = p->sig_blocked;
    sf->ctx = *f;

    p->sig_blocked |= sa->sa_mask | SIGMASK(sig);
    f->rsp = sp;
    f->rip = sa->sa_handler;
    f->rdi = (uint64_t)sig;
    return true;
}

void signal_check(struct interrupt_frame *f)
{
    if ((f->cs & 3) != 3)
        return;
    struct process *p = process_current();
    if (!p)
        return;

    for (;;) {
        int sig = next_pending(p);
        if (!sig)
            return;
        __atomic_fetch_and(&p->sig_pending, ~SIGMASK(sig), __ATOMIC_SEQ_CST);

        uint64_t action = p->sig_handlers[sig].sa_handler;
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

long signal_sigaction(int sig, const struct user_sigaction *act, struct user_sigaction *old)
{
    if (sig <= 0 || sig >= NSIG || sig == SIGKILL)
        return -EINVAL;
    struct process *p = process_current();
    if (old)
        *old = p->sig_handlers[sig];
    if (act) {
        p->sig_handlers[sig] = *act;
        if (act->sa_handler == SIG_IGN || (act->sa_handler == SIG_DFL && default_ignores(sig)))
            __atomic_fetch_and(&p->sig_pending, ~SIGMASK(sig), __ATOMIC_SEQ_CST);
    }
    return 0;
}

long signal_procmask(int how, const uint64_t *set, uint64_t *old)
{
    struct process *p = process_current();
    if (old)
        *old = p->sig_blocked;
    if (!set)
        return 0;
    uint64_t v = *set & ~(SIGMASK(SIGKILL));
    switch (how) {
    case SIG_BLOCK:   p->sig_blocked |= v; break;
    case SIG_UNBLOCK: p->sig_blocked &= ~v; break;
    case SIG_SETMASK: p->sig_blocked = v; break;
    default: return -EINVAL;
    }
    return 0;
}

long signal_sigreturn(struct interrupt_frame *f)
{
    struct process *p = process_current();
    uint64_t sp = f->rsp - 8;
    if (!user_stack_ok(p, sp, sizeof(struct sigframe)))
        return -EFAULT;

    struct sigframe *sf = (struct sigframe *)sp;
    if (sf->magic != SIGFRAME_MAGIC)
        return -EFAULT;

    struct interrupt_frame saved = sf->ctx;
    p->sig_blocked = sf->mask;

    saved.cs = f->cs;
    saved.ss = f->ss;
    saved.int_no = f->int_no;
    saved.err_code = f->err_code;
    *f = saved;
    return (long)f->rax;
}
