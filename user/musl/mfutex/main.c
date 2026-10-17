#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define THREADS    8
#define MUTEX_OPS  125000
#define QUEUE_CAP  4
#define PRODUCERS  4
#define ITEMS      25000
#define ROUNDS     2000
#define SHARED_OPS 100000

static int fails;

static void check(const char *what, int ok)
{
    printf("  %-52s %s\n", what, ok ? "ok" : "FALHA");
    if (!ok)
        fails++;
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static long futex(int *addr, int op, int val, const struct timespec *to, int *addr2, int val3)
{
    long r = syscall(SYS_futex, addr, op, val, to, addr2, val3);
    return r < 0 ? -errno : r;
}

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static long mutex_count;

static void *mutex_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < MUTEX_OPS; i++) {
        pthread_mutex_lock(&mtx);
        mutex_count++;
        pthread_mutex_unlock(&mtx);
    }
    return NULL;
}

static pthread_mutex_t qlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t not_full = PTHREAD_COND_INITIALIZER;
static pthread_cond_t not_empty = PTHREAD_COND_INITIALIZER;
static int queue[QUEUE_CAP];
static int qhead, qcount, producers_left;
static long consumed_sum, consumed_items;

static void *producer(void *arg)
{
    int base = (int)(long)arg * ITEMS;
    for (int i = 1; i <= ITEMS; i++) {
        pthread_mutex_lock(&qlock);
        while (qcount == QUEUE_CAP)
            pthread_cond_wait(&not_full, &qlock);
        queue[(qhead + qcount) % QUEUE_CAP] = base + i;
        qcount++;
        pthread_cond_signal(&not_empty);
        pthread_mutex_unlock(&qlock);
    }
    pthread_mutex_lock(&qlock);
    producers_left--;
    pthread_cond_broadcast(&not_empty);
    pthread_mutex_unlock(&qlock);
    return NULL;
}

static void *consumer(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&qlock);
        while (qcount == 0 && producers_left > 0)
            pthread_cond_wait(&not_empty, &qlock);
        if (qcount == 0) {
            pthread_mutex_unlock(&qlock);
            return NULL;
        }
        int v = queue[qhead];
        qhead = (qhead + 1) % QUEUE_CAP;
        qcount--;
        consumed_sum += v;
        consumed_items++;
        pthread_cond_signal(&not_full);
        pthread_mutex_unlock(&qlock);
    }
}

static pthread_barrier_t barrier;
static int round_marks[THREADS];
static int barrier_bad;

static void *barrier_worker(void *arg)
{
    int id = (int)(long)arg;
    for (int r = 0; r < ROUNDS; r++) {
        round_marks[id] = r;
        pthread_barrier_wait(&barrier);
        for (int j = 0; j < THREADS; j++)
            if (round_marks[j] != r)
                __atomic_store_n(&barrier_bad, 1, __ATOMIC_SEQ_CST);
        pthread_barrier_wait(&barrier);
    }
    return NULL;
}

static sem_t sem_items;
static long sem_taken;

static void *sem_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < 10000; i++) {
        sem_wait(&sem_items);
        __atomic_fetch_add(&sem_taken, 1, __ATOMIC_SEQ_CST);
    }
    return NULL;
}

static const struct timespec ten_seconds = { 10, 0 };
static int wait_word;
static volatile long wait_result = 1;

static void *futex_waiter(void *arg)
{
    (void)arg;
    wait_result = futex(&wait_word, FUTEX_WAIT_PRIVATE, 0, &ten_seconds, NULL, 0);
    return NULL;
}

static const struct timespec huge_timeout = { 0x7fffffffffffffffL, 999999999 };

static void *futex_waiter_huge(void *arg)
{
    (void)arg;
    wait_result = futex(&wait_word, FUTEX_WAIT_PRIVATE, 0, &huge_timeout, NULL, 0);
    return NULL;
}

static long queued(int *addr)
{
    for (;;) {
        int v = __atomic_load_n(addr, __ATOMIC_SEQ_CST);
        long r = futex(addr, FUTEX_CMP_REQUEUE_PRIVATE, 0, (const struct timespec *)(uintptr_t)0x7fffffff, addr, v);
        if (r != -EAGAIN)
            return r;
    }
}

static int wait_queued(int *addr, long n)
{
    for (int i = 0; i < 5000; i++) {
        if (queued(addr) == n)
            return 1;
        usleep(1000);
    }
    return 0;
}

static volatile int usr1_seen;

