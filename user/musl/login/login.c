#include "auth.h"

#include <signal.h>

#define TRIES 3

int login_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (geteuid() != 0) {
        fprintf(stderr, "login: precisa rodar como root\n");
        return 1;
    }
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    char host[64] = "orion";
    gethostname(host, sizeof(host));
    char prompt[96];
    snprintf(prompt, sizeof(prompt), "%s login: ", host);
    int failures = 0;
    while (failures < TRIES) {
        char user[64], password[128];
        if (auth_read_line(prompt, user, sizeof(user), 0) < 0)
            return 1;
        if (!user[0])
            continue;
        if (auth_read_line("Senha: ", password, sizeof(password), 1) < 0)
            return 1;
        struct passwd *pw = getpwnam(user);
        int ok = pw && auth_check(user, password);
        memset(password, 0, sizeof(password));
        if (!ok) {
            sleep(1);
            printf("Login incorreto\n\n");
            failures++;
            continue;
        }
        if (auth_become(pw) < 0) {
            fprintf(stderr, "login: nao trocou de usuario: %s\n", strerror(errno));
            return 1;
        }
        printf("Bem-vindo, %s.\n", pw->pw_name);
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        auth_exec_shell(pw, NULL, 1);
    }
    printf("login: tentativas demais\n");
    return 1;
}
