#pragma once

#include <sys/types.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

ssize_t read(int fd, void *buf, size_t len);
ssize_t write(int fd, const void *buf, size_t len);
int     close(int fd);
off_t   lseek(int fd, off_t off, int whence);
int     pipe(int fds[2]);
int     dup(int fd);
int     dup2(int oldfd, int newfd);
pid_t   fork(void);
int     execve(const char *path, char *const argv[], char *const envp[]);
int     execv(const char *path, char *const argv[]);
pid_t   getpid(void);
pid_t   getppid(void);
int     chdir(const char *path);
char   *getcwd(char *buf, size_t size);
int     isatty(int fd);
unsigned sleep(unsigned seconds);
int     usleep(unsigned long usec);
void   *sbrk(long increment);
int     brk(void *addr);
void    _exit(int code) __attribute__((noreturn));
void    sched_yield(void);

extern char **environ;
