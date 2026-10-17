#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

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

static const char *B(void)
{
    return base[0] ? base : "/";
}

static int err_is(int r, int e)
{
    return r < 0 && errno == e;
}

static const char *P(const char *rel)
{
    static char bufs[4][512];
    static int n;
    char *b = bufs[n++ & 3];
    snprintf(b, 512, "%s/%s", base, rel);
    return b;
}

static int same(const char *a, const char *b)
{
    struct stat sa, sb;
    if (stat(a, &sa) < 0 || stat(b, &sb) < 0)
        return 0;
    return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

static int put(const char *path, const char *text)
{
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    ssize_t n = write(fd, text, strlen(text));
    close(fd);
    return n == (ssize_t)strlen(text) ? 0 : -1;
}

static int content_is(const char *path, const char *text)
{
    char buf[128];
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n < 0)
        return 0;
    buf[n] = 0;
    return strcmp(buf, text) == 0;
}

static int all_zero(int fd, off_t from, off_t to)
{
    char buf[512];
    for (off_t o = from; o < to;) {
        size_t want = (size_t)(to - o) < sizeof(buf) ? (size_t)(to - o) : sizeof(buf);
        ssize_t n = pread(fd, buf, want, o);
        if (n <= 0)
            return 0;
        for (ssize_t i = 0; i < n; i++)
            if (buf[i])
                return 0;
        o += n;
    }
    return 1;
}

static void test_mount_crossing(void)
{
    struct stat sr, sd;
    check("/dev/.. e a raiz", same("/dev/..", "/"));
    check("/dev/../dev/null existe", stat("/dev/../dev/null", &sd) == 0 && S_ISCHR(sd.st_mode));
    check("/dev esta em outro sistema de arquivos",
          stat("/", &sr) == 0 && stat("/dev", &sd) == 0 && sr.st_dev != sd.st_dev);
    check("/.. e a propria raiz", same("/..", "/") && same("/../../..", "/"));

    char cwd[256];
    int ok = chdir("/dev") == 0 && chdir("..") == 0 && same(".", "/") &&
             getcwd(cwd, sizeof(cwd)) && strcmp(cwd, "/") == 0;
    check("chdir /dev e depois .. volta para /", ok);

    int fd = open("/dev", O_RDONLY | O_DIRECTORY);
    struct stat sf;
    int up = fd >= 0 ? openat(fd, "..", O_RDONLY | O_DIRECTORY) : -1;
    check("openat(fd de /dev, \"..\") e a raiz",
          up >= 0 && fstat(up, &sf) == 0 && stat("/", &sr) == 0 && sf.st_ino == sr.st_ino &&
              sf.st_dev == sr.st_dev);
    if (up >= 0)
        close(up);
    if (fd >= 0)
        close(fd);

    check("rmdir de ponto de montagem da EBUSY", err_is(rmdir("/dev"), EBUSY));
    check("rename de ponto de montagem da EBUSY", err_is(rename("/dev", "/devx"), EBUSY));
    check("mount em arquivo da ENOTDIR", err_is(mount("devfs", "/bin/hello", "devfs", 0, NULL), ENOTDIR));
    check("mount de tipo desconhecido da ENODEV", err_is(mount("x", "/mnt", "nada", 0, NULL), ENODEV));
    check("mount vfat de /dev/null da ENOTBLK", err_is(mount("/dev/null", "/mnt", "vfat", 0, NULL), ENOTBLK));
    check("umount do que nao e montagem da EINVAL", err_is(umount("/bin"), EINVAL));
    check("umount da raiz da EINVAL", err_is(umount("/"), EINVAL));
    check("umount de caminho inexistente da ENOENT", err_is(umount("/nao/existe"), ENOENT));
    check("arquivo no meio do caminho da ENOTDIR", err_is(stat("/bin/hello/x", &sf), ENOTDIR));
    check("barra no fim de arquivo da ENOTDIR", err_is(stat("/bin/hello/", &sf), ENOTDIR));
}

