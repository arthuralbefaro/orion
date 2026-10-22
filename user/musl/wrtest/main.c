#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "uso: wrtest <origem> <destino> <bytes-por-escrita>\n");
        return 2;
    }
    size_t chunk = (size_t)strtoul(argv[3], NULL, 10);
    if (chunk == 0 || chunk > 4 * 1024 * 1024) {
        fprintf(stderr, "wrtest: tamanho invalido\n");
        return 2;
    }
    char *buf = malloc(chunk);
    int in = open(argv[1], O_RDONLY);
    int out = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (!buf || in < 0 || out < 0) {
        perror("wrtest");
        return 1;
    }
    double t0 = now();
    unsigned long long total = 0;
    ssize_t n;
    while ((n = read(in, buf, chunk)) > 0) {
        if (write(out, buf, (size_t)n) != n) {
            perror("wrtest: write");
            return 1;
        }
        total += (unsigned long long)n;
    }
    close(out);
    close(in);
    double dt = now() - t0;
    printf("wrtest: %llu bytes em escritas de %zu bytes: %.1f s, %.0f KiB/s\n", total, chunk, dt,
           dt > 0 ? total / 1024.0 / dt : 0.0);
    return 0;
}
