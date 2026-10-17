#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

static int fails;

static void check(const char *what, int ok)
{
    printf("  %-52s %s\n", what, ok ? "ok" : "FALHA");
    if (!ok)
        fails++;
}

static long long ts_ns(const struct timespec *t)
{
    return (long long)t->tv_sec * 1000000000LL + t->tv_nsec;
}

static long long ts_ms(const struct timespec *t)
{
    return (long long)t->tv_sec * 1000 + t->tv_nsec / 1000000;
}

static long long tv_ms(const struct timeval *t)
{
    return (long long)t->tv_sec * 1000 + t->tv_usec / 1000;
}

static long long clock_ns(clockid_t c)
{
    struct timespec t;
    if (clock_gettime(c, &t) != 0)
        return -1;
    return ts_ns(&t);
}

static long long clock_ms(clockid_t c)
{
    struct timespec t;
    if (clock_gettime(c, &t) != 0)
        return -1;
    return ts_ms(&t);
}

static long long now_ms(void)
{
    return clock_ms(CLOCK_MONOTONIC);
}

static long my_tid(void)
{
    return syscall(SYS_gettid);
}

static void busy_ms(long ms)
{
    long long end = now_ms() + ms;
    volatile unsigned long x = 0;
    while (now_ms() < end)
        for (int i = 0; i < 20000; i++)
            x++;
}

static int wait_flag(volatile int *flag, long ms)
{
    long long t0 = now_ms();
    while (!*flag && now_ms() - t0 < ms)
        usleep(1000);
    return *flag;
}

static void set_handler(int sig, void (*fn)(int), int flags)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = fn;
    sa.sa_flags = flags;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

static volatile int int_count;
static volatile int alarm_count;
static volatile int term_count;
static volatile int usr1_count;
static volatile int usr2_count;
static volatile long usr1_tid;
static pthread_t usr1_self;
static pthread_t main_thread;

static void on_int(int sig)
{
    (void)sig;
    int_count++;
}

static void on_alarm(int sig)
{
    (void)sig;
    alarm_count++;
}

static void on_term(int sig)
{
    (void)sig;
    term_count++;
}

static void on_usr1(int sig)
{
    (void)sig;
    int e = errno;
    usr1_tid = my_tid();
    usr1_self = pthread_self();
    usr1_count++;
    errno = e;
}

static void on_usr2(int sig)
{
    (void)sig;
    usr2_count++;
}

static int regs_survive_signal(void)
{
    int before = alarm_count;
    unsigned long c, r;
    alarm(1);
    __asm__ volatile("movq $0x1234567890, %%rcx\n\t"
                     "movq $0x0fedcba987, %%r11\n\t"
                     "1:\n\t"
                     "pause\n\t"
                     "cmpl %[b], %[cnt]\n\t"
                     "je 1b\n\t"
                     "movq %%rcx, %[c]\n\t"
                     "movq %%r11, %[r]\n\t"
                     : [c] "=r"(c), [r] "=r"(r)
                     : [b] "r"(before), [cnt] "m"(alarm_count)
                     : "rcx", "r11", "cc", "memory");
    return c == 0x1234567890ul && r == 0x0fedcba987ul;
}

static void group_basic(void)
{
    signal(SIGINT, on_int);
    signal(SIGTERM, on_term);

    raise(SIGINT);
    check("handler de SIGINT rodou", int_count == 1);

    kill(getpid(), SIGINT);
    kill(getpid(), SIGTERM);
    check("dois sinais diferentes entregues", int_count == 2 && term_count == 1);

    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigprocmask(SIG_BLOCK, &block, &old);
    raise(SIGINT);
    check("sinal bloqueado nao e entregue", int_count == 2);
    sigprocmask(SIG_SETMASK, &old, NULL);
    check("sinal pendente entregue ao desbloquear", int_count == 3);

    signal(SIGINT, SIG_IGN);
    raise(SIGINT);
    check("SIG_IGN descarta o sinal", int_count == 3);
    signal(SIGINT, on_int);
}

