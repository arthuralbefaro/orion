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
#define SYS_SENDMSG       46
#define SYS_SELECT        23
#define SYS_ACCEPT        43
#define SYS_SHUTDOWN      48
#define SYS_LISTEN        50
#define SYS_GETSOCKNAME   51
#define SYS_GETPEERNAME   52
#define SYS_SETSOCKOPT    54
#define SYS_GETSOCKOPT    55
#define SYS_ACCEPT4       288
#define SYS_SOCKETPAIR    53
#define SYS_RECVMSG       47
#define SYS_GETPGID       121
#define SYS_RENAME        82
#define SYS_MKDIR         83
#define SYS_RMDIR         84
#define SYS_UNLINK        87
#define SYS_FSYNC         74
#define SYS_FTRUNCATE     77
#define SYS_SYNC          162
#define SYS_TRUNCATE      76
#define SYS_PREAD64       17
#define SYS_PWRITE64      18
#define SYS_FCHDIR        81
#define SYS_MOUNT         165
#define SYS_UMOUNT2       166
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
#define SYS_GETRLIMIT     97
#define SYS_LSTAT         6
#define SYS_ACCESS        21
#define SYS_SYMLINK       88
#define SYS_READLINK      89
#define SYS_GETDENTS64    217
#define SYS_OPENAT        257
#define SYS_MKDIRAT       258
#define SYS_NEWFSTATAT    262
#define SYS_UNLINKAT      263
#define SYS_RENAMEAT      264
#define SYS_SYMLINKAT     266
#define SYS_READLINKAT    267
#define SYS_CHMOD         90
#define SYS_FCHMOD        91
#define SYS_CHOWN         92
#define SYS_FCHOWN        93
#define SYS_LCHOWN        94
#define SYS_UMASK         95
#define SYS_UTIME         132
#define SYS_UTIMES        235
#define SYS_FCHOWNAT      260
#define SYS_FUTIMESAT     261
#define SYS_FCHMODAT      268
#define SYS_UTIMENSAT     280
#define SYS_FCHMODAT2     452
#define UTIME_NOW  ((1l << 30) - 1l)
#define UTIME_OMIT ((1l << 30) - 2l)
#define SYS_FACCESSAT     269
#define SYS_FACCESSAT2    439
#define SYS_MREMAP        25
#define MREMAP_MAYMOVE    1
#define MREMAP_FIXED      2
#define SYS_DUP3          292
#define SYS_PIPE2         293
#define SYS_READV         19
#define SYS_WRITEV        20
#define SYS_UNAME         63
#define SYS_GETUID        102
#define SYS_GETGID        104
#define SYS_GETEUID       107
#define SYS_GETEGID       108
#define SYS_SIGALTSTACK   131
#define SYS_GETTID        186
#define SYS_SET_TID_ADDRESS 218
#define SYS_EXIT_GROUP    231
#define SYS_CLONE         56
#define SYS_VFORK         58
#define SYS_PAUSE         34
#define CLONE_VM             0x00000100
#define CLONE_FS             0x00000200
#define CLONE_FILES          0x00000400
#define CLONE_SIGHAND        0x00000800
#define CLONE_VFORK          0x00004000
#define CLONE_THREAD         0x00010000
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID   0x01000000
#define SYS_FUTEX         202
#define SYS_GETITIMER     36
#define SYS_SETITIMER     38
#define SYS_GETTIMEOFDAY  96
#define SYS_GETRUSAGE     98
#define SYS_TIMES         100
#define SYS_RT_SIGPENDING 127
#define SYS_RT_SIGTIMEDWAIT 128
#define SYS_RT_SIGSUSPEND 130
#define SYS_TKILL         200
#define SYS_TIME          201
#define SYS_CLOCK_GETRES  229
#define SYS_CLOCK_NANOSLEEP 230
#define SYS_TGKILL        234
#define SYS_RESTART_SYSCALL 219
#define SYS_SCHED_GETAFFINITY 204
#define SYS_SETRLIMIT     160
#define SYS_PRLIMIT64     302
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
#define E2BIG   7
#define ENODEV  19
#define EINTR   4
#define EIO     5
#define EEXIST  17
#define ENOTBLK 15
#define EBUSY   16
#define EXDEV   18
#define EFBIG   27
#define ENOTEMPTY 39
#define ENAMETOOLONG 36
#define ELOOP   40
#define EROFS   30
#define ENOSPC  28
#define EMSGSIZE 90
#define EAFNOSUPPORT 97
#define EADDRINUSE 98
#define ENETUNREACH 101
#define ECONNREFUSED 111
#define ECONNRESET 104
#define ENOBUFS 105
#define EISCONN 106
#define ENOTCONN 107
#define EADDRNOTAVAIL 99
#define EALREADY 114
#define EINPROGRESS 115
#define ENOPROTOOPT 92
#define ENXIO 6
#define EHOSTUNREACH 113
#define ETIMEDOUT 110
#define EOPNOTSUPP 95
#define ERESTARTSYS    512
#define ERESTARTNOINTR 513
#define ERESTARTNOHAND 514
#define ERESTART_RESTARTBLOCK 516
#define EPROTONOSUPPORT 93
#define ENOTSOCK 88
#define EDESTADDRREQ 89

#define USER_ADDR_MAX 0x0000800000000000ull
#define USER_MMAP_BASE 0x0000100000000000ull
#define USER_MMAP_END  0x0000700000000000ull

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
#define O_ACCMODE  3
#define O_EXCL     0x80
#define O_DIRECTORY 0x10000
#define O_NOFOLLOW 0x20000
#define O_CLOEXEC  0x80000

#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_DUPFD_CLOEXEC 1030
#define FD_CLOEXEC 1

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define MAP_TYPE      0x0f
#define MAP_FIXED     0x10
#define MAP_FIXED_NOREPLACE 0x100000
#define PROT_NONE     0
#define MADV_DONTNEED 4
#define SYS_MPROTECT  10
#define SYS_MADVISE   28

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
#define S_IFLNK 0120000
#define S_IFSOCK 0140000
#define S_ISUID 04000
#define S_ISGID 02000
#define S_ISVTX 01000
#define S_IXGRP 00010

struct user_dirent {
    uint64_t size;
    uint8_t is_dir;
    char name[256];
};

struct user_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    int64_t  st_atime;
    int64_t  st_atime_nsec;
    int64_t  st_mtime;
    int64_t  st_mtime_nsec;
    int64_t  st_ctime;
    int64_t  st_ctime_nsec;
    int64_t  unused[3];
};

struct linux_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];
};

#define DT_UNKNOWN 0
#define DT_CHR     2
#define DT_DIR     4
#define DT_FIFO 1
#define DT_LNK 10
#define DT_SOCK 12
#define DT_BLK     6
#define DT_REG     8

#define AT_FDCWD            (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR        0x200
#define MS_RDONLY           0x1
#define MS_NOSUID           0x2
#define MS_NODEV            0x4
#define MS_NOEXEC           0x8
#define MS_SYNCHRONOUS      0x10
#define MS_SILENT           0x8000
#define MS_NOATIME          0x400
#define MS_NODIRATIME       0x800
#define MS_RELATIME         0x200000
#define MS_MGC_MSK          0xFFFF0000u
#define MS_MGC_VAL          0xC0ED0000u
#define UMOUNT_NOFOLLOW     0x8
#define AT_EMPTY_PATH       0x1000
#define AT_EACCESS          0x200

struct user_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

void syscall_init(void);
void syscall_init_cpu(void);
