#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    int fd = open("/proc/usb", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        perror("lsusb: /proc/usb");
        return 1;
    }
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        fwrite(buf, 1, (size_t)n, stdout);
    close(fd);
    return 0;
}
