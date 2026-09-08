#pragma once

#include <stddef.h>
#include <stdarg.h>
#include <sys/types.h>

#define EOF (-1)
#define BUFSIZ 512

typedef struct FILE {
    int fd;
    int eof;
    int err;
    unsigned char buf[BUFSIZ];
    int rpos, rlen;
} FILE;

extern FILE *stdin, *stdout, *stderr;

FILE  *fopen(const char *path, const char *mode);
FILE  *fdopen(int fd, const char *mode);
int    fclose(FILE *f);
int    fflush(FILE *f);
size_t fread(void *buf, size_t size, size_t n, FILE *f);
size_t fwrite(const void *buf, size_t size, size_t n, FILE *f);
int    fgetc(FILE *f);
int    getc(FILE *f);
int    getchar(void);
char  *fgets(char *buf, int size, FILE *f);
int    fputc(int c, FILE *f);
int    putc(int c, FILE *f);
int    putchar(int c);
int    fputs(const char *s, FILE *f);
int    puts(const char *s);
int    feof(FILE *f);
int    ferror(FILE *f);
int    fileno(FILE *f);
int    fseek(FILE *f, long off, int whence);
long   ftell(FILE *f);
void   perror(const char *s);

int printf(const char *fmt, ...);
int fprintf(FILE *f, const char *fmt, ...);
int dprintf(int fd, const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int vdprintf(int fd, const char *fmt, va_list ap);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
