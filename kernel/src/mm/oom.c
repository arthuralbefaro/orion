#include <mm/oom.h>
#include <mm/pmm.h>
#include <fs/vfs.h>
#include <kernel/log.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/signal.h>
#include <kernel/spinlock.h>
#include <kernel/time.h>
#include <lib/string.h>

#define OOM_RECLAIM_PAGES 256
#define OOM_VICTIM_WAIT_MS 5000
#define OOM_SETTLE_MS 2000

static spinlock_t oom_lock = SPINLOCK_INIT;
static uint32_t victim_pid;
static uint64_t victim_since;
static uint64_t victim_left;
static uint64_t settles;
static uint64_t kills;
static uint64_t reclaims;
static uint64_t self_kills;

static bool memory_ok(void)
{
    return pmm_user_pages_available() > 0;
}

bool oom_fatal_pending(void)
{
    struct thread *t = thread_current();
    struct process *p = t->process;
    if (!p)
        return false;
    if (t->kill_me || __atomic_load_n(&p->group_exit, __ATOMIC_ACQUIRE))
        return true;
    uint64_t pending = __atomic_load_n(&p->sig_pending, __ATOMIC_ACQUIRE) |
                       __atomic_load_n(&t->sig_pending, __ATOMIC_ACQUIRE);
    return (pending & SIGMASK(SIGKILL)) != 0;
}

static bool victim_gone(uint32_t pid)
{
    struct process *v = process_get(pid);
    if (!v)
        return true;
    bool gone = v->state == PROC_ZOMBIE;
    process_put(v);
    return gone;
}

static void note_victim_left(uint32_t pid)
{
    __atomic_store_n(&victim_left, time_ticks() | 1, __ATOMIC_RELEASE);
    log_info("oom", "pid %u saiu; %lu paginas livres", pid, pmm_free_count());
}

static int settle(void)
{
    uint64_t left = __atomic_load_n(&victim_left, __ATOMIC_ACQUIRE);
    if (!left)
        return 0;
    while (time_ticks() - left < OOM_SETTLE_MS) {
        if (oom_fatal_pending())
            return -1;
        if (memory_ok()) {
            __atomic_add_fetch(&settles, 1, __ATOMIC_RELAXED);
            return 1;
        }
        thread_sleep_ms(10);
    }
    return 0;
}

bool oom_recover(void)
{
    if (oom_fatal_pending())
        return false;
    if (vfs_cache_reclaim(OOM_RECLAIM_PAGES) > 0 && memory_ok()) {
        __atomic_add_fetch(&reclaims, 1, __ATOMIC_RELAXED);
        return true;
    }
    if (memory_ok())
        return true;

    struct process *me = process_current();
    uint64_t flags;
    uint32_t pid;
    for (;;) {
        int st = settle();
        if (st != 0)
            return st > 0;
        flags = spin_lock_irqsave(&oom_lock);
        pid = victim_pid;
        if (pid && victim_gone(pid)) {
            victim_pid = 0;
            spin_unlock_irqrestore(&oom_lock, flags);
            note_victim_left(pid);
            continue;
        }
        break;
    }
    if (pid && time_ticks() - victim_since > OOM_VICTIM_WAIT_MS) {
        log_warn("oom", "pid %u ainda nao liberou a memoria em %u ms; escolhendo outra vitima", pid,
                 OOM_VICTIM_WAIT_MS);
        victim_pid = pid = 0;
    }
    bool chose = false;
    uint64_t pages = 0;
    char name[PROCESS_NAME_MAX] = "";
    if (!pid) {
        struct process *v = process_oom_pick();
        if (!v) {
            spin_unlock_irqrestore(&oom_lock, flags);
            log_error_limited("oom", "sem memoria e sem processo que possa ser morto");
            return false;
        }
        pid = victim_pid = v->pid;
        victim_since = time_ticks();
        pages = v->pages;
        strlcpy(name, v->name, sizeof(name));
        signal_send(v, SIGKILL);
        process_put(v);
        chose = true;
        __atomic_add_fetch(&kills, 1, __ATOMIC_RELAXED);
    }
    spin_unlock_irqrestore(&oom_lock, flags);

    if (chose)
        log_error("oom", "sem memoria: matando pid %u '%s' (%lu paginas); %lu livres", pid, name, pages,
                  pmm_free_count());
    if (me && pid == me->pid) {
        __atomic_add_fetch(&self_kills, 1, __ATOMIC_RELAXED);
        return false;
    }

    uint64_t start = time_ticks();
    while (time_ticks() - start < OOM_VICTIM_WAIT_MS) {
        if (oom_fatal_pending())
            return false;
        if (victim_gone(pid)) {
            flags = spin_lock_irqsave(&oom_lock);
            bool mine = victim_pid == pid;
            if (mine)
                victim_pid = 0;
            spin_unlock_irqrestore(&oom_lock, flags);
            if (mine)
                note_victim_left(pid);
            int st = settle();
            return st >= 0;
        }
        if (memory_ok())
            return true;
        thread_sleep_ms(10);
    }
    return true;
}

void oom_stats(struct oom_stats *out)
{
    out->kills = __atomic_load_n(&kills, __ATOMIC_RELAXED);
    out->self_kills = __atomic_load_n(&self_kills, __ATOMIC_RELAXED);
    out->reclaims = __atomic_load_n(&reclaims, __ATOMIC_RELAXED);
}
