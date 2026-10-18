#include "auth.h"

int su_main(int argc, char **argv)
{
    const char *command = NULL;
    const char *target = "root";
    int login = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            command = argv[++i];
        } else if (strcmp(argv[i], "-") == 0 || strcmp(argv[i], "-l") == 0) {
            login = 1;
        } else if (argv[i][0] != '-') {
            target = argv[i];
        } else {
            fprintf(stderr, "uso: su [-] [-c comando] [usuario]\n");
            return 2;
        }
    }
    struct passwd *found = getpwnam(target);
    if (!found) {
        fprintf(stderr, "su: usuario %s nao existe\n", target);
        return 1;
    }
    struct passwd pw = *found;
    char name[64], dir[128], shell[128];
    snprintf(name, sizeof(name), "%s", found->pw_name);
    snprintf(dir, sizeof(dir), "%s", found->pw_dir);
    snprintf(shell, sizeof(shell), "%s", found->pw_shell ? found->pw_shell : "");
    pw.pw_name = name;
    pw.pw_dir = dir;
    pw.pw_shell = shell;
    if (getuid() != 0) {
        char password[128];
        if (auth_read_line("Senha: ", password, sizeof(password), 1) < 0)
            return 1;
        int ok = auth_check(name, password);
        memset(password, 0, sizeof(password));
        if (!ok) {
            sleep(1);
            fprintf(stderr, "su: senha incorreta\n");
            return 1;
        }
    }
    if (auth_become(&pw) < 0) {
        fprintf(stderr, "su: nao trocou de usuario: %s\n", strerror(errno));
        return 1;
    }
    auth_exec_shell(&pw, command, login);
    return 1;
}