static void group_alarm(void)
{
    signal(SIGALRM, on_alarm);
    long long t0 = now_ms();
    alarm(1);
    while (alarm_count == 0 && now_ms() - t0 < 4000)
        usleep(50000);
    long long dt = now_ms() - t0;
    check("alarm(1) disparou SIGALRM", alarm_count == 1);
    check("alarm demorou perto de 1 s", dt >= 900 && dt < 2500);
    if (alarm_count != 1)
        check("rcx e r11 sobrevivem a um sinal", 0);
    else
        check("rcx e r11 sobrevivem a um sinal", regs_survive_signal());
    alarm(5);
    unsigned left = alarm(0);
    check("alarm devolve os segundos que faltavam", left >= 4 && left <= 5);
}

static void group_processes(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGTERM, SIG_DFL);
        for (;;)
            usleep(50000);
    }
    usleep(200000);
    kill(pid, SIGTERM);
    int st = 0;
    waitpid(pid, &st, 0);
    check("filho morto por SIGTERM", WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM);
    check("WIFEXITED falso para morto por sinal", !WIFEXITED(st));

    pid = fork();
    if (pid == 0) {
        sigset_t all;
        sigfillset(&all);
        sigprocmask(SIG_BLOCK, &all, NULL);
        signal(SIGTERM, SIG_IGN);
        signal(SIGKILL, SIG_IGN);
        for (;;)
            usleep(50000);
    }
    usleep(200000);
    kill(pid, SIGTERM);
    kill(pid, SIGKILL);
    st = 0;
    waitpid(pid, &st, 0);
    check("SIGKILL mata mesmo ignorado", WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL);

    pid = fork();
    if (pid == 0)
        _exit(7);
    st = 0;
    waitpid(pid, &st, 0);
    check("saida normal codifica o codigo", WIFEXITED(st) && WEXITSTATUS(st) == 7);

    errno = 0;
    check("kill de pid inexistente da ESRCH", kill(31337, SIGTERM) < 0 && errno == ESRCH);
}

static void group_sigpending(void)
{
    set_handler(SIGUSR1, on_usr1, 0);
    sigset_t block, old, pend;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR1);
    sigprocmask(SIG_BLOCK, &block, &old);
    kill(getpid(), SIGUSR1);
    sigemptyset(&pend);
    int r = sigpending(&pend);
    check("sigpending mostra o sinal bloqueado pendente",
          r == 0 && sigismember(&pend, SIGUSR1) == 1 && sigismember(&pend, SIGUSR2) == 0 && usr1_count == 0);
    sigprocmask(SIG_SETMASK, &old, NULL);
    sigfillset(&pend);
    r = sigpending(&pend);
    check("sinal sai de sigpending depois de entregue", r == 0 && usr1_count == 1 && sigismember(&pend, SIGUSR1) == 0);
}

static void group_sigsuspend(void)
{
    set_handler(SIGUSR1, on_usr1, 0);
    sigset_t block, empty, cur;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR1);
    sigprocmask(SIG_BLOCK, &block, NULL);
    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid == 0) {
        usleep(100000);
        kill(parent, SIGUSR1);
        _exit(0);
    }
    sigemptyset(&empty);
    errno = 0;
    int r = sigsuspend(&empty);
    int e = errno;
    int hits = usr1_count;
    sigemptyset(&cur);
    sigprocmask(SIG_BLOCK, NULL, &cur);
    waitpid(pid, NULL, 0);
    check("sigsuspend volta com EINTR depois do handler", r == -1 && e == EINTR && hits == 1);
    check("sigsuspend restaura a mascara com SIGUSR1 bloqueado", sigismember(&cur, SIGUSR1) == 1);
}

static void *sigwait_sender(void *arg)
{
    (void)arg;
    usleep(50000);
    kill(getpid(), SIGUSR2);
    usleep(50000);
    pthread_kill(main_thread, SIGUSR1);
    return NULL;
}

