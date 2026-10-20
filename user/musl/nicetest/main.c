#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct job {
    int nice;
    int cpu;
};

struct result {
    int idx;
    long ms;
};

static int failures;

static double now_s(clockid_t clk)
{
    struct timespec ts;
    clock_gettime(clk, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void check(int ok, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("nicetest: ");
    vprintf(fmt, ap);
    va_end(ap);
    printf("  %s\n", ok ? "ok" : "FALHOU");
    if (!ok)
        failures++;
    fflush(stdout);
}

static int pin(int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return sched_setaffinity(0, sizeof(set), &set);
}

static void spin_child(const struct job *j, int idx, int secs, int ready_fd, int go_fd, int res_fd)
{
    if (j->cpu >= 0 && pin(j->cpu) < 0)
        _exit(3);
    if (j->nice && setpriority(PRIO_PROCESS, 0, j->nice) < 0)
        _exit(4);
    char c = 'r';
    if (write(ready_fd, &c, 1) != 1 || read(go_fd, &c, 1) != 1)
        _exit(5);
    double c0 = now_s(CLOCK_THREAD_CPUTIME_ID);
    double w0 = now_s(CLOCK_MONOTONIC);
    volatile unsigned long x = 0;
    for (;;) {
        for (int k = 0; k < 4096; k++)
            x += k;
        if (now_s(CLOCK_MONOTONIC) - w0 >= secs)
            break;
    }
    struct result r = { idx, (long)((now_s(CLOCK_THREAD_CPUTIME_ID) - c0) * 1000) };
    if (write(res_fd, &r, sizeof(r)) != sizeof(r))
        _exit(6);
    _exit(0);
}

static int run_group(const struct job *jobs, int n, int secs, long *ms)
{
    int ready[2], go[2], res[2];
    if (pipe(ready) < 0 || pipe(go) < 0 || pipe(res) < 0) {
        perror("nicetest: pipe");
        return -1;
    }
    pid_t pids[64];
    for (int i = 0; i < n; i++) {
        pids[i] = fork();
        if (pids[i] < 0) {
            perror("nicetest: fork");
            return -1;
        }
        if (pids[i] == 0) {
            close(ready[0]);
            close(go[1]);
            close(res[0]);
            spin_child(&jobs[i], i, secs, ready[1], go[0], res[1]);
        }
    }
    close(ready[1]);
    close(go[0]);
    close(res[1]);
    char c;
    int got = 0;
    for (int i = 0; i < n; i++)
        if (read(ready[0], &c, 1) == 1)
            got++;
    for (int i = 0; i < n; i++)
        if (write(go[1], "g", 1) != 1)
            break;
    int results = 0;
    struct result r;
    while (results < n && read(res[0], &r, sizeof(r)) == sizeof(r)) {
        if (r.idx >= 0 && r.idx < n)
            ms[r.idx] = r.ms;
        results++;
    }
    int bad = 0;
    for (int i = 0; i < n; i++) {
        int st = 0;
        waitpid(pids[i], &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            printf("nicetest: filho %d saiu com %d\n", i, WIFEXITED(st) ? WEXITSTATUS(st) : -1);
            bad++;
        }
    }
    close(ready[0]);
    close(go[1]);
    close(res[0]);
    return got == n && results == n && !bad ? 0 : -1;
}

static void test_pinned(int ncpu)
{
    int cpu = ncpu > 1 ? 1 : 0;
    struct job jobs[2] = { { 0, cpu }, { 10, cpu } };
    long ms[2] = { 0, 0 };
    int secs = 4;
    if (run_group(jobs, 2, secs, ms) < 0) {
        check(0, "fixa: grupo nao rodou");
        return;
    }
    double ratio = ms[1] > 0 ? (double)ms[0] / ms[1] : 999;
    check(ratio >= 5 && ratio <= 15 && ms[0] + ms[1] >= secs * 900,
          "fixa na CPU %d: nice 0 %ld ms, nice 10 %ld ms de CPU em %d s, razao %.1f:1 (aceito 5:1 a 15:1)", cpu,
          ms[0], ms[1], secs, ratio);
}

static void test_spread(int ncpu)
{
    int n = 2 * ncpu;
    struct job jobs[64];
    long ms[64];
    for (int i = 0; i < n; i++) {
        jobs[i].nice = 0;
        jobs[i].cpu = -1;
        ms[i] = 0;
    }
    int secs = 3;
    if (run_group(jobs, n, secs, ms) < 0) {
        check(0, "espalha: grupo nao rodou");
        return;
    }
    long total = 0, lo = ms[0], hi = ms[0];
    for (int i = 0; i < n; i++) {
        total += ms[i];
        if (ms[i] < lo)
            lo = ms[i];
        if (ms[i] > hi)
            hi = ms[i];
    }
    long wall = (long)secs * 1000;
    check(lo >= wall * 35 / 100 && total >= (long)ncpu * wall * 85 / 100,
          "espalha: %d threads de nice 0 em %d CPUs, de %ld a %ld ms cada (ideal %ld), total %ld de %ld ms", n,
          ncpu, lo, hi, wall / 2, total, (long)ncpu * wall);
}

static void test_mix(int ncpu)
{
    int n = 2 * ncpu;
    struct job jobs[64];
    long ms[64];
    for (int i = 0; i < n; i++) {
        jobs[i].nice = i % 2 ? 10 : 0;
        jobs[i].cpu = -1;
        ms[i] = 0;
    }
    int secs = 4;
    if (run_group(jobs, n, secs, ms) < 0) {
        check(0, "mistura: grupo nao rodou");
        return;
    }
    long s0 = 0, s10 = 0, lo0 = ms[0];
    for (int i = 0; i < n; i++) {
        if (i % 2) {
            s10 += ms[i];
        } else {
            s0 += ms[i];
            if (ms[i] < lo0)
                lo0 = ms[i];
        }
    }
    double ratio = s10 > 0 ? (double)s0 / s10 : 999;
    long wall = (long)secs * 1000;
    check(ratio >= 5 && ratio <= 15 && lo0 >= wall * 60 / 100,
          "mistura: %d de nice 0 e %d de nice 10 em %d CPUs, nice 0 %ld ms, nice 10 %ld ms, razao %.1f:1, "
          "pior nice 0 %ld de %ld ms",
          ncpu, ncpu, ncpu, s0 / ncpu, s10 / ncpu, ratio, lo0, wall);
}

static void test_affinity(int ncpu)
{
    int target = ncpu - 1;
    int r = pin(target);
    int first = sched_getcpu();
    int moved = 0;
    double w0 = now_s(CLOCK_MONOTONIC);
    while (now_s(CLOCK_MONOTONIC) - w0 < 0.3)
        if (sched_getcpu() != target)
            moved++;
    check(r == 0 && first == target && moved == 0,
          "afinidade: sched_setaffinity para a CPU %d, ja na volta esta na CPU %d, saiu dela %d vez(es)", target,
          first, moved);
    cpu_set_t set;
    CPU_ZERO(&set);
    errno = 0;
    r = sched_setaffinity(0, sizeof(set), &set);
    check(r < 0 && errno == EINVAL, "afinidade: mascara vazia da EINVAL (%s)", strerror(errno));
    CPU_ZERO(&set);
    for (int i = 0; i < ncpu; i++)
        CPU_SET(i, &set);
    sched_setaffinity(0, sizeof(set), &set);
    cpu_set_t back;
    CPU_ZERO(&back);
    sched_getaffinity(0, sizeof(back), &back);
    check(CPU_COUNT(&back) == ncpu, "afinidade: sched_getaffinity devolve %d CPU(s)", CPU_COUNT(&back));
}

static void test_permissions(void)
{
    if (geteuid() != 0) {
        printf("nicetest: permissoes: pulado (precisa de root)\n");
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        int bad = 0;
        if (setuid(1000) < 0)
            _exit(10);
        errno = 0;
        int r = setpriority(PRIO_PROCESS, 0, 5);
        printf("nicetest: usuario 1000: subir o nice para 5: %s\n", r == 0 ? "aceito" : strerror(errno));
        bad += r != 0;
        errno = 0;
        int got = getpriority(PRIO_PROCESS, 0);
        printf("nicetest: usuario 1000: getpriority devolve %d\n", got);
        bad += got != 5;
        errno = 0;
        r = setpriority(PRIO_PROCESS, 0, 4);
        printf("nicetest: usuario 1000: baixar o nice para 4: %s\n", r == 0 ? "aceito" : strerror(errno));
        bad += !(r < 0 && errno == EACCES);
        errno = 0;
        r = setpriority(PRIO_PROCESS, 1, 10);
        printf("nicetest: usuario 1000: mexer no nice do pid 1: %s\n", r == 0 ? "aceito" : strerror(errno));
        bad += !(r < 0 && errno == EPERM);
        errno = 0;
        r = setpriority(PRIO_PGRP, 0, 6);
        printf("nicetest: PRIO_PGRP: %s\n", r == 0 ? "aceito" : strerror(errno));
        bad += !(r < 0 && errno == EINVAL);
        errno = 0;
        r = setpriority(PRIO_PROCESS, 99999, 6);
        printf("nicetest: pid que nao existe: %s\n", r == 0 ? "aceito" : strerror(errno));
        bad += !(r < 0 && errno == ESRCH);
        fflush(stdout);
        _exit(bad);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    check(WIFEXITED(st) && WEXITSTATUS(st) == 0, "permissoes: EACCES para baixar, EPERM em outro usuario, "
          "EINVAL fora de PRIO_PROCESS, ESRCH sem processo");
}

int main(int argc, char **argv)
{
    int times = argc > 1 ? atoi(argv[1]) : 1;
    if (times < 1)
        times = 1;
    int ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu < 1)
        ncpu = 1;
    if (ncpu > 32)
        ncpu = 32;
    printf("nicetest: %d CPU(s), %d rodada(s)\n", ncpu, times);
    for (int k = 0; k < times; k++) {
        if (times > 1)
            printf("nicetest: rodada %d\n", k + 1);
        test_pinned(ncpu);
        test_spread(ncpu);
        test_mix(ncpu);
    }
    test_affinity(ncpu);
    test_permissions();
    if (failures)
        printf("nicetest: FALHOU (%d)\n", failures);
    else
        printf("nicetest: TUDO OK\n");
    printf("nicetest: fim\n");
    return failures ? 1 : 0;
}
