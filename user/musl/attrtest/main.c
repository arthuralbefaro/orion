#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>
#include <sys/stat.h>
#include <sys/time.h>

#define KEEP_MODE  0751
#define KEEP_UID   4321
#define KEEP_GID   8765
#define KEEP_ATIME 999999999
#define KEEP_MTIME 1111111111

static int fails;
static char base[256];

static void check(const char *what, int ok)
{
    if (ok)
        printf("  %-52s ok\n", what);
    else
        printf("  %-52s FALHA (errno %d)\n", what, errno);
    if (!ok)
        fails++;
}

static const char *P(const char *rel)
{
    static char bufs[6][600];
    static int n;
    char *b = bufs[n++ % 6];
    snprintf(b, 600, "%s/at/%s", base, rel);
    return b;
}

static int fails_with(int r, int err)
{
    return r < 0 && errno == err;
}

static int put_file(const char *path, mode_t mode, size_t len)
{
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, mode);
    if (fd < 0)
        return -1;
    char buf[512];
    memset(buf, 'a', sizeof(buf));
    size_t done = 0;
    while (done < len) {
        size_t chunk = len - done < sizeof(buf) ? len - done : sizeof(buf);
        if (write(fd, buf, chunk) != (ssize_t)chunk) {
            close(fd);
            return -1;
        }
        done += chunk;
    }
    return close(fd);
}

static mode_t perm_of(const char *path)
{
    struct stat st;
    if (lstat(path, &st) < 0)
        return (mode_t)-1;
    return st.st_mode & 07777;
}

static int near_now(time_t t)
{
    time_t now = time(NULL);
    return t >= now - 5 && t <= now + 5;
}

static void modes(void)
{
    struct stat st;
    mode_t old = umask(022);
    check("umask devolve o anterior (022 no boot)", old == 022);
    check("open com 0666 e umask 022 cria 0644", put_file(P("f"), 0666, 0) == 0 && perm_of(P("f")) == 0644);
    umask(077);
    check("mkdir com 0777 e umask 077 cria 0700", mkdir(P("d"), 0777) == 0 && perm_of(P("d")) == 0700);
    check("umask devolve 077", umask(0) == 077);
    check("open com 0606 e umask 0 cria 0606", put_file(P("g"), 0606, 0) == 0 && perm_of(P("g")) == 0606);
    umask(022);
    check("chmod 640", chmod(P("f"), 0640) == 0 && perm_of(P("f")) == 0640);
    check("chmod guarda setuid, setgid e sticky", chmod(P("f"), 07755) == 0 && perm_of(P("f")) == 07755);
    check("chmod nao muda o tipo", stat(P("f"), &st) == 0 && S_ISREG(st.st_mode));
    int fd = open(P("f"), O_RDONLY);
    check("fchmod 600", fd >= 0 && fchmod(fd, 0600) == 0 && perm_of(P("f")) == 0600);
    check("fstat ve o modo novo", fd >= 0 && fstat(fd, &st) == 0 && (st.st_mode & 07777) == 0600);
    close(fd);
    check("chmod em diretorio", chmod(P("d"), 0750) == 0 && stat(P("d"), &st) == 0 && S_ISDIR(st.st_mode) &&
                                    (st.st_mode & 07777) == 0750);
    check("chmod segue o link e muda o alvo", symlink("f", P("lf")) == 0 && chmod(P("lf"), 0604) == 0 &&
                                                  perm_of(P("f")) == 0604 && perm_of(P("lf")) == 0777);
    check("chmod de quem nao existe da ENOENT", fails_with(chmod(P("nada"), 0644), ENOENT));
    check("fchmod de descritor invalido da EBADF", fails_with(fchmod(999, 0644), EBADF));
}

