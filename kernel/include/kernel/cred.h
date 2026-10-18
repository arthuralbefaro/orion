#pragma once

#include <kernel/compiler.h>

#define CRED_GROUPS_MAX 32

struct cred {
    uint32_t uid;
    uint32_t euid;
    uint32_t suid;
    uint32_t gid;
    uint32_t egid;
    uint32_t sgid;
    uint32_t ngroups;
    uint32_t groups[CRED_GROUPS_MAX];
};

void cred_get(struct cred *out);
bool cred_member(const struct cred *c, uint32_t gid);
bool cred_is_root(void);
void cred_exec(uint32_t mode, uint32_t uid, uint32_t gid);

long sys_getuid(void);
long sys_geteuid(void);
long sys_getgid(void);
long sys_getegid(void);
long sys_setuid(uint32_t uid);
long sys_setgid(uint32_t gid);
long sys_setreuid(uint32_t r, uint32_t e);
long sys_setregid(uint32_t r, uint32_t e);
long sys_setresuid(uint32_t r, uint32_t e, uint32_t s);
long sys_setresgid(uint32_t r, uint32_t e, uint32_t s);
long sys_getresuid(uint32_t *r, uint32_t *e, uint32_t *s);
long sys_getresgid(uint32_t *r, uint32_t *e, uint32_t *s);
long sys_getgroups(int size, uint32_t *list);
long sys_setgroups(uint64_t size, const uint32_t *list);
