#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#define WORKERS 4
#define ROUNDS  300

static int fails;

static void check(const char *what, int ok)
{
    printf("  %-46s %s\n", what, ok ? "ok" : "FALHA");
    if (!ok)
        fails++;
}

static void load_xmm(unsigned long v)
{
    unsigned long buf[2] = { v, ~v };
    __asm__ volatile("movdqu (%0), %%xmm0\n\tmovdqu (%0), %%xmm1\n\t"
                     "movdqu (%0), %%xmm2\n\tmovdqu (%0), %%xmm3\n\t"
                     "movdqu (%0), %%xmm4\n\tmovdqu (%0), %%xmm5\n\t"
                     "movdqu (%0), %%xmm6\n\tmovdqu (%0), %%xmm7\n\t"
                     "movdqu (%0), %%xmm8\n\tmovdqu (%0), %%xmm9\n\t"
                     "movdqu (%0), %%xmm10\n\tmovdqu (%0), %%xmm11\n\t"
                     "movdqu (%0), %%xmm12\n\tmovdqu (%0), %%xmm13\n\t"
                     "movdqu (%0), %%xmm14\n\tmovdqu (%0), %%xmm15"
                     : : "r"(buf) : "memory");
}

static int xmm_is(unsigned long v)
{
    unsigned long out[32];
    __asm__ volatile("movdqu %%xmm0, 0(%0)\n\tmovdqu %%xmm1, 16(%0)\n\t"
                     "movdqu %%xmm2, 32(%0)\n\tmovdqu %%xmm3, 48(%0)\n\t"
                     "movdqu %%xmm4, 64(%0)\n\tmovdqu %%xmm5, 80(%0)\n\t"
                     "movdqu %%xmm6, 96(%0)\n\tmovdqu %%xmm7, 112(%0)\n\t"
                     "movdqu %%xmm8, 128(%0)\n\tmovdqu %%xmm9, 144(%0)\n\t"
                     "movdqu %%xmm10, 160(%0)\n\tmovdqu %%xmm11, 176(%0)\n\t"
                     "movdqu %%xmm12, 192(%0)\n\tmovdqu %%xmm13, 208(%0)\n\t"
                     "movdqu %%xmm14, 224(%0)\n\tmovdqu %%xmm15, 240(%0)"
                     : : "r"(out) : "memory");
    for (int i = 0; i < 16; i++)
        if (out[2 * i] != v || out[2 * i + 1] != ~v)
            return 0;
    return 1;
}

static void set_mxcsr(unsigned int v)
{
    __asm__ volatile("ldmxcsr %0" : : "m"(v));
}

static unsigned int get_mxcsr(void)
{
    unsigned int v;
    __asm__ volatile("stmxcsr %0" : "=m"(v));
    return v;
}

static int worker(int id)
{
    unsigned long pattern = 0x1111111111111111ul * (unsigned long)(id + 1);
    unsigned int mx = 0x1F80 | ((unsigned int)(id & 3) << 13);
    for (int r = 0; r < ROUNDS; r++) {
        load_xmm(pattern + (unsigned long)r);
        set_mxcsr(mx);
        if (r % 3 == 0)
            sched_yield();
        else
            for (volatile int k = 0; k < 2000; k++)
                ;
        if (!xmm_is(pattern + (unsigned long)r) || get_mxcsr() != mx)
            return 1;
    }
    return 0;
}

static void clobber(int sig)
{
    (void)sig;
    load_xmm(0xdeaddeaddeaddeadul);
    set_mxcsr(0x7F80);
}

int main(void)
{
    printf("fputest: estado de SSE por thread\n");

    pid_t kids[WORKERS];
    for (int i = 0; i < WORKERS; i++) {
        kids[i] = fork();
        if (kids[i] == 0)
            _exit(worker(i));
    }
    int bad = 0;
    for (int i = 0; i < WORKERS; i++) {
        int st = 0;
        waitpid(kids[i], &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
            bad++;
    }
    check("4 processos com xmm0..15 e mxcsr proprios", bad == 0);

    signal(SIGTERM, clobber);
    load_xmm(0x0123456789abcdeful);
    set_mxcsr(0x1F80);
    raise(SIGTERM);
    check("handler de sinal nao estraga os xmm", xmm_is(0x0123456789abcdeful));
    check("handler de sinal nao estraga o mxcsr", get_mxcsr() == 0x1F80);

    load_xmm(0x5555aaaa5555aaaaul);
    pid_t pid = fork();
    if (pid == 0)
        _exit(xmm_is(0x5555aaaa5555aaaaul) ? 0 : 1);
    int st = 0;
    waitpid(pid, &st, 0);
    check("filho do fork herda os xmm do pai", WIFEXITED(st) && WEXITSTATUS(st) == 0);
    check("pai continua com os seus xmm", xmm_is(0x5555aaaa5555aaaaul));

    printf("fputest: fim, %s\n", fails ? "FALHOU" : "TUDO OK");
    return fails ? 1 : 0;
}
