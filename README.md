# Orion

Sistema operacional x86_64 escrito do zero em C (gnu11), bootado pelo Limine.
Kernel com SMP, memoria virtual, processos, FAT16 em AHCI e um compositor de
janelas em espaco de usuario.

## Requisitos (Windows)
`.\setup.ps1` instala tudo via winget: LLVM (clang + ld.lld), QEMU e Git,
e baixa o Limine 12.8.0 para `third_party\limine`.

## Uso
    .\build.ps1              # compila (incremental)
    .\build.ps1 -Run -Smp 4  # compila e roda no QEMU (UEFI, 4 CPUs)
    .\build.ps1 -Clean       # limpa
    .\check.ps1              # so analise sintatica (erros e warnings)

No QEMU a serial aparece no terminal. A entrada da serial e do teclado vao para
o mesmo shell; `Ctrl+K` alterna entre o shell de usuario (`/bin/sh`) e o shell
de diagnostico do kernel (`orion>`), que tem `help`.

### Interface grafica
No `/bin/sh`, o comando `wm` inicia o compositor. Ele abre uma janela de
terminal com o `/bin/sh` rodando dentro (via PTY), um relogio e uma janela
"Sobre".

    Ctrl+T       novo terminal
    Ctrl+Q       fecha a janela focada
    Ctrl+Alt+X   encerra o wm e devolve o teclado ao TTY

Arraste pela barra de titulo para mover uma janela; clique nela para focar e
traze-la para a frente; o `x` da barra fecha.

### Testes sem janela grafica
`test.ps1` roda o QEMU com `-display none`, serial em TCP 4444 e monitor em
TCP 4445. Ele envia comandos ao shell, injeta teclado e mouse pelo monitor,
tira screenshots (PPM convertido para PNG) e salva tudo em `out\test-serial.log`.

    .\test.ps1 -Smp 4 -Commands @("ls /bin | wc","memtest")
    .\test.ps1 -Smp 4 -Screenshot out\shots\tela.ppm
    .\test.ps1 -Smp 4 -Script @("cmd:wm","wait:3","mon:sendkey l","shot:out\shots\wm.ppm")

Passos aceitos em `-Script`: `cmd:<linha>` (envia ao shell), `raw:<texto>`,
`key:<hex>` (um byte cru; `key:0B` e o Ctrl+K), `mon:<comando do monitor>`,
`wait:<segundos>`, `shot:<arquivo.ppm>`.

## Programas em /bin
    sh       shell (builtins cd, pwd, exit, help; pipe simples com |)
    init     primeiro processo, mantem o sh vivo
    ls cat echo wc date hello    utilitarios
    memtest  auto-teste de mmap, malloc, stat, snprintf
    crash    escreve em memoria do kernel (testa a protecao de ring 3)
    fbtest   pinta um gradiente e um retangulo em /dev/fb0
    evtest   imprime eventos de /dev/input por alguns segundos
    ptytest  abre um pty, roda o sh nele e injeta comandos
    wm       compositor de janelas

## Dispositivos em /dev
    /dev/fb0    framebuffer: ioctl FBIOGET_INFO / FBIOREDRAW, mmap
    /dev/input  eventos de teclado e mouse (captura exclusiva enquanto aberto)
    /dev/tty    terminal fisico (serial + console)

## Layout
    kernel/include/   headers (kernel/, lib/, drivers/, arch/x86_64/, mm/, fs/, boot/)
    kernel/src/       fontes, mesma divisao
    user/lib/         libc propria (headers padrao) + fonte 8x8
    user/progs/       um diretorio por programa
    third_party/      Limine (bootloader) - baixado pelo setup.ps1
    linker.ld         layout do ELF (higher half, 0xffffffff80000000)
    limine.conf       configuracao do bootloader
    docs/             decisoes de arquitetura e relatorios
    build/ out/       gerados (out/esp = pasta que o QEMU boota como disco FAT)

## Toolchain
clang/lld como cross-compiler (`--target=x86_64-unknown-none-elf`), sem libc,
sem SSE/x87 no kernel, red zone desligada, modelo de codigo "kernel".
