#define _GNU_SOURCE
#include "auth.h"

#include <crypt.h>
#include <fcntl.h>
#include <sys/stat.h>

#define SHADOW     "/etc/shadow"
#define SHADOW_NEW "/etc/shadow.novo"
#define MIN_LEN    4

static const char salt_chars[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

static int make_salt(char *out)
{
    unsigned char r[16];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t got = read(fd, r, sizeof(r));
    close(fd);
    if (got != (ssize_t)sizeof(r))
        return -1;
    memcpy(out, "$6$", 3);
    for (int i = 0; i < 16; i++)
        out[3 + i] = salt_chars[r[i] % 64];
    out[19] = '$';
    out[20] = 0;
    return 0;
}

static int rewrite(const char *user, const char *hash)
{
    FILE *in = fopen(SHADOW, "r");
    if (!in)
        return -1;
    struct stat st;
    if (fstat(fileno(in), &st) < 0) {
        fclose(in);
        return -1;
    }
    int fd = open(SHADOW_NEW, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0 && (fchown(fd, st.st_uid, st.st_gid) < 0 || fchmod(fd, st.st_mode & 07777) < 0)) {
        close(fd);
        unlink(SHADOW_NEW);
        fd = -1;
    }
    FILE *out = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (!out) {
        if (fd >= 0)
            close(fd);
        fclose(in);
        return -1;
    }
    char line[512];
    size_t ul = strlen(user);
    int found = 0;
    while (fgets(line, sizeof(line), in)) {
        if (!found && strncmp(line, user, ul) == 0 && line[ul] == ':') {
            char *rest = strchr(line + ul + 1, ':');
            fprintf(out, "%s:%s%s", user, hash, rest ? rest : ":::::::\n");
            found = 1;
        } else {
            fputs(line, out);
        }
    }
    fclose(in);
    if (!found)
        fprintf(out, "%s:%s:20000:0:99999:7:::\n", user, hash);
    if (fflush(out) != 0 || fsync(fd) != 0 || fclose(out) != 0) {
        unlink(SHADOW_NEW);
        return -1;
    }
    return rename(SHADOW_NEW, SHADOW);
}

int passwd_main(int argc, char **argv)
{
    uid_t caller = getuid();
    const char *user = NULL;
    if (argc > 2) {
        fprintf(stderr, "uso: passwd [usuario]\n");
        return 2;
    }
    struct passwd *pw = argc > 1 ? getpwnam(argv[1]) : getpwuid(caller);
    if (!pw) {
        fprintf(stderr, "passwd: usuario %s nao existe\n", argc > 1 ? argv[1] : "atual");
        return 1;
    }
    if (caller != 0 && pw->pw_uid != caller) {
        fprintf(stderr, "passwd: so o root troca a senha de outro usuario\n");
        return 1;
    }
    char name[64];
    snprintf(name, sizeof(name), "%s", pw->pw_name);
    user = name;
    char old[128], a[128], b[128];
    if (caller != 0) {
        if (auth_read_line("Senha atual: ", old, sizeof(old), 1) < 0)
            return 1;
        int ok = auth_check(user, old);
        memset(old, 0, sizeof(old));
        if (!ok) {
            sleep(1);
            fprintf(stderr, "passwd: senha atual incorreta\n");
            return 1;
        }
    }
    if (auth_read_line("Senha nova: ", a, sizeof(a), 1) < 0 ||
        auth_read_line("Repita a senha nova: ", b, sizeof(b), 1) < 0)
        return 1;
    int same = strcmp(a, b) == 0;
    memset(b, 0, sizeof(b));
    if (!same) {
        memset(a, 0, sizeof(a));
        fprintf(stderr, "passwd: as senhas nao conferem\n");
        return 1;
    }
    if (strlen(a) < MIN_LEN) {
        memset(a, 0, sizeof(a));
        fprintf(stderr, "passwd: senha curta demais (minimo %d)\n", MIN_LEN);
        return 1;
    }
    char salt[24];
    if (make_salt(salt) < 0) {
        memset(a, 0, sizeof(a));
        fprintf(stderr, "passwd: sem /dev/urandom\n");
        return 1;
    }
    const char *hash = crypt(a, salt);
    memset(a, 0, sizeof(a));
    if (!hash || hash[0] != '$') {
        fprintf(stderr, "passwd: crypt falhou\n");
        return 1;
    }
    if (rewrite(user, hash) < 0) {
        fprintf(stderr, "passwd: gravando %s: %s\n", SHADOW, strerror(errno));
        return 1;
    }
    printf("passwd: senha de %s trocada\n", user);
    return 0;
}
