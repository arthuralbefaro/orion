#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define SIZE 8192

static int failures;

static void check(const char *what, int ok)
{
    printf("  %-52s %s\n", what, ok ? "ok" : "FALHA");
    if (!ok)
        failures++;
}

static int maps_has_shared(void)
{
    char buf[8192];
    int fd = open("/proc/self/maps", O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = 0;
    return strstr(buf, "rw-s") != NULL;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/tmp/shmtest.buf";
    const char *ext2 = argc > 2 ? argv[2] : "/shmtest.ext2";
    printf("shmtest: MAP_SHARED de arquivo em %s\n", path);

    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    check("cria e aumenta o arquivo", fd >= 0 && ftruncate(fd, SIZE) == 0);
    unsigned char *m = mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check("mmap MAP_SHARED com escrita", m != MAP_FAILED);
    if (m == MAP_FAILED)
        return 1;
    check("/proc/self/maps mostra rw-s", maps_has_shared());

    memset(m, 0x5A, SIZE);
    unsigned char back[SIZE];
    check("read() ve o que foi escrito pelo mapeamento",
          pread(fd, back, SIZE, 0) == SIZE && back[0] == 0x5A && back[SIZE - 1] == 0x5A);

    const char msg[] = "escrito por write()";
    pwrite(fd, msg, sizeof(msg), 4096);
    check("o mapeamento ve o que write() escreveu", memcmp(m + 4096, msg, sizeof(msg)) == 0);

    pid_t pid = fork();
    if (pid == 0) {
        int cfd = open(path, O_RDWR);
        unsigned char *c = cfd >= 0 ? mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, cfd, 0) : MAP_FAILED;
        if (c == MAP_FAILED)
            _exit(2);
        c[100] = 0xC3;
        m[200] = 0x7E;
        _exit(c[0] == 0x5A ? 0 : 3);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    check("outro processo mapeia o mesmo arquivo e ve os dados", WIFEXITED(st) && WEXITSTATUS(st) == 0);
    check("o que o outro processo escreveu aparece aqui", m[100] == 0xC3);
    check("mapeamento herdado no fork continua compartilhado", m[200] == 0x7E);

    munmap(m, SIZE);
    check("depois do munmap o arquivo guarda os dados",
          pread(fd, back, SIZE, 0) == SIZE && back[100] == 0xC3 && back[200] == 0x7E);
    close(fd);

    int ro = open(path, O_RDONLY);
    void *r = mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, ro, 0);
    check("PROT_WRITE com o arquivo aberto so para leitura da EACCES", r == MAP_FAILED && errno == EACCES);
    close(ro);
    unlink(path);

    int e = open(ext2, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (e >= 0) {
        ftruncate(e, SIZE);
        void *x = mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, e, 0);
        check("MAP_SHARED com escrita fora do tmpfs da ENODEV", x == MAP_FAILED && errno == ENODEV);
        close(e);
        unlink(ext2);
    }

    printf("shmtest: %s (%d falha(s))\n", failures ? "FALHOU" : "TUDO OK", failures);
    return failures ? 1 : 0;
}
