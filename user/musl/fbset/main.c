#include <errno.h>
#include <fcntl.h>
#include <orion.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int show(int fd)
{
    struct fb_info info;
    if (ioctl(fd, FBIOGET_INFO, &info) < 0) {
        fprintf(stderr, "fbset: FBIOGET_INFO: %s\n", strerror(errno));
        return 1;
    }
    printf("fbset: %ux%u, %u bpp, %u bytes por linha\n", info.width, info.height, info.bpp, info.pitch);
    return 0;
}

int main(int argc, char **argv)
{
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "fbset: /dev/fb0: %s\n", strerror(errno));
        return 1;
    }
    if (argc < 2) {
        int r = show(fd);
        close(fd);
        return r;
    }
    struct fb_mode mode;
    char x;
    if (argc != 2 || sscanf(argv[1], "%u%c%u", &mode.width, &x, &mode.height) != 3 || (x != 'x' && x != 'X')) {
        fprintf(stderr, "uso: fbset [LARGURAxALTURA]\n");
        close(fd);
        return 2;
    }
    if (ioctl(fd, FBIOSET_MODE, &mode) < 0) {
        int err = errno;
        if (err == EBUSY)
            fprintf(stderr, "fbset: %ux%u: o framebuffer esta em uso por um programa grafico\n", mode.width,
                    mode.height);
        else if (err == ENODEV)
            fprintf(stderr, "fbset: esta placa de video nao troca de modo\n");
        else
            fprintf(stderr, "fbset: %ux%u: %s\n", mode.width, mode.height, strerror(err));
        close(fd);
        return 1;
    }
    int r = show(fd);
    close(fd);
    return r;
}