static void test_devfs_mount(void)
{
    char m[512];
    strcpy(m, P("vt-m"));
    rmdir(m);
    check("mkdir do ponto de montagem", mkdir(m, 0755) == 0);
    check("mount devfs no diretorio", mount("devfs", m, "devfs", 0, NULL) == 0);
    struct stat st;
    check("dispositivo aparece dentro da montagem", stat(P("vt-m/null"), &st) == 0 && S_ISCHR(st.st_mode));
    check("montagem/.. e o diretorio de cima", same(P("vt-m/.."), B()));
    check("mount de novo no mesmo lugar da EBUSY", err_is(mount("devfs", m, "devfs", 0, NULL), EBUSY));
    check("rmdir do ponto montado da EBUSY", err_is(rmdir(m), EBUSY));

    int ok = chdir(m) == 0 && err_is(umount(m), EBUSY) && chdir("/") == 0;
    check("umount com o cwd dentro da EBUSY", ok);
    int fd = open(P("vt-m/zero"), O_RDONLY);
    check("umount com arquivo aberto da EBUSY", fd >= 0 && err_is(umount(m), EBUSY));
    if (fd >= 0)
        close(fd);
    check("umount depois de fechar tudo", umount(m) == 0);
    check("depois do umount o diretorio esta vazio", err_is(stat(P("vt-m/null"), &st), ENOENT));
    check("rmdir do ponto desmontado", rmdir(m) == 0);
}

static void test_shared_vnode(void)
{
    const char *a = P("vt-a");
    unlink(a);
    int w = open(a, O_CREAT | O_RDWR, 0644);
    int r = open(a, O_RDONLY);
    char buf[200];
    memset(buf, 'x', sizeof(buf));
    check("dois descritores no mesmo arquivo", w >= 0 && r >= 0);
    check("write de 100 bytes", write(w, buf, 100) == 100);
    struct stat st;
    check("fstat do outro descritor ve o tamanho novo", fstat(r, &st) == 0 && st.st_size == 100);
    check("lseek SEEK_END do outro descritor", lseek(r, 0, SEEK_END) == 100);

    check("unlink com o arquivo aberto", unlink(a) == 0);
    check("o nome sumiu", err_is(stat(a, &st), ENOENT));
    check("leitura continua pelo descritor", pread(r, buf, sizeof(buf), 0) == 100 && buf[99] == 'x');
    check("escrita continua pelo descritor", write(w, buf, 50) == 50 && fstat(r, &st) == 0 && st.st_size == 150);
    check("criar de novo com o mesmo nome", put(a, "novo") == 0 && content_is(a, "novo"));
    check("o arquivo aberto continua o antigo", pread(r, buf, 4, 0) == 4 && memcmp(buf, "xxxx", 4) == 0);
    close(w);
    close(r);
    check("o arquivo novo ficou intacto", content_is(a, "novo"));
    unlink(a);
}

