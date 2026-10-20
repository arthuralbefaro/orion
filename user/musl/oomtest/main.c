#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHUNK (16u << 20)
#define WAIT_MS 120000

struct meminfo {
    long total_kb;
    long free_kb;
    long kills;
    long self_kills;
};

static volatile uint64_t *filled;
static int failures;

static void check(int ok, const char *what)
{
    printf("oomtest: %s  %s\n", what, ok ? "ok" : "FALHOU");
    fflush(stdout);
    if (!ok)
        failures++;
}

static void read_meminfo(struct meminfo *m)
{
    memset(m, 0, sizeof(*m));
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f)
        return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        sscanf(line, "MemTotal: %ld kB", &m->total_kb);
        sscanf(line, "MemFree: %ld kB", &m->free_kb);
        sscanf(line, "OomKills: %ld", &m->kills);
        sscanf(line, "OomSelfKills: %ld", &m->self_kills);
    }
    fclose(f);
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void filler(int idx, int go_fd)
{
    char c;
    if (go_fd >= 0 && read(go_fd, &c, 1) != 1)
        _exit(3);
    for (;;) {
        uint8_t *p = mmap(NULL, CHUNK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            struct timespec ts = { 0, 10 * 1000 * 1000 };
            nanosleep(&ts, NULL);
            continue;
        }
        for (uint32_t off = 0; off < CHUNK; off += 4096) {
            p[off] = (uint8_t)off;
            __atomic_add_fetch(&filled[idx], 4096, __ATOMIC_RELAXED);
        }
    }
}

