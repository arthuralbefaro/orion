#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define USER_CS 0x23
#define USER_SS 0x1b
#define MAX_THREADS 64

struct counts {
    unsigned long after_syscall;
    unsigned long after_migration;
    unsigned long spinning;
    unsigned long in_handler;
    unsigned long bad;
    unsigned long last_cs;
    unsigned long last_ss;
};

struct worker {
    pthread_t tid;
    int index;
    int cpus;
    double deadline;
    struct counts c;
};

static volatile sig_atomic_t handler_reads;
static volatile sig_atomic_t handler_bad;
static volatile sig_atomic_t signals;

static inline unsigned long read_cs(void)
{
    unsigned long v;
    __asm__ volatile("movq %%cs, %0" : "=r"(v));
    return v;
}

static inline unsigned long read_ss(void)
{
    unsigned long v;
    __asm__ volatile("movq %%ss, %0" : "=r"(v));
    return v;
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void check(struct counts *c, unsigned long *where)
{
    unsigned long cs = read_cs(), ss = read_ss();
    (*where)++;
    if (cs != USER_CS || ss != USER_SS) {
        c->bad++;
        c->last_cs = cs;
        c->last_ss = ss;
    }
}

static void on_alarm(int sig)
{
    (void)sig;
    signals++;
    handler_reads++;
    if (read_cs() != USER_CS || read_ss() != USER_SS)
        handler_bad++;
}

static void *work(void *arg)
{
    struct worker *w = arg;
    unsigned long round = 0;
    while (now() < w->deadline) {
        syscall(SYS_getpid);
        check(&w->c, &w->c.after_syscall);
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET((int)((w->index + round) % (unsigned long)w->cpus), &set);
        sched_setaffinity(0, sizeof(set), &set);
        check(&w->c, &w->c.after_migration);
        double until = now() + 0.0003;
        while (now() < until)
            check(&w->c, &w->c.spinning);
        if (round % 8 == 0)
            sched_yield();
        round++;
    }
    return NULL;
}

static unsigned long iret_checks(unsigned long *fixed)
{
    FILE *f = fopen("/proc/interrupts", "r");
    char line[256];
    unsigned long checks = 0;
    *fixed = 0;
    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f))
        if (sscanf(line, "IRT: %lu volta(s) ao anel 3 por iretq conferida(s), %lu", &checks, fixed) == 2)
            break;
    fclose(f);
    return checks;
}

static int child_run(double seconds)
{
    struct counts c;
    memset(&c, 0, sizeof(c));
    double deadline = now() + seconds;
    while (now() < deadline) {
        syscall(SYS_getppid);
        check(&c, &c.after_syscall);
    }
    return c.bad ? 1 : 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    double seconds = argc > 1 ? atof(argv[1]) : 4.0;
    if (seconds <= 0)
        seconds = 4.0;
    int cpus = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (cpus < 1)
        cpus = 1;
    int threads = cpus * 2 > MAX_THREADS ? MAX_THREADS : cpus * 2;
    unsigned long fixed0, fixed1;
    unsigned long iret0 = iret_checks(&fixed0);
    printf("segtest: CS=%lx SS=%lx no comeco (esperado CS=%x SS=%x); %d CPU(s), %d thread(s), %.1f s\n", read_cs(),
           read_ss(), USER_CS, USER_SS, cpus, threads, seconds);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_alarm;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);
    struct itimerval it = { { 0, 2000 }, { 0, 2000 } };
    setitimer(ITIMER_REAL, &it, NULL);

    pid_t kids[4];
    int nkids = 0;
    for (int i = 0; i < 4; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            struct itimerval off = { { 0, 0 }, { 0, 0 } };
            setitimer(ITIMER_REAL, &off, NULL);
            _exit(child_run(seconds / 2));
        }
        if (pid > 0)
            kids[nkids++] = pid;
    }

    static struct worker w[MAX_THREADS];
    double deadline = now() + seconds;
    for (int i = 0; i < threads; i++) {
        w[i].index = i;
        w[i].cpus = cpus;
        w[i].deadline = deadline;
        pthread_create(&w[i].tid, NULL, work, &w[i]);
    }
    struct counts total;
    memset(&total, 0, sizeof(total));
    for (int i = 0; i < threads; i++) {
        pthread_join(w[i].tid, NULL);
        total.after_syscall += w[i].c.after_syscall;
        total.after_migration += w[i].c.after_migration;
        total.spinning += w[i].c.spinning;
        total.bad += w[i].c.bad;
        if (w[i].c.bad) {
            total.last_cs = w[i].c.last_cs;
            total.last_ss = w[i].c.last_ss;
        }
    }
    struct itimerval off = { { 0, 0 }, { 0, 0 } };
    setitimer(ITIMER_REAL, &off, NULL);
    int kids_bad = 0;
    for (int i = 0; i < nkids; i++) {
        int st = 0;
        waitpid(kids[i], &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
            kids_bad++;
    }
    unsigned long iret1 = iret_checks(&fixed1);
    unsigned long bad = total.bad + (unsigned long)handler_bad;

    printf("segtest: leituras de CS/SS: %lu depois de syscall, %lu depois de trocar de CPU, %lu em laco preemptado, "
           "%d em handler de sinal (%d sinais)\n", total.after_syscall, total.after_migration, total.spinning,
           (int)handler_reads, (int)signals);
    printf("segtest: %d filho(s) de fork conferiram CS/SS, %d com erro\n", nkids, kids_bad);
    printf("segtest: o kernel conferiu %lu volta(s) ao anel 3 por iretq durante o teste, %lu com seletor errado\n",
           iret1 - iret0, fixed1 - fixed0);
    if (bad)
        printf("segtest: %lu leitura(s) erradas, a ultima com CS=%lx SS=%lx\n", bad, total.last_cs, total.last_ss);
    int ok = !bad && !kids_bad && signals > 0 && iret1 > iret0 && fixed1 == fixed0;
    printf("segtest: %s\n", ok ? "TUDO OK" : "FALHOU");
    printf("segtest: fim\n");
    return ok ? 0 : 1;
}
