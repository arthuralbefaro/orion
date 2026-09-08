#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;

static void check(const char *what, int ok)
{
    printf("  %-42s %s\n", what, ok ? "ok" : "FALHA");
    if (!ok)
        fails++;
}

static int write_file(const char *path, const char *data)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return -1;
    long n = write(fd, data, strlen(data));
    close(fd);
    return n == (long)strlen(data) ? 0 : -1;
}

static long read_file(const char *path, char *buf, long max)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    long n = read(fd, buf, max - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = '\0';
    return n;
}

static const char *P(const char *rel);

static void cleanup(void)
{
    unlink(P("/orion-t1.txt"));
    unlink(P("/orion-t2.txt"));
    unlink(P("/orion-big.bin"));
    unlink(P("/orion-d/dentro.txt"));
    rmdir(P("/orion-d"));
}

static char base[256] = "";

static const char *P(const char *rel)
{
    static char out[320];
    snprintf(out, sizeof(out), "%s%s", base, rel);
    return out;
}

int main(int argc, char **argv)
{
    int heavy = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-a") == 0)
            heavy = 1;
        else
            snprintf(base, sizeof(base), "%s", argv[i]);
    }
    char buf[512];

    cleanup();

    check("criar e escrever um arquivo", write_file(P("/orion-t1.txt"), "alo mundo\n") == 0);
    check("ler de volta o que escreveu",
          read_file(P("/orion-t1.txt"), buf, sizeof(buf)) == 10 && strcmp(buf, "alo mundo\n") == 0);

    struct stat st;
    check("stat mostra o tamanho certo", stat(P("/orion-t1.txt"), &st) == 0 && st.st_size == 10);

    int fd = open(P("/orion-t1.txt"), O_WRONLY | O_APPEND);
    check("abrir para append", fd >= 0);
    if (fd >= 0) {
        write(fd, "segunda\n", 8);
        close(fd);
    }
    check("append somou ao arquivo",
          read_file(P("/orion-t1.txt"), buf, sizeof(buf)) == 18 &&
          strcmp(buf, "alo mundo\nsegunda\n") == 0);

    fd = open(P("/orion-t1.txt"), O_WRONLY);
    if (fd >= 0) {
        ftruncate(fd, 4);
        close(fd);
    }
    check("truncate corta o arquivo",
          read_file(P("/orion-t1.txt"), buf, sizeof(buf)) == 4 && strcmp(buf, "alo ") == 0);

    char oldname[320];
    snprintf(oldname, sizeof(oldname), "%s", P("/orion-t1.txt"));
    check("rename", rename(oldname, P("/orion-t2.txt")) == 0);
    check("nome antigo sumiu", stat(P("/orion-t1.txt"), &st) < 0);
    check("nome novo existe", stat(P("/orion-t2.txt"), &st) == 0 && st.st_size == 4);

    check("mkdir", mkdir(P("/orion-d"), 0755) == 0);
    check("mkdir de novo da EEXIST", mkdir(P("/orion-d"), 0755) < 0 && errno == EEXIST);
    check("arquivo dentro do diretorio", write_file(P("/orion-d/dentro.txt"), "conteudo\n") == 0);
    check("ler o arquivo de dentro",
          read_file(P("/orion-d/dentro.txt"), buf, sizeof(buf)) == 9);
    check("rmdir de diretorio cheio falha", rmdir(P("/orion-d")) < 0);

    DIR *d = opendir(P("/orion-d"));
    int seen = 0;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)))
            if (strcmp(e->d_name, "dentro.txt") == 0)
                seen = 1;
        closedir(d);
    }
    check("readdir encontra o arquivo criado", seen);

    check("unlink", unlink(P("/orion-d/dentro.txt")) == 0);
    check("rmdir de diretorio vazio", rmdir(P("/orion-d")) == 0);
    check("unlink de inexistente da ENOENT",
          unlink(P("/orion-nao-existe")) < 0 && errno == ENOENT);

    if (heavy) {
        printf("  criando 300 arquivos...\n");
        int ok = mkdir(P("/orion-d"), 0755) == 0;
        int made = 0;
        for (int i = 0; ok && i < 300; i++) {
            char path[64];
            snprintf(path, sizeof(path), "%s/orion-d/arquivo-numero-%03d.txt", base, i);
            ok = write_file(path, "x\n") == 0;
            if (ok)
                made++;
        }
        printf("  criou %d arquivos\n", made);
        check("criou 300 arquivos com nome longo", made == 300);

        int count = 0;
        d = opendir(P("/orion-d"));
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)))
                count++;
            closedir(d);
        }
        check("readdir devolve todos de volta", count == made);

        for (int i = 0; i < made; i++) {
            char path[64];
            snprintf(path, sizeof(path), "%s/orion-d/arquivo-numero-%03d.txt", base, i);
            unlink(path);
        }
        check("apagou os arquivos", rmdir(P("/orion-d")) == 0);

        printf("  gravando 3 MiB...\n");
        fd = open(P("/orion-big.bin"), O_WRONLY | O_CREAT | O_TRUNC);
        ok = fd >= 0;
        char *block = malloc(8192);
        memset(block, 'Z', 8192);
        for (int i = 0; ok && i < 384; i++)
            ok = write(fd, block, 8192) == 8192;
        close(fd);
        free(block);
        check("escreveu 3 MiB", ok);
        check("tamanho de 3 MiB",
              stat(P("/orion-big.bin"), &st) == 0 && st.st_size == 3 * 1024 * 1024);
        unlink(P("/orion-big.bin"));
    }

    sync();
    cleanup();
    printf("fstest: %s\n", fails == 0 ? "TUDO OK" : "FALHOU");
    return fails ? 1 : 0;
}