static void group_sigtimedwait(void)
{
    set_handler(SIGUSR1, on_usr1, 0);
    set_handler(SIGUSR2, on_usr2, 0);
    sigset_t s1, s2, both, pend;
    sigemptyset(&s1);
    sigaddset(&s1, SIGUSR1);
    sigemptyset(&s2);
    sigaddset(&s2, SIGUSR2);
    sigemptyset(&both);
    sigaddset(&both, SIGUSR1);
    sigaddset(&both, SIGUSR2);
    sigprocmask(SIG_BLOCK, &both, NULL);

    kill(getpid(), SIGUSR2);
    siginfo_t si;
    memset(&si, 0, sizeof(si));
    si.si_code = -12345;
    struct timespec to = { 2, 0 };
    int r = sigtimedwait(&s2, &si, &to);
    sigfillset(&pend);
    sigpending(&pend);
    check("sigtimedwait recebe SIGUSR2 com si_signo", r == SIGUSR2 && si.si_signo == SIGUSR2);
    check("sigtimedwait da si_code SI_USER para kill", si.si_code == SI_USER);
    check("sigtimedwait consome o sinal sem rodar o handler", usr2_count == 0 && sigismember(&pend, SIGUSR2) == 0);

    to.tv_sec = 0;
    to.tv_nsec = 100 * 1000000;
    long long t0 = now_ms();
    errno = 0;
    r = sigtimedwait(&s2, &si, &to);
    int e = errno;
    long long dt = now_ms() - t0;
    printf("  sigtimedwait de 100 ms: %lld ms\n", dt);
    check("sigtimedwait sem sinal expira com EAGAIN", r == -1 && e == EAGAIN && dt >= 90 && dt < 2000);

    main_thread = pthread_self();
    pthread_t t;
    if (pthread_create(&t, NULL, sigwait_sender, NULL) != 0) {
        check("sigwaitinfo recebe kill feito por outra thread", 0);
        check("sigwait recebe pthread_kill de outra thread", 0);
        return;
    }
    memset(&si, 0, sizeof(si));
    r = sigwaitinfo(&s2, &si);
    check("sigwaitinfo recebe kill feito por outra thread", r == SIGUSR2 && si.si_signo == SIGUSR2);
    int sig = 0;
    r = sigwait(&s1, &sig);
    check("sigwait recebe pthread_kill de outra thread", r == 0 && sig == SIGUSR1 && usr1_count == 0 && usr2_count == 0);
    pthread_join(t, NULL);
}

static volatile int idle_ready;
static volatile int idle_stop;
static volatile long idle_tid;

static void *idle_worker(void *arg)
{
    (void)arg;
    idle_tid = my_tid();
    idle_ready = 1;
    for (int i = 0; i < 10000 && !idle_stop; i++)
        usleep(1000);
    return NULL;
}

static void group_thread_signals(void)
{
    set_handler(SIGUSR1, on_usr1, 0);
    pthread_t w;
    if (pthread_create(&w, NULL, idle_worker, NULL) != 0 || !wait_flag(&idle_ready, 3000)) {
        check("raise entrega o sinal a propria thread", 0);
        return;
    }
    long me = my_tid();
    raise(SIGUSR1);
    check("raise entrega o sinal a propria thread", usr1_count == 1 && usr1_tid == me);

    pthread_kill(w, SIGUSR1);
    long long t0 = now_ms();
    while (usr1_count < 2 && now_ms() - t0 < 2000)
        usleep(1000);
    check("pthread_kill entrega o sinal a thread alvo",
          usr1_count == 2 && usr1_tid == idle_tid && pthread_equal(usr1_self, w));

    errno = 0;
    long r = syscall(SYS_tgkill, getpid(), idle_tid, 0);
    check("tgkill com tgid e tid certos e sinal 0 da 0", r == 0);

    pid_t pid = fork();
    if (pid == 0) {
        for (;;)
            pause();
    }
    int before = usr1_count;
    errno = 0;
    r = syscall(SYS_tgkill, pid, idle_tid, SIGUSR1);
    int e = errno;
    usleep(20000);
    check("tgkill com tgid errado da ESRCH", r == -1 && e == ESRCH && usr1_count == before);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);

    errno = 0;
    r = syscall(SYS_tkill, 31337, SIGUSR1);
    check("tkill de tid inexistente da ESRCH", r == -1 && errno == ESRCH);

    idle_stop = 1;
    pthread_join(w, NULL);
}

