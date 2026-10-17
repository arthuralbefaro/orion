#pragma once

extern int errno;

#define EPERM   1
#define EINTR   4
#define ENOENT  2
#define ESRCH   3
#define ENOEXEC 8
#define E2BIG   7
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
#define EIO     5
#define EEXIST  17
#define ENOTEMPTY 39
#define ENAMETOOLONG 36
#define EROFS   30
#define ENOSPC  28

const char *strerror(int err);
