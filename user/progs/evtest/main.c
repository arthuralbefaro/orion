#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/input.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static const char *type_name(uint32_t type)
{
    switch (type) {
    case INPUT_EV_KEY_DOWN:     return "tecla-desce";
    case INPUT_EV_KEY_UP:       return "tecla-sobe";
    case INPUT_EV_MOUSE_MOVE:   return "mouse-move";
    case INPUT_EV_MOUSE_BUTTON: return "mouse-botao";
    default:                    return "?";
    }
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(int argc, char **argv)
{
    long seconds = argc > 1 ? atoi(argv[1]) : 5;

    int fd = open("/dev/input", O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "evtest: /dev/input: %s\n", strerror(errno));
        return 1;
    }
    unsigned long timeout = 250;
    ioctl(fd, INPUTIOC_SETTIMEOUT, (void *)timeout);

    fprintf(stderr, "evtest: lendo /dev/input por %ld s (o TTY nao recebe teclas)\n", seconds);

    long end = now_ms() + seconds * 1000;
    long count = 0;
    while (now_ms() < end) {
        struct input_event ev[16];
        long n = read(fd, ev, sizeof(ev));
        if (n < 0) {
            fprintf(stderr, "evtest: read: %s\n", strerror(errno));
            break;
        }
        for (long i = 0; i < n / (long)sizeof(ev[0]); i++) {
            count++;
            if (ev[i].type == INPUT_EV_KEY_DOWN || ev[i].type == INPUT_EV_KEY_UP)
                fprintf(stderr, "  %-12s scancode=%02x ascii=%d mods=%x\n",
                        type_name(ev[i].type), ev[i].a, ev[i].b, ev[i].c);
            else
                fprintf(stderr, "  %-12s a=%d b=%d c=%d\n",
                        type_name(ev[i].type), ev[i].a, ev[i].b, ev[i].c);
        }
    }

    fprintf(stderr, "evtest: %ld evento(s)\n", count);
    close(fd);
    return 0;
}