static int rpipe[2];
static volatile int reader_ready;
static volatile int reader_done;
static volatile long reader_res;
static volatile int reader_err;

static void *pipe_reader(void *arg)
{
    (void)arg;
    char c;
    reader_ready = 1;
    errno = 0;
    long r = read(rpipe[0], &c, 1);
    reader_err = errno;
    reader_res = r;
    reader_done = 1;
    return NULL;
}

static int start_reader(pthread_t *t)
{
    reader_ready = 0;
    reader_done = 0;
    reader_res = -100;
    reader_err = 0;
    if (pipe(rpipe) != 0)
        return 0;
    if (pthread_create(t, NULL, pipe_reader, NULL) != 0)
        return 0;
    if (!wait_flag(&reader_ready, 3000))
        return 0;
    usleep(50000);
    return 1;
}

static void group_restart(void)
{
    pthread_t t;
    set_handler(SIGUSR1, on_usr1, SA_RESTART);
    usr1_count = 0;
    if (!start_reader(&t)) {
        check("read com SA_RESTART continua e le o dado", 0);
        return;
    }
    for (int i = 0; i < 4 && !reader_done; i++) {
        pthread_kill(t, SIGUSR1);
        usleep(30000);
    }
    int early = reader_done;
    write(rpipe[1], "x", 1);
    int done = wait_flag(&reader_done, 3000);
    if (done)
        pthread_join(t, NULL);
    check("read com SA_RESTART continua e le o dado", !early && done && reader_res == 1 && usr1_count >= 1);
    close(rpipe[0]);
    close(rpipe[1]);

    set_handler(SIGUSR1, on_usr1, 0);
    usr1_count = 0;
    if (!start_reader(&t)) {
        check("read sem SA_RESTART volta com -1 e EINTR", 0);
        return;
    }
    for (int i = 0; i < 100 && !reader_done; i++) {
        pthread_kill(t, SIGUSR1);
        long long t0 = now_ms();
        while (!reader_done && now_ms() - t0 < 20)
            usleep(1000);
    }
    if (!reader_done)
        write(rpipe[1], "x", 1);
    done = wait_flag(&reader_done, 3000);
    if (done)
        pthread_join(t, NULL);
    check("read sem SA_RESTART volta com -1 e EINTR", done && reader_res == -1 && reader_err == EINTR && usr1_count >= 1);
    close(rpipe[0]);
    close(rpipe[1]);
}

static volatile int sleeper_done;

static void *interrupter(void *arg)
{
    (void)arg;
    usleep(100000);
    for (int i = 0; i < 30 && !sleeper_done; i++) {
        pthread_kill(main_thread, SIGUSR1);
        usleep(100000);
    }
    return NULL;
}

static void group_nanosleep(void)
{
    set_handler(SIGUSR1, on_usr1, 0);
    struct timespec req = { 0, 50 * 1000000 };
    long long t0 = now_ms();
    int r = nanosleep(&req, NULL);
    long long dt = now_ms() - t0;
    check("nanosleep de 50 ms dura entre 45 e 2000 ms", r == 0 && dt >= 45 && dt < 2000);

    main_thread = pthread_self();
    sleeper_done = 0;
    pthread_t t;
    if (pthread_create(&t, NULL, interrupter, NULL) != 0) {
        check("nanosleep interrompido por sinal da EINTR", 0);
        return;
    }
    req.tv_sec = 2;
    req.tv_nsec = 0;
    struct timespec rem = { 0, 0 };
    t0 = now_ms();
    errno = 0;
    r = nanosleep(&req, &rem);
    int e = errno;
    dt = now_ms() - t0;
    sleeper_done = 1;
    pthread_join(t, NULL);
    long long rem_ms = ts_ms(&rem);
    printf("  nanosleep de 2 s interrompido em %lld ms, rem %lld ms\n", dt, rem_ms);
    check("nanosleep interrompido por sinal da EINTR", r == -1 && e == EINTR && usr1_count >= 1 && dt < 1900);
    check("nanosleep devolve em rem o tempo que faltou",
          rem_ms >= 1000 && rem_ms <= 2000 && rem.tv_nsec >= 0 && rem.tv_nsec < 1000000000);
}