static void owners(void)
{
    struct stat st;
    check("chown 1000:100", chown(P("f"), 1000, 100) == 0 && stat(P("f"), &st) == 0 && st.st_uid == 1000 &&
                                st.st_gid == 100);
    check("chown -1 -1 nao muda nada", chown(P("f"), (uid_t)-1, (gid_t)-1) == 0 && stat(P("f"), &st) == 0 &&
                                           st.st_uid == 1000 && st.st_gid == 100);
    int fd = open(P("f"), O_RDONLY);
    check("fchown so do dono", fd >= 0 && fchown(fd, 12, (gid_t)-1) == 0 && fstat(fd, &st) == 0 &&
                                   st.st_uid == 12 && st.st_gid == 100);
    close(fd);
    check("lchown muda o link e nao o alvo", lchown(P("lf"), 77, 88) == 0 && lstat(P("lf"), &st) == 0 &&
                                                 st.st_uid == 77 && st.st_gid == 88 && stat(P("f"), &st) == 0 &&
                                                 st.st_uid == 12);
    check("chown segue o link", chown(P("lf"), 5, 6) == 0 && stat(P("f"), &st) == 0 && st.st_uid == 5 &&
                                    st.st_gid == 6 && lstat(P("lf"), &st) == 0 && st.st_uid == 77);
    check("fchownat com AT_SYMLINK_NOFOLLOW", fchownat(AT_FDCWD, P("lf"), 1, 2, AT_SYMLINK_NOFOLLOW) == 0 &&
                                                  lstat(P("lf"), &st) == 0 && st.st_uid == 1 && st.st_gid == 2);
    chmod(P("f"), 06755);
    check("chown tira setuid e setgid", chown(P("f"), 0, 0) == 0 && perm_of(P("f")) == 0755);
    check("chown em diretorio", chown(P("d"), 3, 4) == 0 && stat(P("d"), &st) == 0 && st.st_uid == 3 &&
                                    st.st_gid == 4);
    check("chown de quem nao existe da ENOENT", fails_with(chown(P("nada"), 1, 1), ENOENT));
}

static int times_are(const char *path, time_t a, time_t m)
{
    struct stat st;
    return lstat(path, &st) == 0 && st.st_atime == a && st.st_mtime == m;
}

static void times(void)
{
    struct stat st, st2;
    struct timespec ts[2] = { { 1000000000, 0 }, { 1234567890, 0 } };
    check("utimensat grava atime e mtime", utimensat(AT_FDCWD, P("f"), ts, 0) == 0 &&
                                               times_are(P("f"), 1000000000, 1234567890));
    check("ctime e a hora da mudanca", stat(P("f"), &st) == 0 && near_now(st.st_ctime));
    ts[0].tv_nsec = UTIME_OMIT;
    ts[1].tv_sec = 1300000000;
    check("UTIME_OMIT guarda o atime", utimensat(AT_FDCWD, P("f"), ts, 0) == 0 &&
                                           times_are(P("f"), 1000000000, 1300000000));
    ts[0].tv_nsec = 0;
    ts[1].tv_nsec = UTIME_NOW;
    check("UTIME_NOW poe a hora atual", utimensat(AT_FDCWD, P("f"), ts, 0) == 0 && stat(P("f"), &st) == 0 &&
                                            st.st_atime == 1000000000 && near_now(st.st_mtime));
    check("utimensat sem tempos usa agora", utimensat(AT_FDCWD, P("g"), NULL, 0) == 0 && stat(P("g"), &st) == 0 &&
                                                near_now(st.st_atime) && near_now(st.st_mtime));
    ts[0].tv_nsec = 1000000000;
    check("nanossegundo invalido da EINVAL", fails_with(utimensat(AT_FDCWD, P("f"), ts, 0), EINVAL));
    int fd = open(P("f"), O_RDONLY);
    struct timespec ft[2] = { { 1400000000, 0 }, { 1500000000, 0 } };
    check("futimens", fd >= 0 && futimens(fd, ft) == 0 && times_are(P("f"), 1400000000, 1500000000));
    close(fd);
    struct utimbuf ub = { 1600000000, 1700000000 };
    check("utime", utime(P("f"), &ub) == 0 && times_are(P("f"), 1600000000, 1700000000));
    struct timeval tv[2] = { { 1100000000, 0 }, { 1200000000, 0 } };
    check("utimes", utimes(P("f"), tv) == 0 && times_are(P("f"), 1100000000, 1200000000));
    struct timespec lt[2] = { { 900000000, 0 }, { 950000000, 0 } };
    check("AT_SYMLINK_NOFOLLOW muda as datas do link",
          utimensat(AT_FDCWD, P("lf"), lt, AT_SYMLINK_NOFOLLOW) == 0 && times_are(P("lf"), 900000000, 950000000) &&
              times_are(P("f"), 1100000000, 1200000000));
    fd = open(P("f"), O_WRONLY | O_APPEND);
    check("write atualiza mtime e ctime", fd >= 0 && write(fd, "x", 1) == 1 && fstat(fd, &st) == 0 &&
                                              near_now(st.st_mtime) && near_now(st.st_ctime));
    close(fd);
    check("truncate atualiza o mtime", utimensat(AT_FDCWD, P("f"), (struct timespec[2]){ { 5, 0 }, { 5, 0 } }, 0) == 0 &&
                                           truncate(P("f"), 3) == 0 && stat(P("f"), &st) == 0 && near_now(st.st_mtime));
    struct timespec dt[2] = { { 10, 0 }, { 10, 0 } };
    utimensat(AT_FDCWD, P("d"), dt, 0);
    check("criar no diretorio muda o mtime dele", put_file(P("d/novo"), 0644, 0) == 0 && stat(P("d"), &st) == 0 &&
                                                      near_now(st.st_mtime));
    utimensat(AT_FDCWD, P("d"), dt, 0);
    check("apagar no diretorio muda o mtime dele", unlink(P("d/novo")) == 0 && stat(P("d"), &st) == 0 &&
                                                       near_now(st.st_mtime));
    check("stat e lstat do link diferem", stat(P("lf"), &st) == 0 && lstat(P("lf"), &st2) == 0 &&
                                              st.st_ino != st2.st_ino && S_ISLNK(st2.st_mode));
}

