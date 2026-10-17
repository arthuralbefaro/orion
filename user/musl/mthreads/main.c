#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define THREADS 8
#define ROUNDS  50000

static long atomic_counter;
static long locked_counter;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static __thread int my_slot = -1;
static long tids[THREADS];
static int slots_ok[THREADS];
static volatile int usr1_hits;
static int fails;

static void check(const char *what, int ok)
{
    printf("  %-48s %s\n", what, ok ? "ok" : "FALHA");
    if (!ok)
        fails++;
}

static void *worker(void *arg)
{
    int id = (int)(long)arg;
    my_slot = id;
    tids[id] = syscall(SYS_gettid);
    for (int i = 0; i < ROUNDS; i++) {
        __atomic_fetch_add(&atomic_counter, 1, __ATOMIC_SEQ_CST);
        if (i % 10 == 0) {
            pthread_mutex_lock(&lock);
            locked_counter++;
            pthread_mutex_unlock(&lock);
        }
        if (i % 5000 == 0)
            sched_yield();
    }
    slots_ok[id] = my_slot == id;
    return (void *)(long)(id * 10);
}

static void *exiting_thread(void *arg)
{
    (void)arg;
    usleep(100000);
    exit(5);
    return NULL;
}

static void *quitter(void *arg)
{
    (void)arg;
    pthread_exit((void *)42);
    return NULL;
}

static int block_pipe[2];

static void *blocked_reader(void *arg)
{
    (void)arg;
    char c;
    read(block_pipe[0], &c, 1);
    return NULL;
}

static void *exec_false(void *arg)
{
    (void)arg;
    char *argv[] = { "false", NULL };
    execv("/bin/false", argv);
    return NULL;
}

static int child_status(pid_t pid)
{
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 100 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
}

static void on_usr1(int sig)
{
    (void)sig;
    usr1_hits++;
}

int main(void)
{
    printf("mthreads: threads da musl no Orion\n");
    pthread_t th[THREADS];
    int created = 0;
    for (int i = 0; i < THREADS; i++)
        if (pthread_create(&th[i], NULL, worker, (void *)(long)i) == 0)
            created++;
    check("pthread_create de 8 threads", created == THREADS);
    int joined_ok = 1;
    for (int i = 0; i < created; i++) {
        void *ret = NULL;
        if (pthread_join(th[i], &ret) != 0 || (long)ret != i * 10)
            joined_ok = 0;
    }
    check("pthread_join devolve o valor de cada thread", joined_ok);
    printf("  contador atomico: %ld (esperado %d)\n", atomic_counter, THREADS * ROUNDS);
    check("8 threads somando num contador atomico", atomic_counter == (long)THREADS * ROUNDS);
    check("contador protegido por pthread_mutex", locked_counter == (long)THREADS * (ROUNDS / 10));
    int slots = 1, distinct = 1;
    for (int i = 0; i < THREADS; i++) {
        if (!slots_ok[i])
            slots = 0;
        if (tids[i] == getpid())
            distinct = 0;
        for (int j = 0; j < i; j++)
            if (tids[i] == tids[j])
                distinct = 0;
    }
    check("__thread separado por thread", slots && my_slot == -1);
    check("gettid diferente em cada thread e do pid", distinct);

    pthread_t q;
    void *qret = NULL;
    pthread_create(&q, NULL, quitter, NULL);
    pthread_join(q, &qret);
    check("pthread_exit numa thread nao encerra o processo", (long)qret == 42);

    signal(SIGUSR1, on_usr1);
    kill(getpid(), SIGUSR1);
    check("sinal para o processo chega a uma thread", usr1_hits == 1);

    pid_t pid = fork();
    if (pid == 0) {
        pthread_t e;
        pthread_create(&e, NULL, exiting_thread, NULL);
        for (;;)
            pause();
    }
    int st = 0;
    waitpid(pid, &st, 0);
    check("exit numa thread encerra o processo inteiro", WIFEXITED(st) && WEXITSTATUS(st) == 5);

    pthread_t bg;
    pthread_create(&bg, NULL, quitter, NULL);
    pid = fork();
    if (pid == 0)
        _exit(syscall(SYS_gettid) == getpid() ? 0 : 1);
    st = 0;
    waitpid(pid, &st, 0);
    pthread_join(bg, NULL);
    check("fork de processo com threads gera filho com uma thread", WIFEXITED(st) && WEXITSTATUS(st) == 0);

    pipe(block_pipe);
    pid = fork();
    if (pid == 0) {
        pthread_t r;
        pthread_create(&r, NULL, blocked_reader, NULL);
        usleep(50000);
        exit(7);
    }
    check("exit com outra thread presa num read", child_status(pid) == 7);

    pid = fork();
    if (pid == 0) {
        pthread_t r;
        pthread_create(&r, NULL, blocked_reader, NULL);
        usleep(50000);
        char *argv[] = { "true", NULL };
        execv("/bin/true", argv);
        _exit(9);
    }
    check("execve com outra thread presa num read", child_status(pid) == 0);

    pid = fork();
    if (pid == 0) {
        pthread_t e;
        pthread_create(&e, NULL, exec_false, NULL);
        for (;;)
            pause();
    }
    check("execve feito por uma thread que nao e a principal", child_status(pid) == 1);

    printf("mthreads: fim, %s\n", fails ? "FALHOU" : "TUDO OK");
    return fails ? 1 : 0;
}