static int abs_sleep_ok(clockid_t c)
{
    struct timespec now, deadline, after;
    if (clock_gettime(c, &now) != 0)
        return 0;
    deadline = now;
    deadline.tv_nsec += 50 * 1000000;
    if (deadline.tv_nsec >= 1000000000) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000;
    }
    int r = clock_nanosleep(c, TIMER_ABSTIME, &deadline, NULL);
    if (clock_gettime(c, &after) != 0)
        return 0;
    long long total = ts_ms(&after) - ts_ms(&now);
    return r == 0 && ts_ns(&after) >= ts_ns(&deadline) && total < 2000;
}

static void group_clock_nanosleep(void)
{
    struct timespec rel = { 0, 50 * 1000000 };
    long long t0 = now_ms();
    int r = clock_nanosleep(CLOCK_MONOTONIC, 0, &rel, NULL);
    long long dt = now_ms() - t0;
    check("clock_nanosleep relativo de 50 ms", r == 0 && dt >= 45 && dt < 2000);
    check("clock_nanosleep TIMER_ABSTIME em CLOCK_MONOTONIC", abs_sleep_ok(CLOCK_MONOTONIC));
    check("clock_nanosleep TIMER_ABSTIME em CLOCK_REALTIME", abs_sleep_ok(CLOCK_REALTIME));
}

static const clockid_t all_clocks[] = {
    CLOCK_REALTIME,         CLOCK_MONOTONIC,          CLOCK_MONOTONIC_RAW,     CLOCK_REALTIME_COARSE,
    CLOCK_MONOTONIC_COARSE, CLOCK_BOOTTIME,           CLOCK_PROCESS_CPUTIME_ID, CLOCK_THREAD_CPUTIME_ID,
};

static const clockid_t mono_clocks[] = {
    CLOCK_MONOTONIC, CLOCK_MONOTONIC_RAW, CLOCK_MONOTONIC_COARSE, CLOCK_BOOTTIME,
};

static void group_clocks(void)
{
    struct timeval tv = { 0, 0 };
    struct timespec rt = { 0, 0 };
    int g = gettimeofday(&tv, NULL);
    int c = clock_gettime(CLOCK_REALTIME, &rt);
    time_t tt = time(NULL);
    printf("  CLOCK_REALTIME: %lld s\n", (long long)rt.tv_sec);
    check("gettimeofday e CLOCK_REALTIME concordam em 2 s",
          g == 0 && c == 0 && tv.tv_usec >= 0 && tv.tv_usec < 1000000 && rt.tv_sec > 946684800 &&
              llabs(tv_ms(&tv) - ts_ms(&rt)) < 2000);
    check("time(NULL) concorda com CLOCK_REALTIME", tt != (time_t)-1 && llabs((long long)tt - (long long)rt.tv_sec) <= 2);

    int all_ok = 1;
    for (unsigned i = 0; i < sizeof(all_clocks) / sizeof(all_clocks[0]); i++) {
        struct timespec t = { -1, -1 };
        if (clock_gettime(all_clocks[i], &t) != 0 || t.tv_sec < 0 || t.tv_nsec < 0 || t.tv_nsec >= 1000000000) {
            printf("  relogio %d falhou\n", (int)all_clocks[i]);
            all_ok = 0;
        }
    }
    check("clock_gettime aceita os 8 relogios", all_ok);

    long long m1 = clock_ms(CLOCK_MONOTONIC);
    long long raw = clock_ms(CLOCK_MONOTONIC_RAW);
    long long boot = clock_ms(CLOCK_BOOTTIME);
    long long coarse = clock_ms(CLOCK_MONOTONIC_COARSE);
    long long m2 = clock_ms(CLOCK_MONOTONIC);
    check("MONOTONIC_RAW, BOOTTIME e COARSE perto do MONOTONIC",
          m1 >= 0 && raw >= m1 - 1000 && raw <= m2 + 1000 && boot >= m1 - 1000 && boot <= m2 + 1000 &&
              coarse >= m1 - 1000 && coarse <= m2 + 1000);

    long long r1 = clock_ms(CLOCK_REALTIME);
    long long rc = clock_ms(CLOCK_REALTIME_COARSE);
    long long r2 = clock_ms(CLOCK_REALTIME);
    check("REALTIME_COARSE perto do REALTIME", r1 > 0 && rc >= r1 - 1000 && rc <= r2 + 1000);

    int mono_ok = 1;
    long long last[4];
    for (unsigned j = 0; j < 4; j++)
        last[j] = clock_ns(mono_clocks[j]);
    for (int i = 0; i < 20; i++) {
        usleep(2000);
        for (unsigned j = 0; j < 4; j++) {
            long long v = clock_ns(mono_clocks[j]);
            if (v < 0 || v < last[j])
                mono_ok = 0;
            last[j] = v;
        }
    }
    check("relogios monotonicos nunca voltam para tras", mono_ok);

    struct timespec res = { -1, -1 };
    int rr = clock_getres(CLOCK_MONOTONIC, &res);
    printf("  clock_getres(CLOCK_MONOTONIC): %ld ns\n", (long)res.tv_nsec);
    check("clock_getres(CLOCK_MONOTONIC) positiva e ate 10 ms",
          rr == 0 && res.tv_sec == 0 && res.tv_nsec > 0 && res.tv_nsec <= 10000000);
}

