#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

static void show(unsigned id, int user)
{
    const char *n = NULL;
    if (user) {
        struct passwd *pw = getpwuid(id);
        n = pw ? pw->pw_name : NULL;
    } else {
        struct group *gr = getgrgid(id);
        n = gr ? gr->gr_name : NULL;
    }
    if (n)
        printf("%u(%s)", id, n);
    else
        printf("%u", id);
}

int main(void)
{
    uid_t uid = getuid(), euid = geteuid();
    gid_t gid = getgid(), egid = getegid();
    printf("uid=");
    show(uid, 1);
    printf(" gid=");
    show(gid, 0);
    if (euid != uid) {
        printf(" euid=");
        show(euid, 1);
    }
    if (egid != gid) {
        printf(" egid=");
        show(egid, 0);
    }
    gid_t groups[32];
    int n = getgroups(32, groups);
    if (n > 0) {
        printf(" grupos=");
        for (int i = 0; i < n; i++) {
            if (i)
                putchar(',');
            show(groups[i], 0);
        }
    }
    putchar('\n');
    return 0;
}
