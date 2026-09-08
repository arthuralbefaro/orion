#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv);
char **environ;

void _start_c(long *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    environ = argv + argc + 1;
    exit(main(argc, argv));
}

__attribute__((naked)) void _start(void)
{
    __asm__ volatile(
        "movq %rsp, %rdi\n\t"
        "andq $-16, %rsp\n\t"
        "call _start_c\n\t"
        "ud2");
}