static volatile int busy_started;

static void *busy_worker(void *arg)
{
    (void)arg;
    busy_started = 1;
    busy_ms(500);
    return NULL;
}

static long long ru_ms(const struct rusage *r)
{
    return tv_ms(&r->ru_utime) + tv_ms(&r->ru_stime);
}

static void group_cputime(void)
{
    struct rusage ru0, ru1;
    getrusage(RUSAGE_SELF, &ru0);
    long long p0 = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
    long long t0 = clock_ns(CLOCK_THREAD_CPUTIME_ID);
    busy_ms(300);
    long long p1 = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
    long long t1 = clock_ns(CLOCK_THREAD_CPUTIME_ID);
    int gr = getrusage(RUSAGE_SELF, &ru1);
    long long dp = (p1 - p0) / 1000000;
    long long dt = (t1 - t0) / 1000000;
    long long du = tv_ms(&ru1.ru_utime) - tv_ms(&ru0.ru_utime);
    printf("  laco de 300 ms: processo %lld ms, thread %lld ms, ru_utime %lld ms\n", dp, dt, du);
    check("CLOCK_PROCESS_CPUTIME_ID cresce com laco ocupado", p0 >= 0 && dp >= 50 && dp < 5000);
    check("CLOCK_THREAD_CPUTIME_ID cresce com laco ocupado", t0 >= 0 && dt >= 50 && dt <= dp + 20);
    check("getrusage(RUSAGE_SELF) conta ru_utime do laco", gr == 0 && du >= 50 && du < 5000);

    busy_started = 0;
    pthread_t w;
    if (pthread_create(&w, NULL, busy_worker, NULL) != 0 || !wait_flag(&busy_started, 3000)) {
        check("relogio da thread dormindo cresce menos", 0);
        return;
    }
    struct rusage rs0, rs1, rt0, rt1;
    long long pm0 = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
    long long tm0 = clock_ns(CLOCK_THREAD_CPUTIME_ID);
    getrusage(RUSAGE_SELF, &rs0);
    int g0 = getrusage(RUSAGE_THREAD, &rt0);
    usleep(300000);
    long long pm1 = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
    long long tm1 = clock_ns(CLOCK_THREAD_CPUTIME_ID);
    getrusage(RUSAGE_SELF, &rs1);
    int g1 = getrusage(RUSAGE_THREAD, &rt1);
    pthread_join(w, NULL);
    long long dpm = (pm1 - pm0) / 1000000;
    long long dtm = (tm1 - tm0) / 1000000;
    long long dself = ru_ms(&rs1) - ru_ms(&rs0);
    long long dthr = ru_ms(&rt1) - ru_ms(&rt0);
    printf("  thread dormindo: processo %lld ms, thread %lld ms, RUSAGE_SELF %lld ms, RUSAGE_THREAD %lld ms\n", dpm,
           dtm, dself, dthr);
    check("relogio da thread dormindo cresce menos", dpm >= 50 && dtm < dpm / 2 && dtm < 100);
    check("getrusage(RUSAGE_THREAD) so conta a propria thread",
          g0 == 0 && g1 == 0 && dself >= 50 && dthr >= 0 && dthr < dself / 2);
}

