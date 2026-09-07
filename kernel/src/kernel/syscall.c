#include <kernel/syscall.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/log.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/gdt.h>
#include <arch/x86_64/interrupts.h>
#include <lib/string.h>

extern void syscall_entry(void);

#define EFER_SCE (1ull << 0)

static bool user_range_ok(uint64_t ptr, uint64_t len)
{
    return ptr < USER_ADDR_MAX && len <= USER_ADDR_MAX - ptr;
}

static long sys_write(int fd, const char *buf, size_t len)
{
    if (fd != 1 && fd != 2)
        return -EBADF;
    if (!user_range_ok((uint64_t)buf, len))
        return -EFAULT;
    for (size_t i = 0; i < len; i++)
        kputc(buf[i]);
    return (long)len;
}

struct timespec_user {
    int64_t tv_sec;
    int64_t tv_nsec;
};

static long sys_nanosleep(const struct timespec_user *req)
{
    if (!user_range_ok((uint64_t)req, sizeof(*req)))
        return -EFAULT;
    uint64_t ms = (uint64_t)req->tv_sec * 1000 + (uint64_t)req->tv_nsec / 1000000;
    thread_sleep_ms(ms ? ms : 1);
    return 0;
}

void syscall_dispatch(struct interrupt_frame *f)
{
    sti();
    long ret;
    switch (f->rax) {
        case SYS_WRITE:
            ret = sys_write((int)f->rdi, (const char *)f->rsi, (size_t)f->rdx);
            break;
        case SYS_SCHED_YIELD:
            thread_yield();
            ret = 0;
            break;
        case SYS_NANOSLEEP:
            ret = sys_nanosleep((const struct timespec_user *)f->rdi);
            break;
        case SYS_GETPID:
            ret = process_current()->pid;
            break;
        case SYS_EXIT:
            process_exit((int)f->rdi);
        default:
            log_warn("syscall", "pid %u: syscall %lu desconhecida", process_current()->pid, f->rax);
            ret = -ENOSYS;
            break;
    }
    f->rax = (uint64_t)ret;
}

void syscall_init_cpu(void)
{
    uint64_t star = ((uint64_t)GDT_KERNEL_CODE << 32) | ((uint64_t)GDT_KERNEL_DATA << 48);
    wrmsr(MSR_IA32_STAR, star);
    wrmsr(MSR_IA32_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_IA32_FMASK, RFLAGS_IF | (1 << 10) | (1 << 8));
    wrmsr(MSR_IA32_EFER, rdmsr(MSR_IA32_EFER) | EFER_SCE);
}

void syscall_init(void)
{
    syscall_init_cpu();
    log_info("syscall", "syscall/sysret ativos, entrada em %p", (void *)syscall_entry);
}

