#include <termios.h>
#include <sys/ioctl.h>

int tcgetattr(int fd, struct termios *t)
{
    return ioctl(fd, TCGETS, t);
}

int tcsetattr(int fd, int actions, const struct termios *t)
{
    (void)actions;
    return ioctl(fd, TCSETS, (void *)t);
}

void cfmakeraw(struct termios *t)
{
    t->c_lflag &= ~(unsigned)(ICANON | ECHO);
}