static void group_children(void)
{
    struct rusage c0, c1, wr;
    struct tms tm0, tm1;
    getrusage(RUSAGE_CHILDREN, &c0);
    clock_t k0 = times(&tm0);
    pid_t pid = fork();
    if (pid == 0) {
        busy_ms(300);
        _exit(3);
    }
    memset(&wr, 0, sizeof(wr));
    int st = 0;
    pid_t w = wait4(pid, &st, 0, &wr);
    int gr = getrusage(RUSAGE_CHILDREN, &c1);
    clock_t k1 = times(&tm1);
    long hz = sysconf(_SC_CLK_TCK);
    long long wu = tv_ms(&wr.ru_utime);
    long long cd = tv_ms(&c1.ru_utime) - tv_ms(&c0.ru_utime);
    long long cut = hz > 0 ? (long long)(tm1.tms_cutime - tm0.tms_cutime) * 1000 / hz : -1;
    long long kd = hz > 0 ? (long long)(k1 - k0) * 1000 / hz : -1;
    printf("  filho de 300 ms: wait4 %lld ms, RUSAGE_CHILDREN %lld ms, tms_cutime %lld ms, times %lld ms\n", wu, cd,
           cut, kd);
    check("wait4 devolve o status e o rusage do filho",
          w == pid && WIFEXITED(st) && WEXITSTATUS(st) == 3 && wu >= 50 && wu < 5000);
    check("getrusage(RUSAGE_CHILDREN) soma o filho esperado", gr == 0 && cd >= 50 && cd < 5000);
    check("times() avanca e tms_cutime conta o filho",
          k0 != (clock_t)-1 && k1 != (clock_t)-1 && k1 > 0 && kd >= 200 && kd < 10000 && cut >= 50 &&
              llabs(cut - cd) <= 100);
}

static long long itv_us(const struct timeval *t)
{
    return (long long)t->tv_sec * 1000000 + t->tv_usec;
}

static void group_itimer(void)
{
    set_handler(SIGALRM, on_alarm, SA_RESTART);
    struct itimerval it, cur, old;
    memset(&it, 0, sizeof(it));
    it.it_value.tv_usec = 50000;
    it.it_interval.tv_usec = 50000;
    long long t0 = now_ms();
    int r = setitimer(ITIMER_REAL, &it, NULL);
    memset(&cur, 0, sizeof(cur));
    int g = getitimer(ITIMER_REAL, &cur);
    long long v = itv_us(&cur.it_value);
    long long iv = itv_us(&cur.it_interval);
    check("getitimer mostra o tempo restante e o intervalo", r == 0 && g == 0 && v > 0 && v <= 51000 && iv >= 50000 &&
                                                              iv <= 51000);

    while (alarm_count < 3 && now_ms() - t0 < 2000)
        usleep(10000);
    long long dt = now_ms() - t0;
    int n = alarm_count;
    printf("  ITIMER_REAL: %d disparos em %lld ms\n", n, dt);
    check("ITIMER_REAL de 50 ms dispara SIGALRM 3 vezes", n >= 3 && dt >= 120 && dt < 1500);

    memset(&it, 0, sizeof(it));
    memset(&old, 0, sizeof(old));
    r = setitimer(ITIMER_REAL, &it, &old);
    int before = alarm_count;
    t0 = now_ms();
    while (now_ms() - t0 < 200)
        usleep(20000);
    memset(&cur, 0xff, sizeof(cur));
    g = getitimer(ITIMER_REAL, &cur);
    long long oiv = itv_us(&old.it_interval);
    check("setitimer com zero para o timer", r == 0 && g == 0 && alarm_count == before && oiv >= 50000 &&
                                                 oiv <= 51000 && itv_us(&cur.it_value) == 0 &&
                                                 itv_us(&cur.it_interval) == 0);
}

static volatile pid_t info_pid;
static volatile int info_code;
static volatile int info_uc_ok;