static int wait_child(pid_t pid, int *status, double deadline)
{
    double start = now_ms(), next = start + 1000;
    while (now_ms() < deadline) {
        pid_t r = waitpid(pid, status, WNOHANG);
        if (r == pid)
            return 0;
        if (now_ms() >= next) {
            printf("oomtest:   %.0f s: %llu, %llu, %llu, %llu MiB\n", (now_ms() - start) / 1000,
                   (unsigned long long)(filled[0] >> 20), (unsigned long long)(filled[1] >> 20),
                   (unsigned long long)(filled[3] >> 20), (unsigned long long)(filled[4] >> 20));
            next += 1000;
        }
        struct timespec ts = { 0, 50 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    kill(pid, SIGKILL);
    waitpid(pid, status, 0);
    return -1;
}

static int by_sigkill(int st)
{
    return WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL;
}

static void test_alone(void)
{
    struct meminfo a, b;
    read_meminfo(&a);
    filled[0] = 0;
    double t0 = now_ms();
    pid_t pid = fork();
    if (pid == 0)
        filler(0, -1);
    int st = 0;
    int timed_out = wait_child(pid, &st, t0 + WAIT_MS);
    read_meminfo(&b);
    char what[256];
    snprintf(what, sizeof(what),
             "sozinho: o filho encheu %llu MiB em %.1f s e morreu com %s; OomKills +%ld, OomSelfKills +%ld (matou a si mesmo)",
             (unsigned long long)(filled[0] >> 20), (now_ms() - t0) / 1000, by_sigkill(st) ? "SIGKILL" : "outra coisa",
             b.kills - a.kills, b.self_kills - a.self_kills);
    check(!timed_out && by_sigkill(st) && b.kills - a.kills == 1 && b.self_kills - a.self_kills == 1, what);
}

static void test_two(void)
{
    struct meminfo a, b;
    read_meminfo(&a);
    filled[1] = filled[2] = 0;
    int go[2];
    if (pipe(go) < 0) {
        check(0, "dois: pipe");
        return;
    }
    double t0 = now_ms();
    pid_t p1 = fork();
    if (p1 == 0) {
        close(go[1]);
        filler(1, go[0]);
    }
    pid_t p2 = fork();
    if (p2 == 0) {
        close(go[1]);
        filler(2, go[0]);
    }
    close(go[0]);
    if (write(go[1], "gg", 2) != 2)
        check(0, "dois: largada");
    close(go[1]);
    int s1 = 0, s2 = 0;
    int t1 = wait_child(p1, &s1, t0 + WAIT_MS);
    int t2 = wait_child(p2, &s2, t0 + WAIT_MS);
    read_meminfo(&b);
    char what[256];
    snprintf(what, sizeof(what),
             "dois: enchendo juntos, %llu e %llu MiB, os dois morreram com SIGKILL: %s; OomKills +%ld",
             (unsigned long long)(filled[1] >> 20), (unsigned long long)(filled[2] >> 20),
             by_sigkill(s1) && by_sigkill(s2) ? "sim" : "nao", b.kills - a.kills);
    check(!t1 && !t2 && by_sigkill(s1) && by_sigkill(s2) && b.kills - a.kills == 2, what);
}

static void hog(uint64_t want, int ready_fd)
{
    while (filled[3] < want) {
        uint8_t *p = mmap(NULL, CHUNK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED)
            break;
        for (uint32_t off = 0; off < CHUNK; off += 4096) {
            p[off] = 1;
            __atomic_add_fetch(&filled[3], 4096, __ATOMIC_RELAXED);
        }
    }
    if (write(ready_fd, "r", 1) != 1)
        _exit(4);
    for (;;)
        pause();
}

static void test_other(long free_kb)
{
    struct meminfo a, b;
    read_meminfo(&a);
    filled[3] = filled[4] = 0;
    int ready[2];
    if (pipe(ready) < 0) {
        check(0, "outro: pipe");
        return;
    }
    double t0 = now_ms();
    pid_t big = fork();
    if (big == 0) {
        close(ready[0]);
        hog((uint64_t)free_kb * 1024 * 6 / 10, ready[1]);
    }
    close(ready[1]);
    char c;
    int up = read(ready[0], &c, 1) == 1;
    close(ready[0]);
    pid_t small = fork();
    if (small == 0)
        filler(4, -1);
    int sb = 0, ss = 0;
    int tb = wait_child(big, &sb, t0 + WAIT_MS);
    uint64_t small_at_kill = filled[4];
    int ts = wait_child(small, &ss, t0 + WAIT_MS);
    read_meminfo(&b);
    char what[320];
    snprintf(what, sizeof(what),
             "outro: o parado com %llu MiB morreu primeiro (SIGKILL: %s) com o que enchia em %llu MiB, que seguiu ate "
             "%llu MiB e morreu depois (SIGKILL: %s); OomKills +%ld, OomSelfKills +%ld",
             (unsigned long long)(filled[3] >> 20), by_sigkill(sb) ? "sim" : "nao",
             (unsigned long long)(small_at_kill >> 20), (unsigned long long)(filled[4] >> 20),
             by_sigkill(ss) ? "sim" : "nao", b.kills - a.kills, b.self_kills - a.self_kills);
    check(up && !tb && !ts && by_sigkill(sb) && by_sigkill(ss) && filled[4] > small_at_kill &&
              b.kills - a.kills == 2 && b.self_kills - a.self_kills == 1,
          what);
}

static void test_refuse(long total_kb)
{
    uint64_t huge = (uint64_t)total_kb * 1024 * 2;
    errno = 0;
    void *p = mmap(NULL, huge, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int e1 = errno;
    if (p != MAP_FAILED)
        munmap(p, huge);
    void *r = mmap(NULL, huge, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int reserved = r != MAP_FAILED;
    if (reserved)
        munmap(r, huge);
    void *cur = sbrk(0);
    errno = 0;
    void *b = sbrk((intptr_t)huge);
    int e2 = errno;
    char what[256];
    snprintf(what, sizeof(what),
             "recusa: mmap de %llu MiB para escrita da %s, sbrk do mesmo tamanho da %s, reserva PROT_NONE aceita: %s",
             (unsigned long long)(huge >> 20), p == MAP_FAILED ? strerror(e1) : "certo",
             b == (void *)-1 ? strerror(e2) : "certo", reserved ? "sim" : "nao");
    check(p == MAP_FAILED && e1 == ENOMEM && b == (void *)-1 && sbrk(0) == cur && reserved, what);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    filled = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (filled == MAP_FAILED) {
        printf("oomtest: mmap compartilhado falhou\noomtest: fim\n");
        return 1;
    }
    struct meminfo start, end;
    read_meminfo(&start);
    printf("oomtest: %ld MiB de RAM, %ld MiB livres no comeco\n", start.total_kb / 1024, start.free_kb / 1024);
    test_refuse(start.total_kb);
    test_alone();
    test_other(start.free_kb);
    test_two();
    for (int i = 0; i < 50; i++) {
        read_meminfo(&end);
        if (end.free_kb >= start.free_kb - 8192)
            break;
        struct timespec ts = { 0, 100 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    char what[160];
    snprintf(what, sizeof(what), "memoria livre volta: %ld MiB antes, %ld MiB depois", start.free_kb / 1024,
             end.free_kb / 1024);
    check(end.free_kb >= start.free_kb - 8192, what);
    if (failures)
        printf("oomtest: FALHOU (%d)\n", failures);
    else
        printf("oomtest: TUDO OK\n");
    printf("oomtest: fim\n");
    return failures ? 1 : 0;
}