static void counts(void)
{
    struct stat st;
    check("arquivo tem 1 link", stat(P("f"), &st) == 0 && st.st_nlink == 1);
    check("diretorio vazio tem 2 links", stat(P("d"), &st) == 0 && st.st_nlink == 2);
    mkdir(P("d/s1"), 0755);
    mkdir(P("d/s2"), 0755);
    put_file(P("d/arq"), 0644, 0);
    check("diretorio com 2 subdiretorios tem 4 links", stat(P("d"), &st) == 0 && st.st_nlink == 4);
    rmdir(P("d/s1"));
    check("rmdir baixa o contador do pai", stat(P("d"), &st) == 0 && st.st_nlink == 3);
    rmdir(P("d/s2"));
    unlink(P("d/arq"));
    check("arquivo de 20000 bytes ocupa blocos", put_file(P("big"), 0644, 20000) == 0 && stat(P("big"), &st) == 0 &&
                                                     st.st_blocks >= 40 && st.st_blocks <= 64);
    check("arquivo esparso de 4 MiB quase nao ocupa", truncate(P("big"), 4 << 20) == 0 && stat(P("big"), &st) == 0 &&
                                                          st.st_size == 4 << 20 && st.st_blocks <= 64);
    unlink(P("big"));
}

static void keep(void)
{
    if (put_file(P("keep"), 0644, 100) < 0 || chmod(P("keep"), KEEP_MODE) < 0 ||
        chown(P("keep"), KEEP_UID, KEEP_GID) < 0) {
        check("prepara at/keep", 0);
        return;
    }
    struct timespec ts[2] = { { KEEP_ATIME, 0 }, { KEEP_MTIME, 0 } };
    check("prepara at/keep", utimensat(AT_FDCWD, P("keep"), ts, 0) == 0);
}

static int verify_keep(void)
{
    struct stat st;
    int ok = stat(P("keep"), &st) == 0;
    check("at/keep existe", ok);
    if (!ok)
        return 1;
    check("modo sobreviveu", (st.st_mode & 07777) == KEEP_MODE && S_ISREG(st.st_mode));
    check("dono e grupo sobreviveram", st.st_uid == KEEP_UID && st.st_gid == KEEP_GID);
    check("datas sobreviveram", st.st_atime == KEEP_ATIME && st.st_mtime == KEEP_MTIME);
    check("tamanho sobreviveu", st.st_size == 100);
    check("apaga at/keep", unlink(P("keep")) == 0);
    check("apaga o diretorio de teste", rmdir(P("")) == 0);
    return 0;
}

static void cleanup(void)
{
    unlink(P("lf"));
    unlink(P("f"));
    unlink(P("g"));
    rmdir(P("d"));
}

int main(int argc, char **argv)
{
    int verify = argc > 2 && strcmp(argv[1], "-k") == 0;
    if (argc < 2 || (argc > 2 && !verify)) {
        fprintf(stderr, "uso: attrtest [-k] <diretorio>\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    snprintf(base, sizeof(base), "%s", argv[verify ? 2 : 1]);
    size_t n = strlen(base);
    while (n > 1 && base[n - 1] == '/')
        base[--n] = '\0';
    if (verify) {
        printf("\nattrtest: conferindo at/keep em %s\n", base);
        verify_keep();
    } else {
        printf("\nattrtest: modo, dono e datas em %s\n", base);
        char dir[300];
        snprintf(dir, sizeof(dir), "%s/at", base);
        if (mkdir(dir, 0755) < 0) {
            printf("attrtest: nao criou %s (errno %d)\n", dir, errno);
            return 1;
        }
        modes();
        owners();
        times();
        counts();
        cleanup();
        keep();
    }
    printf("attrtest: %s (%d falha(s))\n", fails ? "FALHOU" : "TUDO OK", fails);
    return fails ? 1 : 0;
}
