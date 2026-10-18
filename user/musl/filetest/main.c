#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define SYS_NEWFSTATAT_N 262
#define SYS_OPENAT_N     257
#define SYS_GETDENTS64_N 217
#define SYS_ACCESS_N     21
#define SYS_FACCESSAT_N  269
#define SYS_READLINK_N   89
#define SYS_DUP3_N       292
#define SYS_PIPE2_N      293
#define AT_FDCWD_N       (-100)

struct linux_dirent64 {
    unsigned long d_ino;
    long d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};

static int fails;

static long syscall4(long n, long a, long b, long c, long d)
{
    long r = syscall(n, a, b, c, d, 0, 0);
    return r < 0 ? -errno : r;
}

static long syscall3(long n, long a, long b, long c)
{
    return syscall4(n, a, b, c, 0);
}

static long syscall2(long n, long a, long b)
{
    return syscall4(n, a, b, 0, 0);
}

static void check(const char *what, int ok)
{
    printf("  %-46s %s\n", what, ok ? "ok" : "FALHA");
    if (!ok)
        fails++;
}

static int child_checks_fd(int fd)
{
    long r = syscall3(SYS_fcntl, fd, F_GETFD, 0);
    return r == -EBADF ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc > 2 && strcmp(argv[1], "-c") == 0)
        return child_checks_fd(atoi(argv[2]));

    printf("filetest: arquivos no formato do Linux\n");
    check("struct stat tem 144 bytes", sizeof(struct stat) == 144);

    struct stat st;
    memset(&st, 0, sizeof(st));
    check("stat de /bin/sh", stat("/bin/sh", &st) == 0);
    check("st_mode e regular e st_size > 0", S_ISREG(st.st_mode) && st.st_size > 1000);
    check("st_blocks em setores de 512 (o que o disco aloca)",
          st.st_blocks * 512 >= st.st_size && st.st_blocks * 512 < st.st_size + 65536);
    check("st_nlink = 1", st.st_nlink == 1);
    long size_sh = (long)st.st_size;

    memset(&st, 0, sizeof(st));
    check("stat de /bin e diretorio", stat("/bin", &st) == 0 && S_ISDIR(st.st_mode));

    memset(&st, 0, sizeof(st));
    long r = syscall4(SYS_NEWFSTATAT_N, AT_FDCWD_N, (long)"/bin/sh", (long)&st, 0);
    check("newfstatat com AT_FDCWD", r == 0 && (long)st.st_size == size_sh);

    int dir = open("/bin", O_RDONLY | O_DIRECTORY);
    check("open de diretorio com O_DIRECTORY", dir >= 0);
    memset(&st, 0, sizeof(st));
    r = syscall4(SYS_NEWFSTATAT_N, dir, (long)"sh", (long)&st, 0);
    check("newfstatat relativo a um diretorio", r == 0 && (long)st.st_size == size_sh);
    int fd = (int)syscall4(SYS_OPENAT_N, dir, (long)"sh", O_RDONLY, 0);
    check("openat relativo a um diretorio", fd >= 0);
    if (fd >= 0) {
        check("lseek SEEK_END da o tamanho", lseek(fd, 0, SEEK_END) == size_sh);
        char m[4];
        lseek(fd, 0, SEEK_SET);
        check("depois de SEEK_SET le o ELF", read(fd, m, 4) == 4 && m[0] == 0x7f && m[1] == 'E');
        memset(&st, 0, sizeof(st));
        check("fstat do arquivo aberto", fstat(fd, &st) == 0 && (long)st.st_size == size_sh);
        close(fd);
    }
    check("open de arquivo com O_DIRECTORY da ENOTDIR", open("/bin/sh", O_RDONLY | O_DIRECTORY) < 0 &&
                                                          errno == ENOTDIR);

    char buf[2048];
    int found_sh = 0, found_dot = 0, entries = 0;
    for (;;) {
        long n = syscall3(SYS_GETDENTS64_N, dir, (long)buf, sizeof(buf));
        if (n <= 0)
            break;
        for (long off = 0; off < n;) {
            struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + off);
            if (strcmp(d->d_name, "sh") == 0 && d->d_type == 8)
                found_sh = 1;
            if (strcmp(d->d_name, ".") == 0)
                found_dot = 1;
            entries++;
            off += d->d_reclen;
        }
    }
    check("getdents64 lista /bin com sh (DT_REG)", found_sh && entries > 20);
    check("getdents64 em blocos de 2 KiB juntou tudo", entries >= 40);
    (void)found_dot;
    check("lseek 0 num diretorio recomeca a listagem", lseek(dir, 0, SEEK_SET) == 0 &&
                                                        syscall3(SYS_GETDENTS64_N, dir, (long)buf, sizeof(buf)) > 0);
    close(dir);

    check("access de arquivo existente", syscall2(SYS_ACCESS_N, (long)"/bin/sh", 0) == 0);
    check("access de inexistente da ENOENT", syscall2(SYS_ACCESS_N, (long)"/nao/existe", 0) == -ENOENT);
    check("faccessat com AT_FDCWD", syscall4(SYS_FACCESSAT_N, AT_FDCWD_N, (long)"/bin", 0, 0) == 0);
    check("readlink de arquivo comum da EINVAL",
          syscall3(SYS_READLINK_N, (long)"/bin/sh", (long)buf, sizeof(buf)) == -EINVAL);
    check("readlink de inexistente da ENOENT",
          syscall3(SYS_READLINK_N, (long)"/nao/existe", (long)buf, sizeof(buf)) == -ENOENT);

    int d = fcntl(1, F_DUPFD, 40);
    check("fcntl F_DUPFD devolve >= 40", d >= 40);
    check("FD_CLOEXEC comeca desligado", fcntl(d, F_GETFD) == 0);
    check("F_SETFD liga FD_CLOEXEC", fcntl(d, F_SETFD, FD_CLOEXEC) == 0 && fcntl(d, F_GETFD) == FD_CLOEXEC);
    int e = (int)syscall3(SYS_fcntl, 1, F_DUPFD_CLOEXEC, 50);
    check("F_DUPFD_CLOEXEC", e >= 50 && syscall3(SYS_fcntl, e, F_GETFD, 0) == FD_CLOEXEC);
    check("dup3 com O_CLOEXEC", syscall3(SYS_DUP3_N, 1, 60, O_CLOEXEC) == 60 && fcntl(60, F_GETFD) == FD_CLOEXEC);
    check("dup3 com fds iguais da EINVAL", syscall3(SYS_DUP3_N, 1, 1, 0) == -EINVAL);
    int keep = fcntl(1, F_DUPFD, 70);

    pid_t pid = fork();
    if (pid == 0) {
        char n1[8], n2[8];
        snprintf(n1, sizeof(n1), "%d", d);
        snprintf(n2, sizeof(n2), "%d", keep);
        char *a1[] = { "/bin/filetest", "-c", n1, NULL };
        int pid2 = fork();
        if (pid2 == 0) {
            execv("/bin/filetest", a1);
            _exit(9);
        }
        int s1 = 0;
        waitpid(pid2, &s1, 0);
        char *a2[] = { "/bin/filetest", "-c", n2, NULL };
        int pid3 = fork();
        if (pid3 == 0) {
            execv("/bin/filetest", a2);
            _exit(9);
        }
        int s2 = 0;
        waitpid(pid3, &s2, 0);
        _exit((WEXITSTATUS(s1) == 0 && WEXITSTATUS(s2) == 1) ? 0 : 1);
    }
    int stc = 0;
    waitpid(pid, &stc, 0);
    check("exec fecha FD_CLOEXEC e mantem os outros", WIFEXITED(stc) && WEXITSTATUS(stc) == 0);
    close(d);
    close(e);
    close(60);
    close(keep);

    int p[2];
    check("pipe2 com O_CLOEXEC|O_NONBLOCK", syscall2(SYS_PIPE2_N, (long)p, O_CLOEXEC | O_NONBLOCK) == 0);
    check("pipe2: as duas pontas com FD_CLOEXEC", fcntl(p[0], F_GETFD) == FD_CLOEXEC && fcntl(p[1], F_GETFD) == FD_CLOEXEC);
    check("pipe2: leitura vazia da EAGAIN", read(p[0], buf, 4) < 0 && errno == EAGAIN);
    close(p[0]);
    close(p[1]);

    int c = (int)syscall4(SYS_OPENAT_N, AT_FDCWD_N, (long)"/bin/sh", O_RDONLY | O_CLOEXEC, 0);
    check("open com O_CLOEXEC", c >= 0 && syscall3(SYS_fcntl, c, F_GETFD, 0) == FD_CLOEXEC);
    close(c);

    printf("filetest: fim, %s\n", fails ? "FALHOU" : "TUDO OK");
    return fails ? 1 : 0;
}
