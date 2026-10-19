#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "uso: wmctl term | procs | files <caminho> | list | ping | fundo <imagem|nenhum>\n");
        return 2;
    }
    char line[256] = "";
    for (int i = 1; i < argc; i++) {
        if (i > 1)
            strncat(line, " ", sizeof(line) - strlen(line) - 1);
        strncat(line, argv[i], sizeof(line) - strlen(line) - 1);
    }
    strncat(line, "\n", sizeof(line) - strlen(line) - 1);
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = { 0 };
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "/run/wm.sock");
    if (s < 0 || connect(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        fprintf(stderr, "wmctl: %s: %s\n", a.sun_path, strerror(errno));
        return 1;
    }
    if (write(s, line, strlen(line)) < 0) {
        fprintf(stderr, "wmctl: envio: %s\n", strerror(errno));
        return 1;
    }
    shutdown(s, SHUT_WR);
    char buf[1024];
    ssize_t n;
    int bad = 0;
    while ((n = read(s, buf, sizeof(buf))) > 0) {
        fwrite(buf, 1, (size_t)n, stdout);
        if (strncmp(buf, "erro", 4) == 0)
            bad = 1;
    }
    close(s);
    return bad;
}
