#include <stdio.h>
#include <time.h>

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    char buf[64];
    strftime(buf, sizeof(buf), "%a %b %d %H:%M:%S UTC %Y", &tm);
    puts(buf);
    struct timespec up;
    clock_gettime(CLOCK_MONOTONIC, &up);
    printf("uptime: %ld.%03ld s\n", (long)up.tv_sec, up.tv_nsec / 1000000);
    return 0;
}
