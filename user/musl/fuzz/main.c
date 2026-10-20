#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <grp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <orion.h>

#define RING 32
#define WORKERS_MAX 8
#define CALLS_PER_CHILD 3000
#define STALL_MS 500
#define KILL_GRACE_MS 5000
#define GRAND_MAX 16
#define FUZZ_UID 1000
#define FUZZ_DIR "/tmp/fuzz"
#define SANDBOX_SIZE (4u << 20)
#define NFDS 16

struct call {
    uint64_t nr;
    uint64_t a[6];
    int64_t ret;
};

struct slot {
    volatile uint64_t count;
    volatile uint64_t pos;
    volatile uint64_t seed;
    volatile int grand;
    struct call ring[RING];
};

struct shared {
    volatile uint64_t total;
    struct slot slots[WORKERS_MAX];
};

static struct shared *sh;
static struct slot *my_slot;
static uint64_t rng;
static int fds[NFDS];
static int nfds;
static uint8_t *edge;
static uint8_t *bigbuf;
static uint8_t *sandbox;

static uint64_t next_rand(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static uint64_t pick(uint64_t n)
{
    return n ? next_rand() % n : 0;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static const uint64_t sizes[] = { 0, 1, 4095, 4096, 4097, 65536, 0x7fffffff, 0x80000000ull, 0xffffffffull,
                                  1ull << 40, ~0ull, ~0ull >> 1 };

static const char *const paths[] = { "f0", "f1", "f2", "d0", "d0/f3", ".", "..", "", "/", "/proc/self/stat",
                                     "/proc/self/maps", "/proc", "/dev/null", "/dev/zero", "/tmp/fuzz/f1",
                                     "/nao/existe", "/etc/passwd", "/etc/shadow", "/bin/sh", "f0/x" };

static uint64_t kernel_ptr(void)
{
    static const uint64_t bases[] = { 0xffffffff80000000ull, 0xffff800000000000ull, 0xfffffffffffff000ull,
                                      0x0000800000000000ull, 0x00007ffffffff000ull, 0x8000000000000000ull };
    return bases[pick(6)] + (pick(2) ? 0 : pick(0x100000));
}

static uint64_t edge_ptr(void)
{
    static const uint64_t back[] = { 1, 2, 4, 8, 16, 64, 512, 4096 };
    return (uint64_t)(edge + 4096 - back[pick(8)]);
}

static uint64_t pointer_arg(void)
{
    switch (pick(8)) {
    case 0: return 0;
    case 1: return kernel_ptr();
    case 2: return edge_ptr();
    case 3: return edge_ptr();
    case 4: return (uint64_t)(bigbuf + pick(65536 - 256));
    case 5: return (uint64_t)paths[pick(sizeof(paths) / sizeof(paths[0]))];
    case 6: return next_rand();
    default: return (uint64_t)(edge + 4096);
    }
}

static uint64_t fd_arg(void)
{
    if (pick(8) == 0)
        return pick(4) ? pick(1024) : (uint64_t)(int64_t)-1;
    return nfds ? (uint64_t)fds[pick(nfds)] : 0;
}

static uint64_t any_arg(void)
{
    switch (pick(10)) {
    case 0:
    case 1:
    case 2: return fd_arg();
    case 3:
    case 4: return sizes[pick(sizeof(sizes) / sizeof(sizes[0]))];
    case 5:
    case 6: return pointer_arg();
    case 7: return pick(64);
    case 8: return 1ull << pick(32);
    default: return next_rand();
    }
}

static uint64_t sandbox_addr(void)
{
    switch (pick(4)) {
    case 0: return (uint64_t)sandbox + (pick(SANDBOX_SIZE / 4096) << 12);
    case 1: return (uint64_t)sandbox + pick(SANDBOX_SIZE);
    case 2: return kernel_ptr();
    default: return pick(2) ? 0 : 4096;
    }
}

static void clamp_len(uint64_t addr, uint64_t *len)
{
    uint64_t lo = (uint64_t)sandbox, hi = lo + SANDBOX_SIZE;
    if (addr >= lo && addr < hi) {
        if (*len > hi - addr)
            *len = pick(hi - addr + 1);
    } else if (addr < 0x0000800000000000ull) {
        if (*len > 65536)
            *len = pick(65537);
    }
}

static void sanitize(uint64_t *nr, uint64_t *a, int *self)
{
    pid_t me = getpid();
    switch (*nr) {
    case SYS_mmap:
    case SYS_munmap:
    case SYS_mprotect:
    case SYS_madvise:
    case SYS_mremap:
    case SYS_msync:
    case SYS_mincore:
    case SYS_mlock:
    case SYS_munlock:
    case SYS_shmdt:
    case SYS_remap_file_pages:
    case SYS_pkey_mprotect:
        a[0] = sandbox_addr();
        if (*nr != SYS_shmdt)
            clamp_len(a[0], &a[1]);
        if (*nr == SYS_mremap) {
            a[4] = sandbox_addr();
            clamp_len(a[4], &a[2]);
        }
        if (*nr == SYS_mmap)
            a[3] &= ~(uint64_t)(MAP_POPULATE | MAP_LOCKED);
        break;
    case SYS_shmat:
        a[1] = sandbox_addr();
        break;
    case SYS_process_vm_readv:
    case SYS_process_vm_writev:
        *nr = SYS_getpid;
        break;
    case SYS_brk:
        a[0] = pick(2) ? 0 : kernel_ptr();
        break;
    case SYS_arch_prctl:
        if (a[0] == 0x1001 || a[0] == 0x1002)
            a[0] = 0x1003 + pick(2);
        break;
    case SYS_rt_sigreturn:
    case SYS_exit:
    case SYS_exit_group:
    case SYS_vfork:
        *nr = SYS_getpid;
        break;
    case SYS_fork:
    case SYS_clone:
    case 435:
        *self = 1;
        break;
    case SYS_execve:
    case SYS_execveat:
        if (pick(2))
            a[*nr == SYS_execve ? 0 : 1] = (uint64_t)"/tmp/fuzz/nao-existe";
        else
            a[*nr == SYS_execve ? 0 : 1] = pick(2) ? kernel_ptr() : edge_ptr();
        break;
    case SYS_rt_sigaction:
        if (a[1] && pick(4)) {
            static struct sigaction act;
            memset(&act, 0, sizeof(act));
            act.sa_handler = pick(2) ? SIG_IGN : SIG_DFL;
            a[1] = (uint64_t)&act;
        }
        if (a[0] == SIGALRM)
            a[0] = SIGUSR1;
        break;
    case SYS_kill:
    case SYS_tkill:
    case SYS_tgkill:
    case SYS_rt_sigqueueinfo:
    case SYS_rt_tgsigqueueinfo: {
        static const int64_t who[] = { 0, -1, 1, 99999 };
        int64_t w = pick(3) ? me : who[pick(4)];
        a[0] = (uint64_t)w;
        int si = 1;
        if (*nr == SYS_tgkill || *nr == SYS_rt_tgsigqueueinfo) {
            a[1] = (uint64_t)(pick(2) ? me : 99999);
            si = 2;
        }
        uint64_t sig = a[si];
        if (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE || sig == SIGSYS)
            a[si] = SIGUSR1;
        else if (sig == SIGKILL && pick(2))
            a[si] = SIGUSR2;
        break;
    }
    case SYS_set_tid_address:
        a[0] = pick(2) ? 0 : kernel_ptr();
        break;
    case SYS_setrlimit:
    case SYS_prlimit64:
        if (pick(2))
            *nr = SYS_getrlimit;
        break;
    default:
        break;
    }
}

static void record(struct slot *s, uint64_t nr, const uint64_t *a)
{
    if (!s)
        return;
    struct call *c = &s->ring[s->pos % RING];
    c->nr = nr;
    for (int i = 0; i < 6; i++)
        c->a[i] = a[i];
    c->ret = 0x7fffffffffffffffll;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static void grandchild(void)
{
    for (int i = 0; i < 40; i++) {
        uint64_t nr = pick(335), a[6];
        for (int k = 0; k < 6; k++)
            a[k] = any_arg();
        int self = 0;
        sanitize(&nr, a, &self);
        if (self)
            continue;
        syscall((long)nr, a[0], a[1], a[2], a[3], a[4], a[5]);
    }
    __atomic_sub_fetch(&my_slot->grand, 1, __ATOMIC_SEQ_CST);
    _exit(0);
}

static void setup_fds(void)
{
    int p[2], sv[2];
    nfds = 0;
    if (pipe(p) == 0) {
        fds[nfds++] = p[0];
        fds[nfds++] = p[1];
    }
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
        fds[nfds++] = sv[0];
        fds[nfds++] = sv[1];
    }
    int t = socket(AF_INET, SOCK_STREAM, 0);
    if (t >= 0) {
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons((uint16_t)(20000 + getpid() % 20000));
        if (bind(t, (struct sockaddr *)&sa, sizeof(sa)) == 0)
            listen(t, 4);
        fds[nfds++] = t;
    }
    int u = socket(AF_INET, SOCK_DGRAM, 0);
    if (u >= 0)
        fds[nfds++] = u;
    int m, s;
    if (orion_openpty(&m, &s) == 0) {
        fds[nfds++] = m;
        fds[nfds++] = s;
    }
    int f = open("f0", O_RDWR | O_CREAT, 0644);
    if (f >= 0) {
        ssize_t w = write(f, "fuzz\n", 5);
        (void)w;
        fds[nfds++] = f;
    }
    int d = open(".", O_RDONLY | O_DIRECTORY);
    if (d >= 0)
        fds[nfds++] = d;
    int pr = open("/proc/self/stat", O_RDONLY);
    if (pr >= 0)
        fds[nfds++] = pr;
    int nl = open("/dev/null", O_RDWR);
    if (nl >= 0)
        fds[nfds++] = nl;
    for (int i = 0; i < nfds; i++)
        if (pick(2))
            fcntl(fds[i], F_SETFL, O_NONBLOCK);
}

static void worker(int idx, uint64_t seed)
{
    struct slot *s = &sh->slots[idx];
    my_slot = s;
    rng = seed ? seed : 1;
    setpgid(0, 0);
    int nul = open("/dev/null", O_RDWR);
    if (nul >= 0) {
        dup2(nul, 0);
        dup2(nul, 1);
        dup2(nul, 2);
        close(nul);
    }
    if (chdir(FUZZ_DIR) < 0)
        _exit(20);
    struct rlimit rl = { 64, 64 };
    setrlimit(RLIMIT_NPROC, &rl);
    if (setgroups(0, NULL) < 0 || setgid(FUZZ_UID) < 0 || setuid(FUZZ_UID) < 0)
        _exit(21);
    for (int sig = 1; sig < 65; sig++)
        if (sig != SIGKILL && sig != SIGSTOP && sig != SIGSEGV && sig != SIGBUS && sig != SIGILL &&
            sig != SIGFPE && sig != SIGSYS)
            signal(sig, SIG_IGN);
    edge = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    bigbuf = mmap(NULL, 65536 + 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    sandbox = mmap(NULL, SANDBOX_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (edge == MAP_FAILED || bigbuf == MAP_FAILED || sandbox == MAP_FAILED)
        _exit(22);
    munmap(edge + 4096, 4096);
    munmap(bigbuf + 65536, 4096);
    memset(edge, 'A', 4096);
    memset(bigbuf, 0, 65536);
    setup_fds();
    for (uint64_t i = 0; i < CALLS_PER_CHILD; i++) {
        uint64_t nr, a[6];
        uint64_t r = pick(100);
        if (r < 4)
            nr = 335 + pick(2000);
        else
            nr = pick(335);
        if (pick(200) == 0)
            nr = 1000 + pick(3);
        for (int k = 0; k < 6; k++)
            a[k] = any_arg();
        int self = 0;
        sanitize(&nr, a, &self);
        if (self) {
            if (pick(50) != 0 || __atomic_load_n(&s->grand, __ATOMIC_SEQ_CST) >= GRAND_MAX / 4)
                continue;
            __atomic_add_fetch(&s->grand, 1, __ATOMIC_SEQ_CST);
            record(s, SYS_fork, a);
            pid_t g = fork();
            if (g == 0)
                grandchild();
            if (g < 0)
                __atomic_sub_fetch(&s->grand, 1, __ATOMIC_SEQ_CST);
            s->ring[s->pos % RING].ret = g;
            s->pos++;
            s->count++;
            continue;
        }
        record(s, nr, a);
        long ret = syscall((long)nr, a[0], a[1], a[2], a[3], a[4], a[5]);
        s->ring[s->pos % RING].ret = ret < 0 ? -errno : ret;
        s->pos++;
        s->count++;
        __atomic_add_fetch(&sh->total, 1, __ATOMIC_RELAXED);
        if (nfds < 4)
            setup_fds();
    }
    _exit(0);
}

static void dump_ring(int idx, pid_t pid, const char *why)
{
    struct slot *s = &sh->slots[idx];
    printf("fuzz: filho %d (pid %d, semente %llu) %s depois de %llu chamada(s); ultimas chamadas:\n", idx, pid,
           (unsigned long long)s->seed, why, (unsigned long long)s->count);
    uint64_t n = s->pos < RING - 1 ? s->pos : RING - 1;
    for (uint64_t k = s->pos - n; k <= s->pos; k++) {
        struct call *c = &s->ring[k % RING];
        bool pending = k == s->pos;
        if (pending && c->ret != 0x7fffffffffffffffll)
            break;
        printf("fuzz:   #%llu nr %llu (%llx, %llx, %llx, %llx, %llx, %llx) = ", (unsigned long long)k,
               (unsigned long long)c->nr, (unsigned long long)c->a[0], (unsigned long long)c->a[1],
               (unsigned long long)c->a[2], (unsigned long long)c->a[3], (unsigned long long)c->a[4],
               (unsigned long long)c->a[5]);
        if (pending)
            printf("em andamento\n");
        else
            printf("%lld\n", (long long)c->ret);
    }
    fflush(stdout);
}

static long mem_free_kb(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f)
        return -1;
    char line[128];
    long v = -1;
    while (fgets(line, sizeof(line), f))
        if (sscanf(line, "MemFree: %ld kB", &v) == 1)
            break;
    fclose(f);
    return v;
}

static int proc_count(void)
{
    DIR *d = opendir("/proc");
    if (!d)
        return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)))
        if (e->d_name[0] >= '0' && e->d_name[0] <= '9')
            n++;
    closedir(d);
    return n;
}

