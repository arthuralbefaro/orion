#include <drivers/bga.h>
#include <drivers/console.h>
#include <drivers/pci.h>
#include <arch/x86_64/cpu.h>
#include <kernel/log.h>
#include <kernel/syscall.h>
#include <mm/vmm.h>

#define DISPI_INDEX 0x01CE
#define DISPI_DATA  0x01CF

#define REG_ID          0x0
#define REG_XRES        0x1
#define REG_YRES        0x2
#define REG_BPP         0x3
#define REG_ENABLE      0x4
#define REG_VIRT_WIDTH  0x6
#define REG_VIRT_HEIGHT 0x7
#define REG_X_OFFSET    0x8
#define REG_Y_OFFSET    0x9
#define REG_VIDEO_64K   0xA

#define ENABLED     0x01
#define LFB_ENABLED 0x40

static bool present;
static uint16_t version;
static uint64_t lfb_phys;
static uint64_t vram;
static void *lfb;

static uint16_t rd(uint16_t index)
{
    outw(DISPI_INDEX, index);
    return inw(DISPI_DATA);
}

static void wr(uint16_t index, uint16_t value)
{
    outw(DISPI_INDEX, index);
    outw(DISPI_DATA, value);
}

bool bga_init(void)
{
    const struct pci_device *dev = pci_find_id(0x1234, 0x1111, 0);
    if (!dev)
        dev = pci_find_id(0x80EE, 0xBEEF, 0);
    if (!dev)
        return false;
    version = rd(REG_ID);
    if (version < 0xB0C0 || version > 0xB0C5) {
        log_warn("bga", "placa %04x:%04x sem interface DISPI (ID 0x%04x)", dev->vendor, dev->device, version);
        return false;
    }
    lfb_phys = pci_bar_address(dev, 0);
    if (!lfb_phys) {
        log_warn("bga", "placa sem BAR do framebuffer");
        return false;
    }
    uint16_t blocks = version >= 0xB0C2 ? rd(REG_VIDEO_64K) : 0;
    vram = blocks ? (uint64_t)blocks * 65536 : 16ull << 20;
    lfb = vmm_map_mmio_flags(lfb_phys, vram, PTE_PRESENT | PTE_WRITABLE | PTE_WRITETHRU | PTE_NX);
    present = true;
    log_info("bga", "Bochs/BGA %04x:%04x, DISPI 0x%04x, framebuffer em 0x%lx, %lu MiB de video", dev->vendor,
             dev->device, version, lfb_phys, vram >> 20);
    return true;
}

bool bga_present(void)
{
    return present;
}

int bga_set_mode(uint32_t width, uint32_t height)
{
    if (!present)
        return -ENODEV;
    if (width < 320 || height < 200 || width > 4096 || height > 4096 || width % 8)
        return -EINVAL;
    if ((uint64_t)width * height * 4 > vram)
        return -EINVAL;
    wr(REG_ENABLE, 0);
    wr(REG_XRES, (uint16_t)width);
    wr(REG_YRES, (uint16_t)height);
    wr(REG_BPP, 32);
    wr(REG_VIRT_WIDTH, (uint16_t)width);
    wr(REG_VIRT_HEIGHT, (uint16_t)height);
    wr(REG_X_OFFSET, 0);
    wr(REG_Y_OFFSET, 0);
    wr(REG_ENABLE, ENABLED | LFB_ENABLED);
    if (rd(REG_XRES) != width || rd(REG_YRES) != height || rd(REG_BPP) != 32) {
        log_warn("bga", "a placa nao aceitou %ux%u", width, height);
        return -EINVAL;
    }
    struct framebuffer fb = {
        .address = lfb,
        .width = width,
        .height = height,
        .pitch = (uint64_t)width * 4,
        .bpp = 32,
        .red_mask_size = 8,
        .red_mask_shift = 16,
        .green_mask_size = 8,
        .green_mask_shift = 8,
        .blue_mask_size = 8,
        .blue_mask_shift = 0,
    };
    if (!console_set_framebuffer(&fb))
        return -EIO;
    log_info("bga", "modo %ux%u, 32 bpp", width, height);
    return 0;
}
