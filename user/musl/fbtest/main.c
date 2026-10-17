#include <errno.h>
#include <fcntl.h>
#include <orion.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static void fill(uint8_t *fb, const struct fb_info *info, uint32_t x, uint32_t y,
                 uint32_t w, uint32_t h, uint32_t rgb)
{
    uint32_t pixel = fb_pack(info, rgb);
    for (uint32_t j = 0; j < h; j++) {
        uint32_t *row = (uint32_t *)(fb + (uint64_t)(y + j) * info->pitch + (uint64_t)x * 4);
        for (uint32_t i = 0; i < w; i++)
            row[i] = pixel;
    }
}

int main(int argc, char **argv)
{
    unsigned hold = argc > 1 ? (unsigned)atoi(argv[1]) : 3;

    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "fbtest: /dev/fb0: %s\n", orion_strerror(errno));
        return 1;
    }

    struct fb_info info;
    if (ioctl(fd, FBIOGET_INFO, &info) < 0) {
        fprintf(stderr, "fbtest: FBIOGET_INFO: %s\n", orion_strerror(errno));
        return 1;
    }
    fprintf(stderr, "fbtest: %ux%u, %u bpp, pitch %u, %lu bytes\n",
            info.width, info.height, info.bpp, info.pitch, info.size);

    uint8_t *fb = mmap(NULL, info.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) {
        fprintf(stderr, "fbtest: mmap: %s\n", orion_strerror(errno));
        return 1;
    }
    fprintf(stderr, "fbtest: framebuffer mapeado em %p\n", (void *)fb);

    for (uint32_t y = 0; y < info.height; y++) {
        uint32_t *row = (uint32_t *)(fb + (uint64_t)y * info.pitch);
        uint32_t g = y * 255 / (info.height ? info.height : 1);
        for (uint32_t x = 0; x < info.width; x++) {
            uint32_t r = x * 255 / (info.width ? info.width : 1);
            row[x] = fb_pack(&info, (r << 16) | (g << 8) | 0x40);
        }
    }

    uint32_t w = info.width / 3;
    uint32_t h = info.height / 3;
    fill(fb, &info, info.width / 2 - w / 2, info.height / 2 - h / 2, w, h, 0x101820);
    fill(fb, &info, info.width / 2 - w / 2 + 8, info.height / 2 - h / 2 + 8, w - 16, h - 16, 0xE6C84C);

    sleep(hold);
    fprintf(stderr, "fbtest: gradiente + retangulo desenhados (%u s na tela)\n", hold);
    munmap(fb, info.size);
    close(fd);
    return 0;
}
