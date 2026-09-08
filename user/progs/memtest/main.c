#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>


int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    int ok = 1;

    char *big = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (big == MAP_FAILED) { perror("mmap"); return 1; }
    memset(big, 0xAB, 1 << 20);
    for (int i = 0; i < (1 << 20); i += 4096)
        if ((unsigned char)big[i] != 0xAB) ok = 0;
    printf("mmap 1 MiB em %p: %s\n", (void *)big, ok ? "ok" : "FALHA");
    munmap(big, 1 << 20);

    char *blocks[200];
    for (int i = 0; i < 200; i++) {
        blocks[i] = malloc((size_t)(i * 37 % 5000 + 1));
        memset(blocks[i], i, (size_t)(i * 37 % 5000 + 1));
    }
    for (int i = 0; i < 200; i++)
        if ((unsigned char)blocks[i][0] != (unsigned char)i) ok = 0;
    for (int i = 0; i < 200; i += 2) free(blocks[i]);
    for (int i = 0; i < 200; i += 2) blocks[i] = malloc(100);
    for (int i = 0; i < 200; i++) free(blocks[i]);
    printf("malloc/free 200 blocos: %s\n", ok ? "ok" : "FALHA");

    struct stat st;
    if (stat("/bin/sh", &st) == 0)
        printf("stat /bin/sh: %lu bytes, modo %o\n", st.st_size, st.st_mode);
    else
        ok = 0;

    char cwd[128];
    getcwd(cwd, sizeof(cwd));
    printf("cwd: %s, isatty(stdout)=%d\n", cwd, isatty(1));

    char buf[64];
    snprintf(buf, sizeof(buf), "%5d|%-5d|%05d|%x|%s|%.3s", 42, 42, 42, 255, "str", "abcdef");
    printf("snprintf: [%s] %s\n", buf, strcmp(buf, "   42|42   |00042|ff|str|abc") == 0 ? "ok" : "FALHA");

    printf("resultado: %s\n", ok ? "TUDO OK" : "FALHOU");
    return ok ? 0 : 1;
}