static void test_rename(void)
{
    const char *b = P("vt-b"), *c = P("vt-c");
    put(b, "B");
    put(c, "CC");
    check("rename por cima de arquivo existente", rename(b, c) == 0);
    check("o destino tem o conteudo da origem", content_is(c, "B"));
    struct stat st;
    check("a origem sumiu", err_is(stat(b, &st), ENOENT));
    unlink(c);

    mkdir(P("vt-x"), 0755);
    mkdir(P("vt-y"), 0755);
    put(P("vt-x/f"), "F");
    check("rename de arquivo entre diretorios", rename(P("vt-x/f"), P("vt-y/g")) == 0 && content_is(P("vt-y/g"), "F"));
    int fd = open(P("vt-y/g"), O_RDWR | O_APPEND);
    check("rename com o arquivo aberto", fd >= 0 && rename(P("vt-y/g"), P("vt-y/h")) == 0);
    check("escrita depois do rename vai para o nome novo",
          fd >= 0 && write(fd, "G", 1) == 1 && close(fd) == 0 && content_is(P("vt-y/h"), "FG"));
    mkdir(P("vt-x/sub"), 0755);
    put(P("vt-x/sub/k"), "K");
    check("rename de diretorio entre diretorios", rename(P("vt-x/sub"), P("vt-y/sub")) == 0);
    check("o .. do diretorio movido aponta o pai novo", same(P("vt-y/sub/.."), P("vt-y")));
    check("o conteudo do diretorio movido veio junto", content_is(P("vt-y/sub/k"), "K"));
    check("rename de diretorio para dentro dele mesmo da EINVAL",
          err_is(rename(P("vt-y"), P("vt-y/sub/z")), EINVAL));
    check("rmdir de diretorio com conteudo da ENOTEMPTY", err_is(rmdir(P("vt-y")), ENOTEMPTY));
    mkdir(P("vt-e"), 0755);
    check("rename de diretorio por cima de diretorio vazio",
          rename(P("vt-y/sub"), P("vt-e")) == 0 && content_is(P("vt-e/k"), "K"));
    check("rename de diretorio por cima de arquivo da ENOTDIR",
          put(P("vt-y/arq"), "A") == 0 && err_is(rename(P("vt-e"), P("vt-y/arq")), ENOTDIR));
    check("rename de arquivo por cima de diretorio da EISDIR",
          err_is(rename(P("vt-y/arq"), P("vt-e")), EISDIR));
    check("rename para outro sistema de arquivos da EXDEV", err_is(rename(P("vt-y/arq"), "/dev/arq"), EXDEV));
    unlink(P("vt-y/arq"));
    unlink(P("vt-y/h"));
    unlink(P("vt-e/k"));
    rmdir(P("vt-e"));
    rmdir(P("vt-y"));
    rmdir(P("vt-x"));
    check("tudo removido", err_is(stat(P("vt-x"), &st), ENOENT) && err_is(stat(P("vt-y"), &st), ENOENT));
}

static void test_cwd(void)
{
    const char *g = P("vt-gone");
    mkdir(g, 0755);
    int dfd = open(B(), O_RDONLY | O_DIRECTORY);
    char cwd[256];
    check("fchdir para o diretorio de teste",
          dfd >= 0 && fchdir(dfd) == 0 && getcwd(cwd, sizeof(cwd)) && strcmp(cwd, B()) == 0);
    if (dfd >= 0)
        close(dfd);
    check("chdir relativo", chdir("vt-gone") == 0);
    check("rmdir do proprio cwd", rmdir(g) == 0);
    check("criar arquivo no cwd removido da ENOENT", err_is(open("novo", O_CREAT | O_WRONLY, 0644), ENOENT));
    check("chdir / depois disso", chdir("/") == 0);
}

static void test_holes(void)
{
    const char *h = P("vt-h");
    unlink(h);
    int fd = open(h, O_CREAT | O_RDWR, 0644);
    check("pwrite depois do fim do arquivo", fd >= 0 && pwrite(fd, "Z", 1, 5000) == 1);
    struct stat st;
    check("tamanho cobre o buraco", fstat(fd, &st) == 0 && st.st_size == 5001);
    check("o buraco le zeros", all_zero(fd, 0, 5000));
    char junk[64];
    memset(junk, 'q', sizeof(junk));
    pwrite(fd, junk, sizeof(junk), 0);
    check("ftruncate para 10", ftruncate(fd, 10) == 0 && fstat(fd, &st) == 0 && st.st_size == 10);
    check("ftruncate de volta para 9000", ftruncate(fd, 9000) == 0 && fstat(fd, &st) == 0 && st.st_size == 9000);
    check("a parte estendida le zeros", all_zero(fd, 10, 9000));
    close(fd);
    check("truncate por caminho", truncate(h, 3) == 0 && stat(h, &st) == 0 && st.st_size == 3);
    fd = open(h, O_RDONLY);
    check("ftruncate em descritor so de leitura da EINVAL", fd >= 0 && err_is(ftruncate(fd, 0), EINVAL));
    if (fd >= 0)
        close(fd);
    unlink(h);
}

