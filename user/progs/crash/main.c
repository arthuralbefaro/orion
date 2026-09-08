#include <stdio.h>

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    puts("crash: vou escrever em memoria do kernel...");
    *(volatile int *)0xffffffff80000000 = 1;
    puts("crash: se voce ve isto, a protecao falhou!");
    return 0;
}
