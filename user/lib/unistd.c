#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <pty.h>
#include <poll.h>
#include <stdarg.h>
#include <string.h>
#include "syscall.h"

int errno;

long __syscall_ret(long r)
{
    if (r < 0 && r > -4096) {
        errno = (int)-r;
        return -1;
    }
    return r;
}

ssize_t read(int fd, void *buf, size_t len)        { return __syscall_ret(syscall3(SYS_READ, fd, (long)buf, (long)len)); }
ssize_t write(int fd, const void *buf, size_t len) { return __syscall_ret(syscall3(SYS_WRITE, fd, (long)buf, (long)len)); }
int     close(int fd)                              { return (int)__syscall_ret(syscall1(SYS_CLOSE, fd)); }
off_t   lseek(int fd, off_t off, int whence)       { return __syscall_ret(syscall3(SYS_LSEEK, fd, off, whence)); }
int     pipe(int fds[2])                           { return (int)__syscall_ret(syscall1(SYS_PIPE, (long)fds)); }
int     dup(int fd)                                { return (int)__syscall_ret(syscall1(SYS_DUP, fd)); }
int     dup2(int oldfd, int newfd)                 { return (int)__syscall_ret(syscall2(SYS_DUP2, oldfd, newfd)); }
pid_t   fork(void)                                 { return (pid_t)__syscall_ret(syscall0(SYS_FORK)); }
pid_t   getpid(void)                               { return (pid_t)syscall0(SYS_GETPID); }
pid_t   getppid(void)                              { return (pid_t)syscall0(SYS_GETPPID); }
int     setpgid(pid_t pid, pid_t pgid)             { return (int)__syscall_ret(syscall2(SYS_SETPGID, pid, pgid)); }
pid_t   getpgid(pid_t pid)                         { return (pid_t)__syscall_ret(syscall1(SYS_GETPGID, pid)); }
pid_t   getpgrp(void)                              { return getpgid(0); }
int     chdir(const char *path)                    { return (int)__syscall_ret(syscall1(SYS_CHDIR, (long)path)); }
void    sched_yield(void)                          { syscall0(SYS_SCHED_YIELD); }
int     stat(const char *path, struct stat *st)    { return (int)__syscall_ret(syscall2(SYS_STAT, (long)path, (long)st)); }
int     fstat(int fd, struct stat *st)             { return (int)__syscall_ret(syscall2(SYS_FSTAT, fd, (long)st)); }
int     munmap(void *addr, size_t len)             { return (int)__syscall_ret(syscall2(SYS_MUNMAP, (long)addr, (long)len)); }
int     clock_gettime(int clk, struct timespec *ts){ return (int)__syscall_ret(syscall2(SYS_CLOCK_GETTIME, clk, (long)ts)); }
int     ioctl(int fd, unsigned long req, void *arg) { return (int)__syscall_ret(syscall3(SYS_IOCTL, fd, (long)req, (long)arg)); }
int     poll(struct pollfd *fds, nfds_t n, int timeout) { return (int)__syscall_ret(syscall3(SYS_POLL, (long)fds, (long)n, timeout)); }

int fcntl(int fd, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    long arg = va_arg(ap, long);
    va_end(ap);
    return (int)__syscall_ret(syscall3(SYS_FCNTL, fd, cmd, arg));
}

int execve(const char *path, char *const argv[], char *const envp[])
{
    return (int)__syscall_ret(syscall3(SYS_EXECVE, (long)path, (long)argv, (long)envp));
}

int execv(const char *path, char *const argv[])
{
    return execve(path, argv, environ);
}

int open(const char *path, int flags, ...)
{
    return (int)__syscall_ret(syscall2(SYS_OPEN, (long)path, flags));
}

int  unlink(const char *path)                { return (int)__syscall_ret(syscall1(SYS_UNLINK, (long)path)); }
int  rmdir(const char *path)                 { return (int)__syscall_ret(syscall1(SYS_RMDIR, (long)path)); }
int  rename(const char *a, const char *b)    { return (int)__syscall_ret(syscall2(SYS_RENAME, (long)a, (long)b)); }
int  mkdir(const char *path, unsigned mode)  { (void)mode; return (int)__syscall_ret(syscall1(SYS_MKDIR, (long)path)); }
int  ftruncate(int fd, off_t len)            { return (int)__syscall_ret(syscall2(SYS_FTRUNCATE, fd, len)); }
int  fsync(int fd)                           { return (int)__syscall_ret(syscall1(SYS_FSYNC, fd)); }
void sync(void)                              { syscall0(SYS_SYNC); }
int  remove(const char *path)                { return unlink(path); }