static void test_errors(void)
{
    check("abrir diretorio para escrita da EISDIR", err_is(open(B(), O_WRONLY), EISDIR));
    check("O_CREAT com barra no fim da EISDIR", err_is(open(P("vt-n/"), O_CREAT | O_WRONLY, 0644), EISDIR));
    check("mkdir de diretorio existente da EEXIST", err_is(mkdir(B(), 0755), EEXIST));
    check("criar dentro de /dev da EPERM", err_is(open("/dev/vt", O_CREAT | O_WRONLY, 0644), EPERM));
    check("O_DIRECTORY em arquivo da ENOTDIR", err_is(open("/bin/hello", O_RDONLY | O_DIRECTORY), ENOTDIR));
    check("unlink de diretorio da EISDIR", err_is(unlink("/bin"), EISDIR));
    check("rmdir de arquivo da ENOTDIR", err_is(rmdir("/bin/hello"), ENOTDIR));
    check("O_CREAT num diretorio existente da EISDIR", err_is(open("/bin", O_CREAT | O_RDONLY, 0644), EISDIR));
    check("unlink de . da EISDIR", err_is(unlink("/bin/."), EISDIR));
    check("rmdir de . da EINVAL", err_is(rmdir("/bin/."), EINVAL));
    check("rename de .. da EBUSY", err_is(rename("/bin/..", "/bin/x"), EBUSY));
}

static int count_entries(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
            n++;
    closedir(d);
    return n;
}

static int has_entry(const char *dir, const char *name)
{
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    int found = 0;
    struct dirent *e;
    while ((e = readdir(d)))
        if (strcmp(e->d_name, name) == 0)
            found = 1;
    closedir(d);
    return found;
}

static void test_review(void)
{
    char a[512], ab[512];
    strcpy(a, P("vt-da"));
    strcpy(ab, P("vt-da/b"));
    mkdir(a, 0755);
    mkdir(ab, 0755);
    int ok = chdir(ab) == 0 && rmdir(ab) == 0 && rmdir(a) == 0;
    check("rmdir do cwd e do pai dele", ok);
    check(".. a partir de cwd apagado da ENOENT", err_is(chdir(".."), ENOENT));
    char cwd[256];
    check("getcwd em cwd apagado da ENOENT", getcwd(cwd, sizeof(cwd)) == NULL && errno == ENOENT);
    chdir("/");

    char r1[512], r2[512];
    strcpy(r1, P("vt-r1"));
    strcpy(r2, P("vt-r2"));
    mkdir(r1, 0755);
    ok = chdir(r1) == 0 && rename(r1, r2) == 0 && getcwd(cwd, sizeof(cwd)) && strcmp(cwd, r2) == 0;
    check("getcwd acompanha o rename do cwd", ok);
    chdir("/");
    rmdir(r2);

    put(P("vt-caso"), "C");
    check("rename que so muda maiusculas", rename(P("vt-caso"), P("VT-CASO")) == 0 && has_entry(B(), "VT-CASO") &&
                                               !has_entry(B(), "vt-caso"));
    unlink(P("VT-CASO"));

    char log[512];
    strcpy(log, P("vt-log"));
    unlink(log);
    pid_t kids[2];
    for (int k = 0; k < 2; k++) {
        kids[k] = fork();
        if (kids[k] == 0) {
            int fd = open(log, O_CREAT | O_WRONLY | O_APPEND, 0644);
            for (int i = 0; i < 200 && fd >= 0; i++)
                write(fd, k ? "bbbbbbbbb\n" : "aaaaaaaaa\n", 10);
            _exit(fd >= 0 ? 0 : 1);
        }
    }
    int st0 = 1, st1 = 1;
    waitpid(kids[0], &st0, 0);
    waitpid(kids[1], &st1, 0);
    struct stat st;
    check("O_APPEND em dois processos nao perde escrita",
          st0 == 0 && st1 == 0 && stat(log, &st) == 0 && st.st_size == 4000);
    unlink(log);

    char dd[512];
    strcpy(dd, P("vt-dd"));
    mkdir(dd, 0755);
    for (int i = 0; i < 60; i++) {
        char nm[600];
        snprintf(nm, sizeof(nm), "%s/arquivo-numero-%02d", dd, i);
        put(nm, "x");
    }
    check("60 arquivos criados", count_entries(dd) == 60);
    DIR *d = opendir(dd);
    int removed = 0;
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char nm[600];
        snprintf(nm, sizeof(nm), "%s/%s", dd, e->d_name);
        if (unlink(nm) == 0)
            removed++;
    }
    if (d)
        closedir(d);
    check("apagar durante o readdir nao pula entradas", removed == 60 && count_entries(dd) == 0);
    check("rmdir do diretorio esvaziado", rmdir(dd) == 0);

    char m1[512], m2[512];
    strcpy(m1, P("vt-modo"));
    strcpy(m2, P("vt-modo2"));
    put(m1, "abc");
    int ro = open(m1, O_RDONLY), wo = open(m1, O_WRONLY);
    char c;
    check("write em descritor so de leitura da EBADF", ro >= 0 && err_is(write(ro, "x", 1), EBADF));
    check("read em descritor so de escrita da EBADF", wo >= 0 && err_is(read(wo, &c, 1), EBADF));
    struct stat s1, s2;
    ok = fstat(ro, &s1) == 0 && rename(m1, m2) == 0 && stat(m2, &s2) == 0 && s1.st_ino == s2.st_ino;
    check("st_ino nao muda no rename", ok);
    close(ro);
    close(wo);
    unlink(m2);

    char g[512];
    strcpy(g, P("vt-fch"));
    mkdir(g, 0755);
    int gfd = open(g, O_RDONLY | O_DIRECTORY);
    ok = gfd >= 0 && rmdir(g) == 0 && fchdir(gfd) == 0;
    check("fchdir para diretorio apagado funciona", ok);
    check("getcwd depois disso da ENOENT", getcwd(cwd, sizeof(cwd)) == NULL && errno == ENOENT);
    chdir("/");
    if (gfd >= 0)
        close(gfd);

    struct stat sb, sp;
    if (stat(B(), &sb) == 0 && stat(P(".."), &sp) == 0 && sb.st_dev != sp.st_dev) {
        int made = 0;
        for (; made < 2000; made++) {
            char nm[600];
            snprintf(nm, sizeof(nm), "%s/C%04d", base, made);
            int fd = open(nm, O_CREAT | O_WRONLY | O_EXCL, 0644);
            if (fd < 0)
                break;
            close(fd);
        }
        int full = errno == ENOSPC;
        check("raiz fixa do FAT enche com ENOSPC", made > 10 && full);
        ok = rename(P("C0001"), P("C0000")) == 0 && err_is(stat(P("C0001"), &sb), ENOENT);
        check("rename por cima com a raiz cheia", ok);
        for (int i = 0; i < made; i++) {
            char nm[600];
            snprintf(nm, sizeof(nm), "%s/C%04d", base, i);
            unlink(nm);
        }
        check("raiz esvaziada de novo", err_is(stat(P("C0000"), &sb), ENOENT));
    }
}

