#include <kernel/cred.h>
#include <kernel/process.h>
#include <kernel/spinlock.h>
#include <kernel/syscall.h>
#include <lib/string.h>
#include <mm/uaccess.h>

#define KEEP 0xFFFFFFFFu

void cred_get(struct cred *out)
{
    struct process *p = process_current();
    if (!p) {
        memset(out, 0, sizeof(*out));
        return;
    }
    uint64_t flags = spin_lock_irqsave(&p->cred_lock);
    *out = p->cred;
    spin_unlock_irqrestore(&p->cred_lock, flags);
}

bool cred_member(const struct cred *c, uint32_t gid)
{
    if (gid == c->egid)
        return true;
    for (uint32_t i = 0; i < c->ngroups; i++)
        if (c->groups[i] == gid)
            return true;
    return false;
}

bool cred_is_root(void)
{
    struct cred c;
    cred_get(&c);
    return c.euid == 0;
}

void cred_exec(uint32_t mode, uint32_t uid, uint32_t gid)
{
    struct process *p = process_current();
    uint64_t flags = spin_lock_irqsave(&p->cred_lock);
    if (mode & S_ISUID)
        p->cred.euid = uid;
    if ((mode & S_ISGID) && (mode & S_IXGRP))
        p->cred.egid = gid;
    p->cred.suid = p->cred.euid;
    p->cred.sgid = p->cred.egid;
    spin_unlock_irqrestore(&p->cred_lock, flags);
}

long sys_getuid(void)
{
    struct cred c;
    cred_get(&c);
    return c.uid;
}

long sys_geteuid(void)
{
    struct cred c;
    cred_get(&c);
    return c.euid;
}

long sys_getgid(void)
{
    struct cred c;
    cred_get(&c);
    return c.gid;
}

long sys_getegid(void)
{
    struct cred c;
    cred_get(&c);
    return c.egid;
}

static bool one_of(uint32_t v, uint32_t a, uint32_t b, uint32_t c)
{
    return v == a || v == b || v == c;
}

static long set_ids(bool user, uint32_t r, uint32_t e, uint32_t s)
{
    struct process *p = process_current();
    uint64_t flags = spin_lock_irqsave(&p->cred_lock);
    struct cred *c = &p->cred;
    uint32_t *cr = user ? &c->uid : &c->gid;
    uint32_t *ce = user ? &c->euid : &c->egid;
    uint32_t *cs = user ? &c->suid : &c->sgid;
    bool root = c->euid == 0;
    long ret = 0;
    if (!root && ((r != KEEP && !one_of(r, *cr, *ce, *cs)) || (e != KEEP && !one_of(e, *cr, *ce, *cs)) ||
                  (s != KEEP && !one_of(s, *cr, *ce, *cs))))
        ret = -EPERM;
    if (ret == 0) {
        if (r != KEEP)
            *cr = r;
        if (e != KEEP)
            *ce = e;
        if (s != KEEP)
            *cs = s;
    }
    spin_unlock_irqrestore(&p->cred_lock, flags);
    return ret;
}

static long set_one(bool user, uint32_t id)
{
    if (id == KEEP)
        return -EINVAL;
    struct process *p = process_current();
    uint64_t flags = spin_lock_irqsave(&p->cred_lock);
    struct cred *c = &p->cred;
    uint32_t *cr = user ? &c->uid : &c->gid;
    uint32_t *ce = user ? &c->euid : &c->egid;
    uint32_t *cs = user ? &c->suid : &c->sgid;
    long ret = 0;
    if (c->euid == 0)
        *cr = *ce = *cs = id;
    else if (id == *cr || id == *cs)
        *ce = id;
    else
        ret = -EPERM;
    spin_unlock_irqrestore(&p->cred_lock, flags);
    return ret;
}

long sys_setuid(uint32_t uid)
{
    return set_one(true, uid);
}

long sys_setgid(uint32_t gid)
{
    return set_one(false, gid);
}

static long set_re(bool user, uint32_t r, uint32_t e)
{
    struct cred c;
    cred_get(&c);
    uint32_t old_r = user ? c.uid : c.gid;
    uint32_t old_e = user ? c.euid : c.egid;
    uint32_t old_s = user ? c.suid : c.sgid;
    if (c.euid != 0) {
        if (r != KEEP && r != old_r && r != old_e)
            return -EPERM;
        if (e != KEEP && !one_of(e, old_r, old_e, old_s))
            return -EPERM;
    }
    uint32_t new_e = e != KEEP ? e : old_e;
    uint32_t s = (r != KEEP || (e != KEEP && e != old_r)) ? new_e : KEEP;
    struct process *p = process_current();
    uint64_t flags = spin_lock_irqsave(&p->cred_lock);
    uint32_t *cr = user ? &p->cred.uid : &p->cred.gid;
    uint32_t *ce = user ? &p->cred.euid : &p->cred.egid;
    uint32_t *cs = user ? &p->cred.suid : &p->cred.sgid;
    if (r != KEEP)
        *cr = r;
    if (e != KEEP)
        *ce = e;
    if (s != KEEP)
        *cs = s;
    spin_unlock_irqrestore(&p->cred_lock, flags);
    return 0;
}

long sys_setreuid(uint32_t r, uint32_t e)
{
    return set_re(true, r, e);
}

long sys_setregid(uint32_t r, uint32_t e)
{
    return set_re(false, r, e);
}

long sys_setresuid(uint32_t r, uint32_t e, uint32_t s)
{
    return set_ids(true, r, e, s);
}

long sys_setresgid(uint32_t r, uint32_t e, uint32_t s)
{
    return set_ids(false, r, e, s);
}

static long put_three(uint32_t *ur, uint32_t *ue, uint32_t *us, uint32_t r, uint32_t e, uint32_t s)
{
    if (copy_to_user(ur, &r, sizeof(r)) < 0 || copy_to_user(ue, &e, sizeof(e)) < 0 ||
        copy_to_user(us, &s, sizeof(s)) < 0)
        return -EFAULT;
    return 0;
}

long sys_getresuid(uint32_t *r, uint32_t *e, uint32_t *s)
{
    struct cred c;
    cred_get(&c);
    return put_three(r, e, s, c.uid, c.euid, c.suid);
}

long sys_getresgid(uint32_t *r, uint32_t *e, uint32_t *s)
{
    struct cred c;
    cred_get(&c);
    return put_three(r, e, s, c.gid, c.egid, c.sgid);
}

long sys_getgroups(int size, uint32_t *list)
{
    struct cred c;
    cred_get(&c);
    if (size < 0)
        return -EINVAL;
    if (size == 0)
        return c.ngroups;
    if ((uint32_t)size < c.ngroups)
        return -EINVAL;
    if (c.ngroups && copy_to_user(list, c.groups, c.ngroups * sizeof(uint32_t)) < 0)
        return -EFAULT;
    return c.ngroups;
}

long sys_setgroups(uint64_t size, const uint32_t *list)
{
    if (!cred_is_root())
        return -EPERM;
    if (size > CRED_GROUPS_MAX)
        return -EINVAL;
    uint32_t g[CRED_GROUPS_MAX];
    if (size && copy_from_user(g, list, size * sizeof(uint32_t)) < 0)
        return -EFAULT;
    struct process *p = process_current();
    uint64_t flags = spin_lock_irqsave(&p->cred_lock);
    memcpy(p->cred.groups, g, size * sizeof(uint32_t));
    p->cred.ngroups = (uint32_t)size;
    spin_unlock_irqrestore(&p->cred_lock, flags);
    return 0;
}
