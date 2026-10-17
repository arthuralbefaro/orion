#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define AT_NULL     0
#define AT_PHDR     3
#define AT_PHENT    4
#define AT_PHNUM    5
#define AT_PAGESZ   6
#define AT_ENTRY    9
#define AT_UID      11
#define AT_PLATFORM 15
#define AT_HWCAP    16
#define AT_RANDOM   25
#define AT_EXECFN   31
#define PT_LOAD     1

struct phdr {
    unsigned int type;
    unsigned int flags;
    unsigned long offset;
    unsigned long vaddr;
    unsigned long paddr;
    unsigned long filesz;
    unsigned long memsz;
    unsigned long align;
};

extern void _start(void);

static int fails;
static unsigned long xmm_at_start[32];
static unsigned int mxcsr_at_start;

static void check(const char *what, int ok)
{
    printf("  %-46s %s\n", what, ok ? "ok" : "FALHA");
    if (!ok)
        fails++;
}

static unsigned long *find_auxv(void)
{
    char **e = environ;
    while (*e)
        e++;
    return (unsigned long *)(e + 1);
}

static unsigned long aux(unsigned long *v, unsigned long type, int *found)
{
    for (; v[0] != AT_NULL; v += 2)
        if (v[0] == type) {
            *found = 1;
            return v[1];
        }
    *found = 0;
    return 0;
}

static void hex16(const unsigned char *r, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[2 * i] = digits[r[i] >> 4];
        out[2 * i + 1] = digits[r[i] & 15];
    }
    out[32] = '\0';
}

static void capture_fpu(void)
{
    __asm__ volatile("movdqu %%xmm0, 0(%0)\n\tmovdqu %%xmm1, 16(%0)\n\t"
                     "movdqu %%xmm2, 32(%0)\n\tmovdqu %%xmm3, 48(%0)\n\t"
                     "movdqu %%xmm4, 64(%0)\n\tmovdqu %%xmm5, 80(%0)\n\t"
                     "movdqu %%xmm6, 96(%0)\n\tmovdqu %%xmm7, 112(%0)\n\t"
                     "movdqu %%xmm8, 128(%0)\n\tmovdqu %%xmm9, 144(%0)\n\t"
                     "movdqu %%xmm10, 160(%0)\n\tmovdqu %%xmm11, 176(%0)\n\t"
                     "movdqu %%xmm12, 192(%0)\n\tmovdqu %%xmm13, 208(%0)\n\t"
                     "movdqu %%xmm14, 224(%0)\n\tmovdqu %%xmm15, 240(%0)\n\t"
                     "stmxcsr %1"
                     : : "r"(xmm_at_start), "m"(mxcsr_at_start) : "memory");
}

int main(int argc, char **argv)
{
    capture_fpu();
    unsigned long *v = find_auxv();
    int f;
    unsigned char *rnd = (unsigned char *)aux(v, AT_RANDOM, &f);
    if (argc > 1 && strcmp(argv[1], "-r") == 0) {
        char hex[33];
        hex16(rnd, hex);
        printf("%s\n", hex);
        return 0;
    }

    printf("auxtest: auxv da pilha inicial\n");
    int zero = 1;
    for (int i = 0; i < 32; i++)
        if (xmm_at_start[i])
            zero = 0;
    check("xmm0..xmm15 zerados na entrada", zero);
    check("mxcsr = 0x1f80 na entrada", mxcsr_at_start == 0x1F80);

    unsigned long phdr = aux(v, AT_PHDR, &f);
    check("AT_PHDR presente", f && phdr != 0);
    unsigned long phent = aux(v, AT_PHENT, &f);
    check("AT_PHENT = 56", f && phent == 56);
    unsigned long phnum = aux(v, AT_PHNUM, &f);
    check("AT_PHNUM >= 2", f && phnum >= 2);
    int has_main = 0;
    if (phdr && phent == 56) {
        const struct phdr *ph = (const struct phdr *)phdr;
        for (unsigned long i = 0; i < phnum; i++)
            if (ph[i].type == PT_LOAD && (unsigned long)&main >= ph[i].vaddr &&
                (unsigned long)&main < ph[i].vaddr + ph[i].memsz)
                has_main = 1;
    }
    check("AT_PHDR aponta para os cabecalhos de verdade", has_main);
    check("AT_PAGESZ = 4096", aux(v, AT_PAGESZ, &f) == 4096 && f);
    check("AT_ENTRY = _start", aux(v, AT_ENTRY, &f) == (unsigned long)&_start && f);
    check("AT_UID = 0", aux(v, AT_UID, &f) == 0 && f);
    unsigned long hw = aux(v, AT_HWCAP, &f);
    check("AT_HWCAP tem SSE e SSE2", f && (hw & (1ul << 25)) && (hw & (1ul << 26)));
    const char *plat = (const char *)aux(v, AT_PLATFORM, &f);
    check("AT_PLATFORM = x86_64", f && plat && strcmp(plat, "x86_64") == 0);
    const char *fn = (const char *)aux(v, AT_EXECFN, &f);
    check("AT_EXECFN = /bin/auxtest", f && fn && strcmp(fn, "/bin/auxtest") == 0);
    int nonzero = 0;
    for (int i = 0; rnd && i < 16; i++)
        if (rnd[i])
            nonzero = 1;
    check("AT_RANDOM aponta para 16 bytes", rnd != NULL && nonzero);
    check("AT_RANDOM alinhado em 16", rnd && ((unsigned long)rnd & 15) == 0);
    check("pilha alinhada em 16 com argc no topo", ((unsigned long)(argv - 1) & 15) == 0);

    int fds[2];
    char mine[33], other[40];
    memset(other, 0, sizeof(other));
    hex16(rnd, mine);
    if (pipe(fds) == 0) {
        pid_t pid = fork();
        if (pid == 0) {
            dup2(fds[1], 1);
            close(fds[0]);
            close(fds[1]);
            char *args[] = { "/bin/auxtest", "-r", NULL };
            execv("/bin/auxtest", args);
            _exit(1);
        }
        close(fds[1]);
        long n = 0, r;
        while (n < 32 && (r = read(fds[0], other + n, (size_t)(32 - n))) > 0)
            n += r;
        close(fds[0]);
        waitpid(pid, NULL, 0);
        check("AT_RANDOM muda a cada exec", n == 32 && memcmp(mine, other, 32) != 0);
    }

    printf("auxtest: fim, %s\n", fails ? "FALHOU" : "TUDO OK");
    return fails ? 1 : 0;
}
