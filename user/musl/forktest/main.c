#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define BIG_PAGES 64
#define BIG_SIZE  (BIG_PAGES * 4096)

static unsigned char big[BIG_SIZE];
static char kbuf[4096] __attribute__((aligned(4096)));
static int fails;

static void check(const char *what, int ok)
{
    printf("  %-40s %s\n", what, ok ? "ok" : "FALHA");
    if (!ok)
        fails++;
}

static void fill(unsigned char *p, size_t n, unsigned char seed)
{
    for (size_t i = 0; i < n; i++)
        p[i] = (unsigned char)(seed + (i & 0x3F));
}

static int matches(const unsigned char *p, size_t n, unsigned char seed)
{
    for (size_t i = 0; i < n; i++)
        if (p[i] != (unsigned char)(seed + (i & 0x3F)))
            return 0;
    return 1;
}

static int wait_status(pid_t pid)
{
    int st = -1;
    if (waitpid(pid, &st, 0) != pid)
        return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int sleep_mode(int seconds)
{
    fill(big, BIG_SIZE, 0x11);
    pid_t kids[8];
    int n = 0;
    for (int i = 0; i < 8; i++) {
        kids[i] = fork();
        if (kids[i] < 0)
            break;
        if (kids[i] == 0) {
            sleep((unsigned)seconds);
            _exit(matches(big, BIG_SIZE, 0x11) ? 0 : 1);
        }
        n++;
    }
    printf("forktest: %d filho(s) dormindo %d s, cada um com %d KiB compartilhados\n",
           n, seconds, BIG_SIZE / 1024);
    fflush(stdout);
    int ok = n == 8;
    for (int i = 0; i < n; i++)
        if (wait_status(kids[i]) != 0)
            ok = 0;
    printf("forktest: filhos so de leitura terminaram %s\n", ok ? "ok" : "com FALHA");
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    printf("\n");
    fflush(stdout);

    if (argc > 1 && strcmp(argv[1], "-s") == 0)
        return sleep_mode(argc > 2 ? atoi(argv[2]) : 8);

    fill(big, BIG_SIZE, 0x11);
    pid_t parent = getpid();

    pid_t pid = fork();
    if (pid == 0) {
        int ok = matches(big, BIG_SIZE, 0x11) && getppid() == parent;
        fill(big, BIG_SIZE, 0x77);
        _exit(ok && matches(big, BIG_SIZE, 0x77) ? 0 : 1);
    }
    check("filho enxerga a memoria do pai", wait_status(pid) == 0);
    check("escrita do filho nao muda o pai", matches(big, BIG_SIZE, 0x11));

    char stack_buf[4096];
    fill((unsigned char *)stack_buf, sizeof(stack_buf), 0x22);
    pid = fork();
    if (pid == 0) {
        fill((unsigned char *)stack_buf, sizeof(stack_buf), 0x33);
        _exit(matches((unsigned char *)stack_buf, sizeof(stack_buf), 0x33) ? 0 : 1);
    }
    check("pilha do filho e independente", wait_status(pid) == 0);
    check("pilha do pai continua igual",
          matches((unsigned char *)stack_buf, sizeof(stack_buf), 0x22));

    unsigned char *heap = malloc(128 * 1024);
    check("malloc de 128 KiB", heap != NULL);
    if (heap) {
        fill(heap, 128 * 1024, 0x44);
        pid = fork();
        if (pid == 0) {
            fill(heap, 128 * 1024, 0x55);
            _exit(matches(heap, 128 * 1024, 0x55) ? 0 : 1);
        }
        check("heap do filho e independente", wait_status(pid) == 0);
        check("heap do pai continua igual", matches(heap, 128 * 1024, 0x44));
    }

    long brk0 = syscall(SYS_brk, 0);
    long brk1 = syscall(SYS_brk, brk0 + 64 * 1024);
    if (brk1 == brk0 + 64 * 1024) {
        unsigned char *b = (unsigned char *)brk0;
        fill(b, 64 * 1024, 0x66);
        pid = fork();
        if (pid == 0) {
            int ok = syscall(SYS_brk, 0) == brk1 && matches(b, 64 * 1024, 0x66);
            fill(b, 64 * 1024, 0x77);
            _exit(ok && matches(b, 64 * 1024, 0x77) ? 0 : 1);
        }
        check("brk do filho herda o break e o conteudo", wait_status(pid) == 0);
        check("brk do pai continua igual", matches(b, 64 * 1024, 0x66));
        syscall(SYS_brk, brk0);
    } else {
        check("brk cresce 64 KiB", 0);
    }

    memset(kbuf, 'A', sizeof(kbuf));
    pid = fork();
    if (pid == 0) {
        if (!getcwd(kbuf, sizeof(kbuf)))
            _exit(1);
        _exit(kbuf[0] == '/' ? 0 : 1);
    }
    check("kernel escreve em pagina COW do filho", wait_status(pid) == 0);
    check("buffer do pai intacto depois disso",
          kbuf[0] == 'A' && kbuf[sizeof(kbuf) - 1] == 'A');

    pid = fork();
    if (pid == 0) {
        pid_t sub = fork();
        if (sub == 0) {
            fill(big, BIG_SIZE, 0x99);
            _exit(matches(big, BIG_SIZE, 0x99) ? 0 : 1);
        }
        int r = wait_status(sub);
        _exit(r == 0 && matches(big, BIG_SIZE, 0x11) ? 0 : 1);
    }
    check("fork dentro de fork isola os tres", wait_status(pid) == 0);
    check("pai intacto depois do neto", matches(big, BIG_SIZE, 0x11));

    pid = fork();
    if (pid == 0) {
        char *const argv[] = { "/bin/hello", "forkexec", NULL };
        execv(argv[0], argv);
        _exit(127);
    }
    check("fork + execv roda outro binario", wait_status(pid) == 42);

    int loop_ok = 1;
    for (int i = 0; i < 40; i++) {
        pid = fork();
        if (pid < 0) {
            loop_ok = 0;
            break;
        }
        if (pid == 0) {
            big[(i * 4096) % BIG_SIZE] = (unsigned char)i;
            _exit(i & 0x7F);
        }
        if (wait_status(pid) != (i & 0x7F))
            loop_ok = 0;
    }
    check("40 forks seguidos com escrita", loop_ok);
    check("pai intacto depois dos 40", matches(big, BIG_SIZE, 0x11));

    pid_t many[8];
    int spawned = 0;
    for (int i = 0; i < 8; i++) {
        many[i] = fork();
        if (many[i] < 0)
            break;
        if (many[i] == 0) {
            while (matches(big, BIG_SIZE, 0x11))
                _exit(0);
            _exit(1);
        }
        spawned++;
    }
    int all_ok = spawned == 8;
    for (int i = 0; i < spawned; i++)
        if (wait_status(many[i]) != 0)
            all_ok = 0;
    check("8 filhos so de leitura ao mesmo tempo", all_ok);

    printf("forktest: %s\n", fails == 0 ? "TUDO OK" : "FALHOU");
    return fails ? 1 : 0;
}
