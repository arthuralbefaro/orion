#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    printf("hello from ring 3! pid %d (pai %d), argc %d\n", getpid(), getppid(), argc);
    for (int i = 0; i < argc; i++)
        printf("  argv[%d] = %s\n", i, argv[i]);
    return 42;
}
