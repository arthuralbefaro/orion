#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

static const char *P(const char *rel)
{
    static char bufs[6][600];
    static int n;
    char *b = bufs[n++ % 6];
    snprintf(b, 600, "%s/lt/%s", base, rel);
    return b;
}

static int put_file(const char *path, const char *text)
{
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0)
        return -1;
    ssize_t n = write(fd, text, strlen(text));
    close(fd);
    return n == (ssize_t)strlen(text) ? 0 : -1;
}

static int has(const char *path, const char *text)
{
    char got[128] = { 0 };
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, got, sizeof(got) - 1);
    close(fd);
    return n == (ssize_t)strlen(text) && memcmp(got, text, (size_t)n) == 0;
}

static int link_is(const char *path, const char *target)
{
    char got[600];
    ssize_t n = readlink(path, got, sizeof(got));
    return n == (ssize_t)strlen(target) && memcmp(got, target, (size_t)n) == 0;
}

static int fails_with(int r, int err)
{
    return r < 0 && errno == err;
}

static int count_entries(const char *path)
{
    DIR *d = opendir(path);
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

static int dtype_of(const char *dir, const char *name)
{
    DIR *d = opendir(dir);
    if (!d)
        return -1;
    int t = -1;
    struct dirent *e;
    while ((e = readdir(d)))
        if (strcmp(e->d_name, name) == 0)
            t = e->d_type;
    closedir(d);
    return t;
}

static void basic(void)
{
    struct stat st, sl;
    check("cria o alvo", put_file(P("alvo.txt"), "conteudo\n") == 0);
    check("symlink relativo", symlink("alvo.txt", P("rel")) == 0);
    check("readlink devolve o alvo", link_is(P("rel"), "alvo.txt"));
    check("lstat ve o link", lstat(P("rel"), &sl) == 0 && S_ISLNK(sl.st_mode) && sl.st_size == 8);
    check("stat ve o alvo", stat(P("rel"), &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 9);
    check("le pelo link", has(P("rel"), "conteudo\n"));
    check("symlink absoluto", symlink(P("alvo.txt"), P("abs")) == 0 && has(P("abs"), "conteudo\n"));
    check("link para link", symlink("rel", P("cadeia")) == 0 && has(P("cadeia"), "conteudo\n"));
    check("escreve pelo link", put_file(P("rel"), "novo\n") == 0 && has(P("alvo.txt"), "novo\n"));
    check("nome que ja existe da EEXIST", fails_with(symlink("x", P("alvo.txt")), EEXIST));
    check("alvo vazio da ENOENT", fails_with(symlink("", P("vazio")), ENOENT));
    check("readlink de arquivo comum da EINVAL", fails_with((int)readlink(P("alvo.txt"), (char[8]){ 0 }, 8), EINVAL));
    check("O_NOFOLLOW no link da ELOOP", fails_with(open(P("rel"), O_RDONLY | O_NOFOLLOW), ELOOP));
    check("barra no fim de link para arquivo da ENOTDIR", fails_with(stat(P("rel/"), &st), ENOTDIR));
    check("tipo DT_LNK no readdir", dtype_of(P(""), "rel") == DT_LNK);
}

static void dirs(void)
{
    struct stat st, sp;
    check("cria sub/fundo", mkdir(P("sub"), 0755) == 0 && mkdir(P("sub/fundo"), 0755) == 0 &&
                                put_file(P("sub/fundo/dentro.txt"), "dentro\n") == 0);
    check("link para diretorio", symlink("sub/fundo", P("dl")) == 0);
    check("atravessa o link no meio do caminho", has(P("dl/dentro.txt"), "dentro\n"));
    check("stat do link e diretorio, lstat e link",
          stat(P("dl"), &st) == 0 && S_ISDIR(st.st_mode) && lstat(P("dl"), &st) == 0 && S_ISLNK(st.st_mode));
    check("barra no fim segue o link", stat(P("dl/"), &st) == 0 && S_ISDIR(st.st_mode));
    check(".. depois do link vai ao pai real", stat(P("dl/.."), &st) == 0 && stat(P("sub"), &sp) == 0 &&
                                                   st.st_ino == sp.st_ino);
    check("rmdir no link da ENOTDIR", fails_with(rmdir(P("dl")), ENOTDIR));
    char before[512], cwd[512];
    check("chdir pelo link", getcwd(before, sizeof(before)) != NULL && chdir(P("dl")) == 0);
    const char *end = "/lt/sub/fundo";
    size_t n = getcwd(cwd, sizeof(cwd)) ? strlen(cwd) : 0;
    check("getcwd mostra o caminho real", n >= strlen(end) && strcmp(cwd + n - strlen(end), end) == 0);
    check("caminho relativo dentro do link", has("dentro.txt", "dentro\n"));
    check("volta ao diretorio anterior", chdir(before) == 0);
    check("cria arquivo atraves do link de diretorio", put_file(P("dl/novo.txt"), "x") == 0 &&
                                                       has(P("sub/fundo/novo.txt"), "x"));
}

static void hard(void)
{
    struct stat st;
    check("laco de links", symlink("laco2", P("laco1")) == 0 && symlink("laco1", P("laco2")) == 0);
    check("abrir o laco da ELOOP", fails_with(open(P("laco1"), O_RDONLY), ELOOP));
    check("stat do laco da ELOOP", fails_with(stat(P("laco1"), &st), ELOOP));
    check("lstat do laco funciona", lstat(P("laco1"), &st) == 0 && S_ISLNK(st.st_mode));
    check("link pendurado", symlink("nao-existe", P("solto")) == 0);
    check("stat do pendurado da ENOENT", fails_with(stat(P("solto"), &st), ENOENT));
    check("O_EXCL no link da EEXIST", fails_with(open(P("solto"), O_CREAT | O_EXCL | O_WRONLY, 0644), EEXIST));
    check("O_CREAT pelo pendurado cria o alvo", put_file(P("solto"), "criado\n") == 0 && has(P("nao-existe"), "criado\n"));

    char name[64], target[64];
    int ok = 1;
    for (int i = 0; i < 41 && ok; i++) {
        snprintf(name, sizeof(name), "c%02d", i);
        if (i == 40)
            snprintf(target, sizeof(target), "alvo.txt");
        else
            snprintf(target, sizeof(target), "c%02d", i + 1);
        ok = symlink(target, P(name)) == 0;
    }
    check("cria 41 links em cadeia", ok);
    check("40 saltos ainda funcionam", has(P("c01"), "novo\n"));
    check("41 saltos dao ELOOP", fails_with(open(P("c00"), O_RDONLY), ELOOP));

    char longer[300];
    strcpy(longer, "");
    while (strlen(longer) < 180)
        strcat(longer, "./");
    strcat(longer, "alvo.txt");
    check("alvo longo (fora do inode no ext2)", symlink(longer, P("longo")) == 0 && link_is(P("longo"), longer) &&
                                                has(P("longo"), "novo\n"));
    check("rename do link", rename(P("abs"), P("abs2")) == 0 && link_is(P("abs2"), P("alvo.txt")));
    check("unlink apaga o link e nao o alvo", unlink(P("rel")) == 0 && lstat(P("rel"), &st) < 0 &&
                                              has(P("alvo.txt"), "novo\n"));
    check("unlink do link de diretorio", unlink(P("dl")) == 0 && stat(P("sub/fundo"), &st) == 0);
}

static void cleanup(void)
{
    static const char *const names[] = { "abs2", "cadeia", "laco1", "laco2", "solto", "nao-existe", "longo",
                                         "alvo.txt", "sub/fundo/dentro.txt", "sub/fundo/novo.txt", NULL };
    for (int i = 0; names[i]; i++)
        unlink(P(names[i]));
    char name[16];
    for (int i = 0; i < 41; i++) {
        snprintf(name, sizeof(name), "c%02d", i);
        unlink(P(name));
    }
    rmdir(P("sub/fundo"));
    rmdir(P("sub"));
    check("diretorio de teste ficou vazio", count_entries(P("")) == 0);
    check("apaga o diretorio de teste", rmdir(P("")) == 0);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "uso: linktest <diretorio>\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    snprintf(base, sizeof(base), "%s", argv[1]);
    size_t n = strlen(base);
    while (n > 1 && base[n - 1] == '/')
        base[--n] = '\0';
    printf("\nlinktest: links simbolicos em %s\n", base);
    int before = count_entries(base);
    char dir[300];
    snprintf(dir, sizeof(dir), "%s/lt", base);
    if (mkdir(dir, 0755) < 0) {
        printf("linktest: nao criou %s (errno %d)\n", dir, errno);
        return 1;
    }
    if (symlink("x", P("sonda")) < 0) {
        printf("linktest: %s nao aceita links (errno %d)\n", base, errno);
        rmdir(dir);
        return 1;
    }
    unlink(P("sonda"));
    basic();
    dirs();
    hard();
    cleanup();
    check("raiz do teste voltou ao que era", count_entries(base) == before);
    printf("linktest: %s (%d falha(s))\n", fails ? "FALHOU" : "TUDO OK", fails);
    return fails ? 1 : 0;
}
