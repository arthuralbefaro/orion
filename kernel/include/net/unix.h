#pragma once

#include <kernel/compiler.h>

#define AF_UNIX       1
#define UNIX_PATH_MAX 108

struct sockaddr_un {
    uint16_t sun_family;
    char sun_path[UNIX_PATH_MAX];
};

struct file;

bool unix_is_file(const struct file *f);
long unix_socket(int type, int flags);
long unix_socketpair(int type, int flags, int *usv);
long unix_bind(struct file *f, const void *uaddr, uint32_t len);
long unix_listen(struct file *f, int backlog);
long unix_connect(struct file *f, const void *uaddr, uint32_t len);
long unix_accept(struct file *f, void *uaddr, uint32_t *ulen, int flags);
long unix_shutdown(struct file *f, int how);
long unix_getsockname(struct file *f, void *uaddr, uint32_t *ulen);
long unix_getpeername(struct file *f, void *uaddr, uint32_t *ulen);