static void list_procs(const char *when)
{
    DIR *d = opendir("/proc");
    if (!d)
        return;
    struct dirent *e;
    printf("fuzz: processos %s:", when);
    while ((e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;
        char path[64], line[160] = "";
        snprintf(path, sizeof(path), "/proc/%s/stat", e->d_name);
        FILE *f = fopen(path, "r");
        if (f) {
            if (!fgets(line, sizeof(line), f))
                line[0] = 0;
            fclose(f);
        }
        line[strcspn(line, "\n")] = 0;
        char *sp = strchr(line, ' ');
        if (sp) {
            sp = strchr(sp + 1, ')');
            if (sp && sp[1] == ' ' && sp[2])
                sp[3] = 0;
        }
        printf(" [%s]", line);
    }
    printf("\n");
    closedir(d);
}

static int kill_leftovers(void)
{
    DIR *d = opendir("/proc");
    if (!d)
        return 0;
    int n = 0;
    pid_t me = getpid();
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;
        pid_t pid = (pid_t)atoi(e->d_name);
        if (pid == me)
            continue;
        char path[64], line[160] = "";
        snprintf(path, sizeof(path), "/proc/%d/stat", pid);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        if (!fgets(line, sizeof(line), f))
            line[0] = 0;
        fclose(f);
        if (strstr(line, " (fuzz) ") && kill(pid, SIGKILL) == 0)
            n++;
    }
    closedir(d);
    return n;
}

