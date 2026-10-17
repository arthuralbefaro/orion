#include <drivers/fbdev.h>
#include <drivers/console.h>
#include <kernel/file.h>
#include <kernel/process.h>
#include <kernel/syscall.h>
#include <lib/string.h>
#include <mm/memory.h>
#include <mm/vmm.h>
#include <mm/uaccess.h>

static uint64_t mapped_pages;
static uint32_t graphics_users;

bool fbdev_available(void)
{
    return console_get_framebuffer() != NULL;
}

uint64_t fbdev_size(void)
{
    const struct framebuffer *fb = console_get_framebuffer();
    return fb ? fb->pitch * fb->height : 0;
}

uint64_t fbdev_mapped_pages(void)
{
    return mapped_pages;
}

bool fbdev_get_info(struct fb_info *out)
{
    const struct framebuffer *fb = console_get_framebuffer();
    if (!fb)
        return false;
    memset(out, 0, sizeof(*out));
    out->width = (uint32_t)fb->width;
    out->height = (uint32_t)fb->height;
    out->pitch = (uint32_t)fb->pitch;
    out->bpp = fb->bpp;
    out->red_mask_size = fb->red_mask_size;
    out->red_mask_shift = fb->red_mask_shift;
    out->green_mask_size = fb->green_mask_size;
    out->green_mask_shift = fb->green_mask_shift;
    out->blue_mask_size = fb->blue_mask_size;
    out->blue_mask_shift = fb->blue_mask_shift;
    out->size = fb->pitch * fb->height;
    return true;
}

static long fb_ioctl(struct file *f, uint64_t request, uint64_t arg)
{
    (void)f;
    if (request == FBIOREDRAW) {
        console_redraw();
        return 0;
    }
    if (request != FBIOGET_INFO)
        return -EINVAL;
    struct fb_info info;
    if (!fbdev_get_info(&info))
        return -ENODEV;
    return copy_to_user((void *)arg, &info, sizeof(info));
}

static int64_t fb_mmap(struct file *f, uint64_t base, uint64_t len, int prot, uint64_t offset)
{
    const struct framebuffer *fb = console_get_framebuffer();
    if (!fb)
        return -ENODEV;

    uint64_t total = fb->pitch * fb->height;
    if (!IS_ALIGNED(offset, PAGE_SIZE) || offset >= total)
        return -EINVAL;
    uint64_t want = len < total - offset ? len : total - offset;
    uint64_t bytes = PAGE_ALIGN_UP(want);

    uint64_t phys = vmm_virt_to_phys((uint64_t)fb->address);
    if (!phys)
        return -ENODEV;
    phys = PAGE_ALIGN_DOWN(phys) + offset;

    struct process *p = process_current();

    uint64_t flags = PTE_PRESENT | PTE_USER | PTE_NX | PTE_WRITETHRU | PTE_NOFREE;
    if (prot & PROT_WRITE)
        flags |= PTE_WRITABLE;
    for (uint64_t off = 0; off < bytes; off += PAGE_SIZE)
        vmm_map_page_in(p->pml4_phys, base + off, phys + off, flags);

    mapped_pages += bytes / PAGE_SIZE;
    if (!f->priv) {
        f->priv = (void *)1;
        if (__atomic_fetch_add(&graphics_users, 1, __ATOMIC_SEQ_CST) == 0)
            console_set_graphics(true);
    }
    return 0;
}

static void fb_close(struct file *f)
{
    if (!f->priv)
        return;
    f->priv = NULL;
    if (__atomic_sub_fetch(&graphics_users, 1, __ATOMIC_SEQ_CST) == 0)
        console_set_graphics(false);
}

static const struct file_ops fb_ops = {
    .ioctl = fb_ioctl,
    .mmap = fb_mmap,
    .close = fb_close,
};

const struct file_ops *fbdev_ops(void)
{
    return &fb_ops;
}

uint32_t fbdev_graphics_users(void)
{
    return graphics_users;
}

struct file *fbdev_open(void)
{
    if (!fbdev_available())
        return NULL;
    return file_alloc(&fb_ops, NULL);
}
