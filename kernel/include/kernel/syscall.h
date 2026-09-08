#pragma once

#include <kernel/compiler.h>

#define SYS_READ          0
#define SYS_POLL          7
#define SYS_FCNTL         72
#define SYS_RT_SIGACTION  13
#define SYS_RT_SIGPROCMASK 14
#define SYS_RT_SIGRETURN  15
#define SYS_ALARM         37
#define SYS_KILL          62
#define SYS_SETPGID       109
#define SYS_SOCKET        41
#define SYS_CONNECT       42
#define SYS_SENDTO        44
#define SYS_RECVFROM      45
#define SYS_BIND          49
#define SYS_GETPGID       121
#define SYS_RENAME        82
#define SYS_MKDIR         83
#define SYS_RMDIR         84
#define SYS_UNLINK        87
#define SYS_FSYNC         74
#define SYS_FTRUNCATE     77
#define SYS_SYNC          162
#define SYS_WRITE         1
#define SYS_OPEN          2
#define SYS_CLOSE         3
#define SYS_STAT          4
#define SYS_FSTAT         5
#define SYS_LSEEK         8
#define SYS_MMAP          9
#define SYS_MUNMAP        11
#define SYS_BRK           12
#define SYS_IOCTL         16
#define SYS_PIPE          22
#define SYS_SCHED_YIELD   24
#define SYS_DUP           32
#define SYS_DUP2          33
#define SYS_NANOSLEEP     35
#define SYS_GETPID        39
#define SYS_FORK          57
#define SYS_EXECVE        59
#define SYS_EXIT          60
#define SYS_WAIT4         61
#define SYS_GETCWD        79
#define SYS_CHDIR         80
#define SYS_GETPPID       110
#define SYS_ARCH_PRCTL    158
#define SYS_CLOCK_GETTIME 228
#define SYS_READDIR       1000
#define SYS_OPENPTY       1001

#define WNOHANG 1

#define EPERM   1
#define ENOENT  2
#define ESRCH   3
#define EBADF   9
#define ECHILD  10
#define EAGAIN  11
#define ENOMEM  12
#define EACCES  13
#define EFAULT  14
#define ENOTDIR 20
#define EISDIR  21
#define EINVAL  22
#define EMFILE  24
#define ENOTTY  25
#define ESPIPE  29
#define EPIPE   32
#define ERANGE  34
#define ENOSYS  38
#define ENOEXEC 8
#define ENODEV  19
#define EINTR   4
#define EIO     5
#define EEXIST  17
#define ENOTEMPTY 39
#define EROFS   30
#define ENOSPC  28
#define EMSGSIZE 90
#define EAFNOSUPPORT 97
#define EADDRINUSE 98
#define ENETUNREACH 101
#define ECONNREFUSED 111
#define EHOSTUNREACH 113
#define ETIMEDOUT 110
#define EPROTONOSUPPORT 93
#define ENOTSOCK 88
#define EDESTADDRREQ 89

#define USER_ADDR_MAX 0x0000800000000000ull
#define USER_MMAP_BASE 0x0000100000000000ull

#define POLLIN   0x0001
#define POLLPRI  0x0002
#define POLLOUT  0x0004
#define POLLERR  0x0008
#define POLLHUP  0x0010
#define POLLNVAL 0x0020

struct user_pollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
};

#define O_RDONLY   0
#define O_WRONLY   1
#define O_RDWR     2
#define O_CREAT    0x40
#define O_TRUNC    0x200
#define O_APPEND   0x400
#define O_NONBLOCK 0x800

#define F_GETFL 3
#define F_SETFL 4

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20

#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

#define S_IFMT  0170000
#define S_IFDIR 0040000
#define S_IFREG 0100000
#define S_IFCHR 0020000
#define S_IFBLK 0060000
#define S_IFIFO 0010000

struct user_dirent {
    uint64_t size;
    uint8_t is_dir;
    char name[256];
};

struct user_stat {
    uint64_t st_size;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint64_t st_ino;
    uint64_t st_blksize;
    uint64_t st_mtime;
};

struct user_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

void syscall_init(void);
void syscall_init_cpu(void);
