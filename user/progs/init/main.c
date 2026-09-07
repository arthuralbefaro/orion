#include <ulib.h>

int main(void)
{
    puts("hello from ring 3! sou o processo pid ");
    print_num(getpid());
    puts("\n");
    for (int i = 1; i <= 3; i++) {
        puts("init: iteracao ");
        print_num(i);
        puts(" (dormindo 500 ms)\n");
        sleep_ms(500);
    }
    puts("init: saindo com codigo 42\n");
    return 42;
}