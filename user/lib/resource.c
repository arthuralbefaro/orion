#include <sys/resource.h>
#include <syscall.h>

int getrlimit(int resource, struct rlimit *r)
{
    return (int)__syscall_ret(syscall2(SYS_GETRLIMIT, resource, (long)r));
}

int setrlimit(int resource, const struct rlimit *r)
{
    return (int)__syscall_ret(syscall2(SYS_SETRLIMIT, resource, (long)r));
}