static void remove_tree(const char *path)
{
    DIR *d = opendir(path);
    if (!d) {
        unlink(path);
        return;
    }
    struct dirent *e;
    char sub[512];
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
        struct stat st;
        if (lstat(sub, &st) == 0 && S_ISDIR(st.st_mode))
            remove_tree(sub);
        else
            unlink(sub);
    }
    closedir(d);
    rmdir(path);
}

static int unexpected(int sig)
{
    return sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE || sig == SIGSYS;
}

int main(int argc, char **argv)
{
    int secs = 60;
    uint64_t seed = 0;
    int workers = (int)sysconf(_SC_NPROCESSORS_ONLN);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seed") && i + 1 < argc)
            seed = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--workers") && i + 1 < argc)
            workers = atoi(argv[++i]);
        else
            secs = atoi(argv[i]);
    }
    if (workers < 1)
        workers = 1;
    if (workers > WORKERS_MAX)
        workers = WORKERS_MAX;
    if (secs < 1)
        secs = 1;
    if (!seed) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        seed = ((uint64_t)ts.tv_sec * 1000003u) ^ (uint64_t)ts.tv_nsec ^ ((uint64_t)getpid() << 32);
    }
    if (geteuid() != 0) {
        fprintf(stderr, "fuzz: rode como root; os filhos viram o usuario %d sozinhos\n", FUZZ_UID);
        return 2;
    }
    list_procs("antes");
    printf("fuzz: semente %llu (repita com --seed %llu), %d s, %d filho(s) por vez como uid %d em %s\n",
           (unsigned long long)seed, (unsigned long long)seed, secs, workers, FUZZ_UID, FUZZ_DIR);
    fflush(stdout);
    remove_tree(FUZZ_DIR);
    if (mkdir(FUZZ_DIR, 0777) < 0 || chown(FUZZ_DIR, FUZZ_UID, FUZZ_UID) < 0) {
        printf("fuzz: nao consegui criar %s: %s\n", FUZZ_DIR, strerror(errno));
        return 1;
    }
    if (mkdir(FUZZ_DIR "/d0", 0777) == 0 && chown(FUZZ_DIR "/d0", FUZZ_UID, FUZZ_UID) < 0)
        printf("fuzz: chown de d0 falhou\n");
    sh = mmap(NULL, sizeof(*sh), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (sh == MAP_FAILED) {
        printf("fuzz: mmap compartilhado falhou: %s\n", strerror(errno));
        return 1;
    }
    memset(sh, 0, sizeof(*sh));
    sleep(1);
    long free0 = mem_free_kb();
    int procs0 = proc_count();

    pid_t pid[WORKERS_MAX] = { 0 };
    uint64_t last_count[WORKERS_MAX] = { 0 };
    double last_progress[WORKERS_MAX] = { 0 };
    double killed_at[WORKERS_MAX] = { 0 };
    uint64_t spawned = 0, stalled = 0, signaled = 0, odd = 0, stuck = 0;
    uint64_t child_seq = 0;
    double start = now_ms(), last_beat = start, worst_gap = 0, last_print = start;
    while (1) {
        double now = now_ms();
        if (now - last_beat > worst_gap)
            worst_gap = now - last_beat;
        last_beat = now;
        bool running = now - start < secs * 1000.0;
        int alive = 0;
        for (int i = 0; i < workers; i++) {
            if (pid[i] > 0) {
                int st = 0;
                pid_t r = waitpid(pid[i], &st, WNOHANG);
                if (r == pid[i]) {
                    if (WIFSIGNALED(st)) {
                        signaled++;
                        if (unexpected(WTERMSIG(st)) && !killed_at[i]) {
                            odd++;
                            char why[64];
                            snprintf(why, sizeof(why), "morreu com o sinal %d", WTERMSIG(st));
                            dump_ring(i, pid[i], why);
                        }
                    } else if (WIFEXITED(st) && WEXITSTATUS(st) != 0) {
                        odd++;
                        char why[64];
                        snprintf(why, sizeof(why), "saiu com %d", WEXITSTATUS(st));
                        dump_ring(i, pid[i], why);
                    }
                    kill(-pid[i], SIGKILL);
                    pid[i] = 0;
                } else {
                    alive++;
                    if (killed_at[i]) {
                        if (now - killed_at[i] > KILL_GRACE_MS && killed_at[i] > 0) {
                            stuck++;
                            dump_ring(i, pid[i], "nao morreu com SIGKILL em 5 s");
                            killed_at[i] = -1;
                        }
                    } else if (sh->slots[i].count != last_count[i]) {
                        last_count[i] = sh->slots[i].count;
                        last_progress[i] = now;
                    } else if (now - last_progress[i] > STALL_MS || !running) {
                        if (now - last_progress[i] > STALL_MS)
                            stalled++;
                        kill(pid[i], SIGKILL);
                        killed_at[i] = now;
                    }
                    continue;
                }
            }
            if (!running)
                continue;
            struct slot *s = &sh->slots[i];
            memset(s, 0, sizeof(*s));
            s->seed = seed + ++child_seq;
            killed_at[i] = 0;
            last_count[i] = 0;
            last_progress[i] = now;
            pid_t c = fork();
            if (c == 0)
                worker(i, s->seed);
            if (c > 0) {
                pid[i] = c;
                spawned++;
                alive++;
            }
        }
        if (now - last_print >= 5000) {
            printf("fuzz: vivo em %.0f s: %llu chamadas, %llu filho(s), %d agora\n", (now - start) / 1000,
                   (unsigned long long)sh->total, (unsigned long long)spawned, alive);
            fflush(stdout);
            last_print = now;
        }
        if (!running && alive == 0)
            break;
        if (!running && now - start > secs * 1000.0 + 30000)
            break;
        struct timespec ts = { 0, 100 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
    int leftovers = kill_leftovers();
    remove_tree(FUZZ_DIR);
    long free1 = 0;
    int procs1 = 0;
    for (int k = 0; k < 50; k++) {
        while (waitpid(-1, NULL, WNOHANG) > 0)
            ;
        free1 = mem_free_kb();
        procs1 = proc_count();
        if (free1 >= free0 - 4096 && procs1 <= procs0)
            break;
        struct timespec ts = { 0, 200 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    if (procs1 > procs0)
        list_procs("depois");
    printf("fuzz: %d neto(s) que sobraram mortos no fim\n", leftovers);
    bool gap_ok = worst_gap < 5000;
    bool res_ok = free1 >= free0 - 4096 && procs1 <= procs0;
    printf("fuzz: %llu chamadas em %llu filho(s), %llu morto(s) por sinal, %llu travado(s) e mortos, %llu morte(s) "
           "inesperada(s), %llu que nao morreram\n",
           (unsigned long long)sh->total, (unsigned long long)spawned, (unsigned long long)signaled,
           (unsigned long long)stalled, (unsigned long long)odd, (unsigned long long)stuck);
    printf("fuzz: maior intervalo sem sinal de vida do pai: %.0f ms  %s\n", worst_gap, gap_ok ? "ok" : "FALHOU");
    printf("fuzz: memoria livre antes %ld kB, depois %ld kB; processos antes %d, depois %d  %s\n", free0, free1,
           procs0, procs1, res_ok ? "ok" : "FALHOU");
    printf("fuzz: fim (semente %llu)\n", (unsigned long long)seed);
    return gap_ok && res_ok && !stuck ? 0 : 1;
}
