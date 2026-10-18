#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define SYS_KFAULT 1002
#define KFAULT_EXEC_USER 1
#define KFAULT_READ_USER 2

static int kernel_fault(int op, const char *what)
{
    unsigned char *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        perror("crash: mmap");
        return 1;
    }
    page[0] = 0xC3;
    printf("crash: pedindo ao kernel para %s em %p...\n", what, (void *)page);
    fflush(stdout);
    long r = syscall(SYS_KFAULT, op, page);
    printf("crash: o kernel voltou (%ld, %s); a protecao nao esta ligada\n", r, r < 0 ? strerror(errno) : "ok");
    return 1;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "smep") == 0)
        return kernel_fault(KFAULT_EXEC_USER, "executar codigo de usuario (SMEP)");
    if (argc > 1 && strcmp(argv[1], "smap") == 0)
        return kernel_fault(KFAULT_READ_USER, "ler memoria de usuario sem copy_from_user (SMAP)");
    if (argc > 1) {
        fprintf(stderr, "uso: crash [smep|smap]\n");
        return 2;
    }
    puts("crash: vou escrever em memoria do kernel...");
    fflush(stdout);
    *(volatile int *)0xffffffff80000000 = 1;
    puts("crash: se voce ve isto, a protecao falhou!");
    return 0;
}