pid_t waitpid(pid_t pid, int *status, int options)
{
    return (pid_t)__syscall_ret(syscall3(SYS_WAIT4, pid, (long)status, options));
}

int openpty(int *master, int *slave)
{
    int fds[2];
    if (__syscall_ret(syscall1(SYS_OPENPTY, (long)fds)) < 0)
        return -1;
    *master = fds[0];
    *slave = fds[1];
    return 0;
}

pid_t wait(int *status)
{
    return waitpid(-1, status, 0);
}

char *getcwd(char *buf, size_t size)
{
    long r = syscall2(SYS_GETCWD, (long)buf, (long)size);
    if (r < 0) {
        errno = (int)-r;
        return NULL;
    }
    return buf;
}

int isatty(int fd)
{
    long r = syscall2(SYS_IOCTL, fd, 0);
    if (r < 0) {
        errno = (int)-r;
        return 0;
    }
    return 1;
}

int nanosleep(const struct timespec *req, struct timespec *rem)
{
    (void)rem;
    return (int)__syscall_ret(syscall1(SYS_NANOSLEEP, (long)req));
}

unsigned sleep(unsigned seconds)
{
    struct timespec ts = { (time_t)seconds, 0 };
    nanosleep(&ts, NULL);
    return 0;
}

int usleep(unsigned long usec)
{
    struct timespec ts = { (time_t)(usec / 1000000), (long)(usec % 1000000) * 1000 };
    return nanosleep(&ts, NULL);
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    long r = syscall6(SYS_MMAP, (long)addr, (long)len, prot, flags, fd, off);
    if (r < 0 && r > -4096) {
        errno = (int)-r;
        return MAP_FAILED;
    }
    return (void *)r;
}

int brk(void *addr)
{
    long r = syscall1(SYS_BRK, (long)addr);
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return 0;
}

void *sbrk(long increment)
{
    long cur = syscall1(SYS_BRK, 0);
    if (increment == 0)
        return (void *)cur;
    long r = syscall1(SYS_BRK, cur + increment);
    if (r < 0) {
        errno = (int)-r;
        return (void *)-1;
    }
    return (void *)cur;
}

void _exit(int code)
{
    syscall1(SYS_EXIT, code);
    for (;;)
        ;
}

static const char *const errors[] = {
    [0] = "Sucesso", [EPERM] = "Operacao nao permitida", [ENOENT] = "Arquivo ou diretorio inexistente",
    [ESRCH] = "Processo inexistente", [ENOEXEC] = "Formato de executavel invalido",
    [EBADF] = "Descritor de arquivo invalido", [ECHILD] = "Sem processos filhos",
    [EAGAIN] = "Tente novamente", [ENOMEM] = "Sem memoria", [EACCES] = "Permissao negada",
    [EFAULT] = "Endereco invalido", [ENOTDIR] = "Nao e um diretorio", [EISDIR] = "E um diretorio",
    [EINVAL] = "Argumento invalido", [EMFILE] = "Muitos arquivos abertos", [ENOTTY] = "Nao e um terminal",
    [ESPIPE] = "Seek invalido", [EPIPE] = "Pipe quebrado", [ERANGE] = "Fora do intervalo",
    [ENOSYS] = "Funcao nao implementada", [EIO] = "Erro de entrada/saida",
    [EEXIST] = "Arquivo ja existe", [ENOTEMPTY] = "Diretorio nao vazio",
    [EROFS] = "Sistema de arquivos somente leitura", [EINTR] = "Chamada interrompida",
    [ENAMETOOLONG] = "Nome muito longo",
};

const char *strerror(int err)
{
    if (err >= 0 && err < (int)(sizeof(errors) / sizeof(errors[0])) && errors[err])
        return errors[err];
    return "Erro desconhecido";
}