static void on_info(int sig, siginfo_t *si, void *ucv)
{
    (void)sig;
    ucontext_t *uc = ucv;
    info_pid = si->si_pid;
    info_code = si->si_code;
    info_uc_ok = uc != NULL && uc->uc_mcontext.gregs[REG_RIP] != 0 && uc->uc_mcontext.gregs[REG_RSP] != 0;
}

static int cancel_pipe[2];
static volatile int cancel_started;

static void *cancel_target(void *arg)
{
    (void)arg;
    char c;
    cancel_started = 1;
    read(cancel_pipe[0], &c, 1);
    return (void *)7;
}

static void group_siginfo(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_info;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGUSR1, &sa, NULL);
    kill(getpid(), SIGUSR1);
    check("SA_SIGINFO recebe si_pid do kill e o ucontext", info_pid == getpid() && info_code == SI_USER && info_uc_ok);

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGCHLD);
    sigprocmask(SIG_BLOCK, &set, NULL);
    pid_t child = fork();
    if (child == 0)
        _exit(3);
    struct timespec to = { 5, 0 };
    siginfo_t si;
    memset(&si, 0, sizeof(si));
    int got = sigtimedwait(&set, &si, &to);
    int st = 0;
    waitpid(child, &st, 0);
    check("sigtimedwait recebe SIGCHLD bloqueado com pid e status",
          got == SIGCHLD && si.si_pid == child && si.si_code == CLD_EXITED && si.si_status == 3);
    sigprocmask(SIG_UNBLOCK, &set, NULL);

    pipe(cancel_pipe);
    pthread_t t;
    pthread_create(&t, NULL, cancel_target, NULL);
    for (int i = 0; i < 500 && !cancel_started; i++)
        usleep(1000);
    usleep(50000);
    pthread_cancel(t);
    void *ret = NULL;
    pthread_join(t, &ret);
    check("pthread_cancel cancela a thread presa num read", ret == PTHREAD_CANCELED);
}

static void run_group(const char *name, void (*fn)(void), long timeout_ms)
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        fails = 0;
        fn();
        fflush(stdout);
        _exit(fails > 100 ? 100 : fails);
    }
    char msg[96];
    if (pid < 0) {
        snprintf(msg, sizeof(msg), "fork para o grupo %s", name);
        check(msg, 0);
        return;
    }
    long long t0 = now_ms();
    int st = 0;
    pid_t r;
    while ((r = waitpid(pid, &st, WNOHANG)) == 0 && now_ms() - t0 < timeout_ms)
        usleep(10000);
    if (r == 0) {
        kill(pid, SIGKILL);
        waitpid(pid, &st, 0);
        snprintf(msg, sizeof(msg), "grupo %s terminou no prazo", name);
        check(msg, 0);
        return;
    }
    if (r < 0 || !WIFEXITED(st)) {
        snprintf(msg, sizeof(msg), "grupo %s terminou normalmente (sinal %d)", name,
                 r > 0 && WIFSIGNALED(st) ? WTERMSIG(st) : 0);
        check(msg, 0);
        return;
    }
    fails += WEXITSTATUS(st);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("sigtest: sinais e tempo contra a musl\n");

    run_group("basico", group_basic, 6000);
    run_group("alarm", group_alarm, 10000);
    run_group("processos", group_processes, 6000);
    run_group("sigpending", group_sigpending, 6000);
    run_group("sigsuspend", group_sigsuspend, 6000);
    run_group("sigtimedwait", group_sigtimedwait, 8000);
    run_group("sinais por thread", group_thread_signals, 8000);
    run_group("SA_RESTART", group_restart, 10000);
    run_group("nanosleep", group_nanosleep, 8000);
    run_group("clock_nanosleep", group_clock_nanosleep, 6000);
    run_group("relogios", group_clocks, 6000);
    run_group("tempo de CPU", group_cputime, 10000);
    run_group("rusage de filhos", group_children, 10000);
    run_group("setitimer", group_itimer, 8000);
    run_group("siginfo e cancelamento", group_siginfo, 8000);

    printf("sigtest: fim, %s\n", fails ? "FALHOU" : "TUDO OK");
    return fails ? 1 : 0;
}
