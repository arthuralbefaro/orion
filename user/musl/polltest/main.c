#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <orion.h>

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int fails;

static void check(const char *what, int ok)
{
    printf("  %-38s %s\n", what, ok ? "ok" : "FALHA");
    if (!ok)
        fails++;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    int a[2], b[2];
    if (pipe(a) < 0 || pipe(b) < 0) {
        orion_perror("polltest: pipe");
        return 1;
    }

    struct pollfd fds[2];
    fds[0].fd = a[0];
    fds[0].events = POLLIN;
    fds[1].fd = b[0];
    fds[1].events = POLLIN;

    long t0 = now_ms();
    int n = poll(fds, 2, 300);
    long dt = now_ms() - t0;
    check("timeout devolve 0", n == 0);
    check("timeout durou perto de 300 ms", dt >= 250 && dt < 900);

    write(b[1], "x", 1);
    n = poll(fds, 2, 1000);
    check("poll acorda com um fd pronto", n == 1);
    check("acordou no fd certo (b)", (fds[1].revents & POLLIN) && !(fds[0].revents & POLLIN));
    char c;
    read(b[0], &c, 1);

    write(a[1], "y", 1);
    write(b[1], "z", 1);
    n = poll(fds, 2, 1000);
    check("poll ve os dois fds prontos", n == 2);
    read(a[0], &c, 1);
    read(b[0], &c, 1);

    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        close(a[0]);
        close(b[0]);
        close(b[1]);
        usleep(400000);
        write(a[1], "tarde", 5);
        _exit(0);
    }
    t0 = now_ms();
    fds[0].events = POLLIN;
    fds[1].events = POLLIN;
    n = poll(fds, 2, 3000);
    dt = now_ms() - t0;
    check("poll bloqueia ate outro processo escrever", n == 1 && (fds[0].revents & POLLIN));
    check("dormiu de verdade (>= 300 ms)", dt >= 300);
    char buf[16];
    long got = read(a[0], buf, sizeof(buf));
    check("leu o que o filho escreveu", got == 5 && memcmp(buf, "tarde", 5) == 0);
    int st = 0;
    waitpid(pid, &st, 0);

    close(a[1]);
    n = poll(fds, 1, 1000);
    check("POLLHUP quando o escritor fecha", n == 1 && (fds[0].revents & POLLHUP));

    close(a[0]);
    close(b[0]);
    close(b[1]);

    int nb[2];
    pipe(nb);
    fcntl(nb[0], F_SETFL, O_NONBLOCK);
    long r = read(nb[0], buf, sizeof(buf));
    check("O_NONBLOCK devolve EAGAIN", r < 0 && errno == EAGAIN);
    close(nb[0]);
    close(nb[1]);

    printf("polltest: %s\n", fails == 0 ? "TUDO OK" : "FALHOU");
    return fails ? 1 : 0;
}
