#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/reboot.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const char *name = argc > 0 ? argv[0] : "poweroff";
    const char *slash = strrchr(name, '/');
    if (slash)
        name = slash + 1;
    int cmd = RB_POWER_OFF;
    const char *what = "desligando";
    if (strcmp(name, "reboot") == 0) {
        cmd = RB_AUTOBOOT;
        what = "reiniciando";
    } else if (strcmp(name, "halt") == 0) {
        cmd = RB_HALT_SYSTEM;
        what = "parando";
    }
    if (argc > 1) {
        fprintf(stderr, "uso: %s\n", name);
        return 2;
    }
    printf("%s: gravando os discos e %s\n", name, what);
    fflush(stdout);
    sync();
    reboot(cmd);
    fprintf(stderr, "%s: %s\n", name, strerror(errno));
    return 1;
}