int main(int argc, char **argv)
{
    printf("\n");
    int readonly = argc > 1 && strcmp(argv[1], "-n") == 0;
    if (argc < 2 + readonly) {
        fprintf(stderr, "uso: vfstest [-n] <diretorio gravavel>\n");
        return 2;
    }
    strncpy(base, argv[1 + readonly], sizeof(base) - 1);
    size_t n = strlen(base);
    while (n > 1 && base[n - 1] == '/')
        base[--n] = 0;
    if (strcmp(base, "/") == 0)
        base[0] = 0;

    printf("vfstest: montagens e ..\n");
    test_mount_crossing();
    if (readonly) {
        printf("vfstest: erros\n");
        test_errors();
        printf("vfstest: %s (%d falha(s))\n", fails ? "FALHOU" : "TUDO OK", fails);
        return fails ? 1 : 0;
    }
    printf("vfstest: devfs montado em %s/vt-m\n", base);
    test_devfs_mount();
    printf("vfstest: vnode compartilhado e unlink com arquivo aberto\n");
    test_shared_vnode();
    printf("vfstest: rename\n");
    test_rename();
    printf("vfstest: cwd\n");
    test_cwd();
    printf("vfstest: buracos e truncate\n");
    test_holes();
    printf("vfstest: erros\n");
    test_errors();
    printf("vfstest: correcoes da revisao\n");
    test_review();
    printf("vfstest: %s (%d falha(s))\n", fails ? "FALHOU" : "TUDO OK", fails);
    return fails ? 1 : 0;
}