static void on_usr1(int sig)
{
    (void)sig;
    usr1_seen = 1;
}

static int requeue_a, requeue_b;
static volatile int requeue_done;

static void *requeue_waiter(void *arg)
{
    (void)arg;
    long r = futex(&requeue_a, FUTEX_WAIT_PRIVATE, 0, &ten_seconds, NULL, 0);
    if (r == 0)
        __atomic_fetch_add(&requeue_done, 1, __ATOMIC_SEQ_CST);
    return NULL;
}

int main(void)
{
    printf("mfutex: futex com mutex, condvar e barreira da musl\n");

    long t0 = now_ms();
    pthread_t th[THREADS];
    for (int i = 0; i < THREADS; i++)
        pthread_create(&th[i], NULL, mutex_worker, NULL);
    for (int i = 0; i < THREADS; i++)
        pthread_join(th[i], NULL);
    long t1 = now_ms();
    printf("  mutex: %ld operacoes em %ld ms\n", mutex_count, t1 - t0);
    check("pthread_mutex entre 8 threads, 1 milhao de operacoes", mutex_count == (long)THREADS * MUTEX_OPS);

    producers_left = PRODUCERS;
    pthread_t prod[PRODUCERS], cons[PRODUCERS];
    for (int i = 0; i < PRODUCERS; i++) {
        pthread_create(&prod[i], NULL, producer, (void *)(long)i);
        pthread_create(&cons[i], NULL, consumer, NULL);
    }
    for (int i = 0; i < PRODUCERS; i++) {
        pthread_join(prod[i], NULL);
        pthread_join(cons[i], NULL);
    }
    long expected_sum = 0;
    for (long i = 1; i <= (long)PRODUCERS * ITEMS; i++)
        expected_sum += i;
    printf("  condvar: %ld itens em %ld ms\n", consumed_items, now_ms() - t1);
    check("condvar produtor/consumidor, 4+4 threads, fila de 4", consumed_items == (long)PRODUCERS * ITEMS &&
                                                                  consumed_sum == expected_sum);

    pthread_barrier_init(&barrier, NULL, THREADS);
    for (int i = 0; i < THREADS; i++)
        pthread_create(&th[i], NULL, barrier_worker, (void *)(long)i);
    for (int i = 0; i < THREADS; i++)
        pthread_join(th[i], NULL);
    pthread_barrier_destroy(&barrier);
    check("pthread_barrier, 8 threads, 2000 rodadas", !barrier_bad);

    sem_init(&sem_items, 0, 0);
    pthread_t sw[4];
    for (int i = 0; i < 4; i++)
        pthread_create(&sw[i], NULL, sem_worker, NULL);
    for (int i = 0; i < 40000; i++)
        sem_post(&sem_items);
    for (int i = 0; i < 4; i++)
        pthread_join(sw[i], NULL);
    check("sem_wait/sem_post entre threads", sem_taken == 40000);

    pthread_mutex_t m2 = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t c2 = PTHREAD_COND_INITIALIZER;
    struct timespec abs;
    clock_gettime(CLOCK_REALTIME, &abs);
    abs.tv_nsec += 50 * 1000000;
    if (abs.tv_nsec >= 1000000000) {
        abs.tv_sec++;
        abs.tv_nsec -= 1000000000;
    }
    pthread_mutex_lock(&m2);
    long ts = now_ms();
    int tr = pthread_cond_timedwait(&c2, &m2, &abs);
    long waited = now_ms() - ts;
    pthread_mutex_unlock(&m2);
    printf("  timedwait: %ld ms\n", waited);
    check("pthread_cond_timedwait expira com ETIMEDOUT", tr == ETIMEDOUT && waited >= 40 && waited < 1000);

    int word = 1;
    check("FUTEX_WAIT com valor diferente devolve EAGAIN", futex(&word, FUTEX_WAIT_PRIVATE, 0, NULL, NULL, 0) == -EAGAIN);
    struct timespec rel = { 0, 20 * 1000000 };
    ts = now_ms();
    long r = futex(&word, FUTEX_WAIT_PRIVATE, 1, &rel, NULL, 0);
    waited = now_ms() - ts;
    check("FUTEX_WAIT com timeout devolve ETIMEDOUT", r == -ETIMEDOUT && waited >= 15 && waited < 1000);
    check("FUTEX_WAKE sem ninguem esperando devolve 0", futex(&word, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0) == 0);
    check("FUTEX_WAIT em endereco desalinhado devolve EINVAL",
          futex((int *)((char *)&word + 1), FUTEX_WAIT_PRIVATE, 1, NULL, NULL, 0) == -EINVAL);

    pthread_t fw;
    pthread_create(&fw, NULL, futex_waiter, NULL);
    int in_wait = wait_queued(&wait_word, 1);
    wait_word = 1;
    long woke = futex(&wait_word, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
    pthread_join(fw, NULL);
    check("FUTEX_WAKE acorda quem esta em FUTEX_WAIT", in_wait && woke == 1 && wait_result == 0);

    wait_word = 0;
    wait_result = 1;
    pthread_create(&fw, NULL, futex_waiter_huge, NULL);
    in_wait = wait_queued(&wait_word, 1);
    usleep(30000);
    int still_waiting = queued(&wait_word) == 1;
    wait_word = 1;
    futex(&wait_word, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
    pthread_join(fw, NULL);
    check("FUTEX_WAIT com timeout enorme continua esperando", in_wait && still_waiting && wait_result == 0);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_usr1;
    sigaction(SIGUSR1, &sa, NULL);
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR1);
    wait_word = 0;
    wait_result = 1;
    pthread_create(&fw, NULL, futex_waiter, NULL);
    pthread_sigmask(SIG_BLOCK, &block, &old);
    in_wait = wait_queued(&wait_word, 1);
    kill(getpid(), SIGUSR1);
    pthread_join(fw, NULL);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    check("sinal interrompe FUTEX_WAIT com EINTR", in_wait && wait_result == -EINTR && usr1_seen);

    pthread_t rq[4];
    for (int i = 0; i < 4; i++)
        pthread_create(&rq[i], NULL, requeue_waiter, NULL);
    int all_in = wait_queued(&requeue_a, 4);
    long rr = futex(&requeue_a, FUTEX_CMP_REQUEUE_PRIVATE, 1, (const struct timespec *)(uintptr_t)3, &requeue_b, 0);
    for (int i = 0; i < 5000 && __atomic_load_n(&requeue_done, __ATOMIC_SEQ_CST) < 1; i++)
        usleep(1000);
    int after_requeue = __atomic_load_n(&requeue_done, __ATOMIC_SEQ_CST);
    long moved_in_b = queued(&requeue_b);
    long w2 = futex(&requeue_b, FUTEX_WAKE_PRIVATE, 10, NULL, NULL, 0);
    for (int i = 0; i < 4; i++)
        pthread_join(rq[i], NULL);
    check("FUTEX_CMP_REQUEUE acorda 1 e move 3", all_in && rr == 4 && after_requeue == 1 && moved_in_b == 3 &&
                                                   w2 == 3 && requeue_done == 4);
    check("FUTEX_CMP_REQUEUE com valor diferente devolve EAGAIN",
          futex(&requeue_a, FUTEX_CMP_REQUEUE_PRIVATE, 1, (const struct timespec *)(uintptr_t)1, &requeue_b, 7) == -EAGAIN);

    struct shared {
        pthread_mutex_t m;
        long count;
    } *sh = mmap(NULL, sizeof(*sh), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&sh->m, &ma);
    sh->count = 0;
    pid_t pid = fork();
    for (int i = 0; i < SHARED_OPS; i++) {
        pthread_mutex_lock(&sh->m);
        long v = sh->count;
        if ((i & 255) == 0)
            sched_yield();
        sh->count = v + 1;
        pthread_mutex_unlock(&sh->m);
    }
    if (pid == 0)
        _exit(0);
    int st = 0;
    waitpid(pid, &st, 0);
    printf("  mutex entre processos: %ld\n", sh->count);
    check("mutex compartilhado entre dois processos (MAP_SHARED)", sh->count == 2L * SHARED_OPS);

    pthread_t bg;
    wait_word = 0;
    wait_result = 1;
    pthread_create(&bg, NULL, futex_waiter, NULL);
    in_wait = wait_queued(&wait_word, 1);
    pid = fork();
    if (pid == 0)
        _exit(queued(&wait_word) == 0 ? 0 : 1);
    waitpid(pid, &st, 0);
    int still = queued(&wait_word) == 1;
    wait_word = 1;
    futex(&wait_word, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
    pthread_join(bg, NULL);
    check("fork com uma thread em FUTEX_WAIT", in_wait && still && WIFEXITED(st) && WEXITSTATUS(st) == 0 &&
                                               wait_result == 0);

    printf("mfutex: fim, %s\n", fails ? "FALHOU" : "TUDO OK");
    return fails ? 1 : 0;
}
