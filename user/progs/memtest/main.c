#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/wait.h>
#include <unistd.h>

#define PAGE 4096

static int ok = 1;

static void check(const char *what, int good)
{
    printf("  %-48s %s\n", what, good ? "ok" : "FALHA");
    if (!good)
        ok = 0;
}

static long used_pages(void)
{
    struct sysinfo_orion si;
    if (sysinfo_orion(&si) < 0)
        return -1;
    return (long)si.used_pages;
}

static volatile char *target;
static int write_value;

static void do_write(void)
{
    *target = (char)write_value;
}

static void do_read(void)
{
    write_value = *target;
}

static int dies_with_segv(void (*fn)(void))
{
    pid_t pid = fork();
    if (pid == 0) {
        fn();
        _exit(0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV;
}

static int deep(int n)
{
    volatile char pad[8192];
    pad[0] = (char)n;
    pad[8191] = (char)n;
    if (n == 0)
        return pad[0] + pad[8191];
    return deep(n - 1) + pad[0];
}

static void basic(void)
{
    char *big = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int good = big != MAP_FAILED;
    if (good) {
        memset(big, 0xAB, 1 << 20);
        for (int i = 0; i < (1 << 20); i += PAGE)
            if ((unsigned char)big[i] != 0xAB)
                good = 0;
        munmap(big, 1 << 20);
    }
    check("mmap de 1 MiB, escrita e leitura", good);

    char *blocks[200];
    good = 1;
    for (int i = 0; i < 200; i++) {
        blocks[i] = malloc((size_t)(i * 37 % 5000 + 1));
        memset(blocks[i], i, (size_t)(i * 37 % 5000 + 1));
    }
    for (int i = 0; i < 200; i++)
        if ((unsigned char)blocks[i][0] != (unsigned char)i)
            good = 0;
    for (int i = 0; i < 200; i += 2)
        free(blocks[i]);
    for (int i = 0; i < 200; i += 2)
        blocks[i] = malloc(100);
    for (int i = 0; i < 200; i++)
        free(blocks[i]);
    check("malloc/free de 200 blocos", good);

    struct stat st;
    check("stat /bin/sh", stat("/bin/sh", &st) == 0 && st.st_size > 0);
    char buf[64];
    snprintf(buf, sizeof(buf), "%5d|%-5d|%05d|%x|%s|%.3s", 42, 42, 42, 255, "str", "abcdef");
    check("snprintf", strcmp(buf, "   42|42   |00042|ff|str|abc") == 0);
}

static void lazy(void)
{
    long before = used_pages();
    char *m = mmap(NULL, 16 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    long after_map = used_pages();
    check("mmap de 16 MiB nao gasta memoria antes do uso", m != MAP_FAILED && after_map - before < 16);
    for (int i = 0; i < 10; i++)
        m[i * (1 << 20)] = 1;
    long after_touch = used_pages();
    check("tocar 10 paginas aloca so essas paginas", after_touch - after_map >= 10 && after_touch - after_map < 40);
    check("pagina nunca tocada le zero", m[5 * (1 << 20) + 123] == 0);
    munmap(m, 16 << 20);
    check("munmap devolve a memoria", used_pages() - before < 16);

    char *a = sbrk(0);
    long b0 = used_pages();
    check("sbrk de 8 MiB", sbrk(8 << 20) == a);
    check("brk nao gasta memoria antes do uso", used_pages() - b0 < 16);
    a[0] = 1;
    a[(8 << 20) - 1] = 2;
    check("brk: o comeco e o fim funcionam", a[0] == 1 && a[(8 << 20) - 1] == 2);
    check("sbrk negativo volta o break", sbrk(-(8 << 20)) != (void *)-1 && sbrk(0) == a);
}

static void layout(void)
{
    char *m = mmap(NULL, 4 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memset(m, 'A', 4 * PAGE);
    char *f = mmap(m + PAGE, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    check("MAP_FIXED no meio de um mapeamento", f == m + PAGE);
    check("MAP_FIXED troca so a pagina pedida", m[0] == 'A' && m[PAGE] == 0 && m[2 * PAGE] == 'A');
    check("MAP_FIXED_NOREPLACE em cima da EEXIST",
          mmap(m, PAGE, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == MAP_FAILED &&
              errno == EEXIST);

    check("munmap so da pagina do meio", munmap(m + 2 * PAGE, PAGE) == 0);
    target = m + 2 * PAGE;
    check("a pagina do meio virou SIGSEGV", dies_with_segv(do_read));
    check("as vizinhas continuam la", m[PAGE - 1] == 'A' && m[3 * PAGE] == 'A');
    check("mprotect num buraco da ENOMEM", mprotect(m, 4 * PAGE, PROT_READ) < 0 && errno == ENOMEM);

    check("mprotect PROT_READ", mprotect(m, PAGE, PROT_READ) == 0);
    target = m;
    write_value = 'B';
    check("escrever numa pagina so de leitura da SIGSEGV", dies_with_segv(do_write));
    check("ler continua funcionando", m[0] == 'A');
    check("mprotect de volta para leitura e escrita", mprotect(m, PAGE, PROT_READ | PROT_WRITE) == 0);
    m[0] = 'C';
    check("depois disso a escrita funciona", m[0] == 'C');

    m[3 * PAGE] = 'Z';
    check("mprotect PROT_NONE", mprotect(m + 3 * PAGE, PAGE, PROT_NONE) == 0);
    target = m + 3 * PAGE;
    check("PROT_NONE da SIGSEGV ate na leitura", dies_with_segv(do_read));
    mprotect(m + 3 * PAGE, PAGE, PROT_READ | PROT_WRITE);
    check("PROT_NONE e volta preserva o conteudo", m[3 * PAGE] == 'Z');

    munmap(m, 4 * PAGE);
    char *again = mmap(NULL, 4 * PAGE, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("mmap seguinte reaproveita o endereco", again == m);
    munmap(again, 4 * PAGE);

    check("mmap de tamanho 0 da EINVAL",
          mmap(NULL, 0, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED && errno == EINVAL);
    check("MAP_FIXED desalinhado da EINVAL",
          mmap((void *)0x100000000123ul, PAGE, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) ==
                  MAP_FAILED &&
              errno == EINVAL);
}

static char *cow_page;

static void cow_child(void)
{
    mprotect(cow_page, PAGE, PROT_READ);
    cow_page[0] = 'X';
}

static void sharing(void)
{
    cow_page = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    cow_page[0] = 'P';
    check("mprotect numa pagina copy-on-write respeita PROT_READ", dies_with_segv(cow_child));
    check("o pai continua escrevendo na sua copia", (cow_page[0] = 'Q') == 'Q');

    volatile int *shared = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    *shared = 1;
    pid_t pid = fork();
    if (pid == 0) {
        *shared = 42;
        _exit(0);
    }
    waitpid(pid, NULL, 0);
    check("MAP_SHARED anonimo e visto pelo pai depois do fork", *shared == 42);

    volatile int *late = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    pid = fork();
    if (pid == 0) {
        *late = 7;
        _exit(0);
    }
    waitpid(pid, NULL, 0);
    check("MAP_SHARED tocado so depois do fork tambem e compartilhado", *late == 7);

    char *ro = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ro[0] = 'P';
    mprotect(ro, PAGE, PROT_READ);
    pid = fork();
    if (pid == 0) {
        mprotect(ro, PAGE, PROT_READ | PROT_WRITE);
        ro[0] = 'C';
        _exit(ro[0] == 'C' ? 0 : 1);
    }
    int st2 = 0;
    waitpid(pid, &st2, 0);
    check("filho que reabre a escrita nao muda a pagina do pai", WIFEXITED(st2) && WEXITSTATUS(st2) == 0 && ro[0] == 'P');

    char *d = mmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    d[0] = 'Q';
    d[PAGE] = 'R';
    check("madvise MADV_DONTNEED", madvise(d, PAGE, MADV_DONTNEED) == 0);
    check("depois do DONTNEED a pagina le zero", d[0] == 0 && d[PAGE] == 'R');

    pid = fork();
    if (pid == 0)
        _exit(deep(150) >= 0 ? 0 : 1);
    int st = 0;
    waitpid(pid, &st, 0);
    check("pilha cresce alem de 64 KiB (1,2 MiB de recursao)", WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

static void files(void)
{
    int fd = open("/bin/hello", O_RDONLY);
    if (fd < 0) {
        check("abrir /bin/hello", 0);
        return;
    }
    unsigned char *m = mmap(NULL, 2 * PAGE, PROT_READ, MAP_PRIVATE, fd, 0);
    check("mmap MAP_PRIVATE de arquivo", m != MAP_FAILED && m[0] == 0x7f && m[1] == 'E');
    unsigned char off_buf[16];
    unsigned char *mo = mmap(NULL, PAGE, PROT_READ, MAP_PRIVATE, fd, PAGE);
    lseek(fd, PAGE, SEEK_SET);
    read(fd, off_buf, sizeof(off_buf));
    check("mmap com deslocamento le a pagina certa", mo != MAP_FAILED && memcmp(mo, off_buf, 16) == 0);
    unsigned char *w = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    w[0] = 0;
    unsigned char first = 0;
    lseek(fd, 0, SEEK_SET);
    read(fd, &first, 1);
    check("escrever no MAP_PRIVATE nao muda o arquivo", w[0] == 0 && first == 0x7f && m[0] == 0x7f);
    check("MAP_SHARED gravavel de arquivo da ENODEV",
          mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) == MAP_FAILED && errno == ENODEV);
    munmap(m, 2 * PAGE);
    munmap(mo, PAGE);
    munmap(w, PAGE);
    close(fd);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("memtest: memoria do processo\n");
    basic();
    lazy();
    layout();
    sharing();
    files();
    printf("resultado: %s\n", ok ? "TUDO OK" : "FALHOU");
    return ok ? 0 : 1;
}
