#include <ulib.h>

int main(void)
{
    puts("crash: vou escrever em memoria do kernel...\n");
    *(volatile int *)0xffffffff80000000 = 1;
    return 0;
}