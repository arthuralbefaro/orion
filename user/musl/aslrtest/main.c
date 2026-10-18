#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define SAMPLES 4
#define STACK_LOW  0x00007ffbfffff000ul
#define STACK_HIGH 0x00007ffffffff000ul
#define MMAP_LOW   0x0000100000000000ul
#define MMAP_HIGH  0x0000110000000000ul

static int failures;

static void check(const char *what, int ok)
{
    printf("  %-44s %s\n", what, ok ? "ok" : "FALHA");
    if (!ok)
        failures++;
}

static int sample(const char *self, unsigned long *stack, unsigned long *map)
{
    int fds[2];
    if (pipe(fds) < 0)
        return -1;
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        dup2(fds[1], 1);
        close(fds[0]);
        close(fds[1]);
        execl(self, self, "filho", (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    char buf[128];
    size_t len = 0;
    ssize_t n;
    while (len < sizeof(buf) - 1 && (n = read(fds[0], buf + len, sizeof(buf) - 1 - len)) > 0)
        len += (size_t)n;
    buf[len] = 0;
    close(fds[0]);
    waitpid(pid, NULL, 0);
    return sscanf(buf, "%lx %lx", stack, map) == 2 ? 0 : -1;
}

static int all_distinct(const unsigned long *v, int n)
{
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (v[i] == v[j])
                return 0;
    return 1;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "filho") == 0) {
        int local = 0;
        void *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        printf("%lx %lx\n", (unsigned long)&local, (unsigned long)m);
        return 0;
    }
    const char *self = argc > 0 && strchr(argv[0], '/') ? argv[0] : "/bin/aslrtest";
    unsigned long stacks[SAMPLES], maps[SAMPLES];
    int got = 1;
    for (int i = 0; i < SAMPLES; i++)
        if (sample(self, &stacks[i], &maps[i]) < 0)
            got = 0;
    if (!got) {
        printf("aslrtest: nao consegui rodar %s\n", self);
        return 1;
    }
    printf("aslrtest: pilha");
    for (int i = 0; i < SAMPLES; i++)
        printf(" %lx", stacks[i]);
    printf("\naslrtest: mmap ");
    for (int i = 0; i < SAMPLES; i++)
        printf(" %lx", maps[i]);
    printf("\n");
    int in_range = 1;
    for (int i = 0; i < SAMPLES; i++) {
        if (stacks[i] < STACK_LOW || stacks[i] >= STACK_HIGH)
            in_range = 0;
        if (maps[i] < MMAP_LOW || maps[i] >= MMAP_HIGH || (maps[i] & 4095))
            in_range = 0;
    }
    check("pilha muda a cada exec", all_distinct(stacks, SAMPLES));
    check("mmap muda a cada exec", all_distinct(maps, SAMPLES));
    check("enderecos dentro das faixas do ASLR", in_range);
    printf("aslrtest: %s (%d falha(s))\n", failures ? "FALHOU" : "TUDO OK", failures);
    return failures ? 1 : 0;
}
