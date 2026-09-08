#include <errno.h>
#include <sys/sysinfo.h>

#include "syscall.h"

int sysinfo_orion(struct sysinfo_orion *out)
{
    long r = syscall1(SYS_SYSINFO, (long)out);
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return 0;
}

int proclist_orion(struct procinfo_orion *out, int max)
{
    long r = syscall2(SYS_PROCLIST, (long)out, max);
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return (int)r;
}
