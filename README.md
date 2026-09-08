# Orion

Sistema operacional x86_64 escrito do zero em C (gnu11), bootado pelo Limine.
Kernel com SMP, memoria virtual, processos, sinais, filas de espera, FAT com
leitura e escrita, e um compositor de janelas em espaco de usuario.

## Requisitos (Windows)
`.\setup.ps1` instala tudo via winget: LLVM (clang + ld.lld), QEMU e Git,
e baixa o Limine 12.8.0 para `third_party\limine`.

## Uso
    .\build.ps1                    # compila (incremental)
    .\build.ps1 -Run -Smp 4        # compila e roda no QEMU (UEFI, 4 CPUs)
    .\build.ps1 -Run -Smp 4 -Disk  # idem, com um segundo disco cru de 64 MiB
    .\build.ps1 -Clean             # limpa
    .\check.ps1                    # so analise sintatica (erros e warnings)
    .\regressao.ps1                # bateria de testes automatica (exit 0 = verde)

No QEMU a serial aparece no terminal. A entrada da serial e do teclado vao para
o mesmo shell; `Ctrl+K` alterna entre o shell de usuario (`/bin/sh`) e o shell
de diagnostico do kernel (`orion>`), que tem `help`.

### Shell
    ls /bin | grep test | sort | wc     pipelines de ate 8 estagios
    echo oi > a.txt                     redireciona a saida (>, >>, <, 2>)
    setas, Home, End, Delete            edicao da linha
    setas cima/baixo                    historico (ultimos 50), builtin `history`
    Ctrl+C                              interrompe o programa em primeiro plano
    Ctrl+L                              limpa a tela
    Ctrl+D                              sai
    builtins: cd, pwd, sync, clear, history, exit, help

### Interface grafica
No `/bin/sh`, o comando `wm` inicia o compositor: uma janela de terminal com o
`/bin/sh` dentro (via PTY), um relogio e uma janela "Sobre".

    Ctrl+T       novo terminal
    Ctrl+Q       fecha a janela focada
    Ctrl+Alt+X   encerra o wm e devolve o teclado ao TTY

Arraste pela barra de titulo para mover; clique para focar; o `x` fecha.
O terminal grafico entende as sequencias ANSI de cursor, apagamento e cores
(16 cores), o que faz o `edit` e o historico do `sh` funcionarem igual ao TTY.

### Disco de escrita
O `out\esp` que o QEMU expoe com `fat:rw:` **nao** aguenta escrita de verdade
(o driver vvfat do proprio QEMU aborta). Para exercitar o FAT com escrita use um
disco cru:

    .\build.ps1 -Run -Smp 4 -Disk    # cria out\disk.img (64 MiB, zerado)
    mkfs /dev/sdb ORION              # formata em FAT16, dentro do Orion
    Ctrl+K -> mount -> Ctrl+K        # monta em /mnt
    fstest -a /mnt                   # bateria completa de escrita

### Testes sem janela grafica
`test.ps1` roda o QEMU com `-display none`, serial em TCP 4444 e monitor em
TCP 4445. Ele envia comandos ao shell, injeta teclado e mouse pelo monitor,
tira screenshots (PPM convertido para PNG) e salva o log da serial.

    .\test.ps1 -Smp 4 -Commands @("ls /bin | wc","memtest")
    .\test.ps1 -Smp 4 -Screenshot out\shots\tela.ppm
    .\test.ps1 -Smp 4 -Disk -Script @("cmd:mkfs /dev/sdb","wait:10")

Passos aceitos em `-Script`: `cmd:<linha>` (envia ao shell), `raw:<texto>`,
`key:<hex>` (um byte cru; `key:0B` e o Ctrl+K), `mon:<comando do monitor>`,
`wait:<segundos>`, `shot:<arquivo.ppm>`.

`regressao.ps1` roda tres sequencias e falha se algum marcador esperado sumir:
A (texto), B (grafica, com screenshots) e C (disco cru, escrita e shell).

## Programas em /bin
    sh       shell com pipelines, redirecionamentos, historico e edicao de linha
    init     primeiro processo, mantem o sh vivo
    ls cat echo wc date sleep hello         utilitarios basicos
    cp mv rm mkdir touch head tail          arquivos
    grep sort uniq tee                      texto
    edit     editor de tela cheia (Ctrl+S salva, Ctrl+X sai)
    mkfs     formata um disco em FAT16 (ex.: mkfs /dev/sdb ORION)
    memtest  auto-teste de mmap, malloc, stat, snprintf
    fstest   auto-teste do sistema de arquivos (-a inclui 300 arquivos e 3 MiB)
    polltest auto-teste de poll, pipes e O_NONBLOCK
    sigtest  auto-teste de sinais, mascaras e alarm
    ptytest  abre um pty, roda o sh nele e injeta comandos
    crash    escreve em memoria do kernel (testa a protecao de ring 3)
    fbtest   pinta um gradiente e um retangulo em /dev/fb0
    evtest   imprime eventos de /dev/input por alguns segundos
    wm       compositor de janelas

## Dispositivos em /dev
    /dev/fb0        framebuffer: ioctl FBIOGET_INFO / FBIOREDRAW, mmap
    /dev/input      eventos de teclado e mouse (captura exclusiva enquanto aberto)
    /dev/tty        terminal fisico (serial + console)
    /dev/sda /dev/sdb ...   discos e particoes, leitura e escrita por deslocamento

## Chamadas de sistema
Numeros do Linux onde existe equivalente: read, write, open, close, stat, fstat,
poll, lseek, mmap, munmap, brk, ioctl, pipe, dup, dup2, nanosleep, getpid, fork,
execve, exit, wait4, getcwd, chdir, getppid, arch_prctl, clock_gettime, fcntl,
kill, rt_sigaction, rt_sigprocmask, rt_sigreturn, alarm, setpgid, getpgid,
rename, mkdir, rmdir, unlink, fsync, ftruncate, sync; e dois proprios:
readdir (1000) e openpty (1001).

## Layout
    kernel/include/   headers (kernel/, lib/, drivers/, arch/x86_64/, mm/, fs/, boot/)
    kernel/src/       fontes, mesma divisao
    user/lib/         libc propria (headers padrao) + fonte 8x8
    user/progs/       um diretorio por programa
    third_party/      Limine (bootloader) - baixado pelo setup.ps1
    linker.ld         layout do ELF (higher half, 0xffffffff80000000)
    limine.conf       configuracao do bootloader
    docs/             decisoes de arquitetura, relatorios e diario do turno
    build/ out/       gerados (out/esp = pasta que o QEMU boota como disco FAT)

## Toolchain
clang/lld como cross-compiler (`--target=x86_64-unknown-none-elf`), sem libc,
sem SSE/x87 no kernel, red zone desligada, modelo de codigo "kernel".
