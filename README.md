# Orion

Sistema operacional x86_64 escrito do zero em C (gnu11), bootado pelo Limine.
Kernel com SMP, memoria virtual com areas (VMAs) e paginas sob demanda, fork
copy-on-write, threads, futex, sinais no formato do Linux, VFS com vnodes e
montagens (`mount`/`umount`; FAT com leitura e escrita na raiz e em `/mnt`,
devfs em `/dev`, tmpfs em `/tmp` e `/run`), rede IPv4 (UDP/ICMP) e um compositor de janelas em espaco de
usuario.

Desde a versao 2.0 o espaco de usuario e Linux: os programas sao binarios
estaticos x86_64 ligados contra a musl, e o Orion roda software de terceiros
sem mudar uma linha dele: `dash`, `lua`, o `sbase`, o `tcc` (que compila C
dentro do proprio sistema) e o Doom.

## Requisitos (Windows)
`.\setup.ps1` instala via winget o LLVM (clang + ld.lld), o QEMU, o Git e o
Python 3, e baixa o Limine 12.8.0 para `third_party\limine` e o Zig 0.16.0
(que traz a musl) para `third_party\zig`. O primeiro build que tem o `doom`
baixa o Freedoom (`tools\freedoom.ps1`). Todos os downloads tem o SHA-256
conferido; versoes e origens estao em `third_party\LICENSES.md`.

## Uso
    .\build.ps1                    # compila (incremental)
    .\build.ps1 -Run -Smp 4        # compila e roda no QEMU (UEFI, 4 CPUs)
    .\build.ps1 -Run -Smp 4 -Disk  # idem, com um segundo disco cru de 64 MiB
    .\build.ps1 -Run -Smp 4 -Gui   # boota direto na interface grafica
    .\build.ps1 -Clean             # limpa
    .\check.ps1                    # analise do kernel e dos programas (erros e warnings)
    .\regressao.ps1                # bateria automatica (exit 0 = verde)

No QEMU a serial aparece no terminal. A entrada da serial e do teclado vao para
o mesmo shell; `Ctrl+K` alterna entre o shell de usuario (`/bin/sh`) e o shell
de diagnostico do kernel (`orion>`), que tem `help`.

### Shell (`/bin/sh`)
    ls /bin | grep test | sort | wc     pipelines de ate 8 estagios
    echo oi > a.txt                     redireciona a saida (>, >>, <, 2>)
    cmd1 ; cmd2   cmd1 && cmd2   cmd1 || cmd2
    cmd &   jobs   fg   wait            trabalhos em segundo plano
    VAR=x cmd                           variavel so para o comando
    echo $? $NOME ${NOME}               status do ultimo comando e expansao
    ulimit -a / -n / -u / -s            limites do processo
    sh -c 'linha'                       o que o system() e o popen() da musl usam
    setas, Home, End, Delete            edicao da linha; setas cima/baixo: historico
    Ctrl+C / Ctrl+L / Ctrl+D            interrompe / limpa a tela / sai
    builtins: cd, pwd, sync, clear, history, export, unset, jobs, fg, wait, ulimit, exit, help

Para um shell POSIX completo (aspas, funcoes, `case`, `while`, here-doc...)
use o `dash`.

### Software portado
    dash        shell POSIX (dash 0.5.13.5)
    lua         Lua 5.4.9 (lua -e 'print(2^10)', os.execute, io.popen)
    tcc         TinyCC: tcc /lib/tcc/hello.c -o /oi && /oi
    doom        doom -iwad /doom/freedoom1.wad -nogui   (Ctrl atira, espaco usa, setas andam)
    sbase-box   97 utilitarios do sbase; 29 deles tambem em /bin (find, xargs, seq, tr, cut,
                expr, od, printf, test, ...); os outros: sbase-box <nome> ...

As fontes estao em `third_party/` como vieram. O que o `configure`/`make`
deles geraria sai de geradores em `tools/` (`dash_gen.py`, `sbase_box.py`,
`tcc_gen.py`), chamados pelo `build.txt` de cada programa.

### Interface grafica
No `/bin/sh`, o comando `wm` inicia o compositor: uma janela de terminal com o
`/bin/sh` dentro (via PTY), um relogio e uma janela "Sobre".

    Ctrl+T       novo terminal
    Ctrl+Q       fecha a janela focada
    Ctrl+Alt+X   encerra o wm e devolve o teclado ao TTY
    Ctrl+P       janela de processos
    Ctrl+F       navegador de arquivos

Arraste pela barra de titulo para mover; clique para focar; o `x` fecha.
O terminal grafico entende as sequencias ANSI de cursor, apagamento e cores.

### Boot direto na interface grafica
O `init` le `/etc/autostart` no boot: uma linha por comando, `#` e comentario, e
`shell <caminho>` troca o programa que ele mantem vivo no terminal. O padrao e
`shell /bin/sh`; `.\build.ps1 -Gui` grava um autostart que roda `/bin/dhcp` e
sobe direto no `wm`.

### Disco de escrita
O `out\esp` que o QEMU expoe com `fat:rw:` aguenta pouca escrita (o vvfat do
QEMU). Para exercitar o FAT de verdade use um disco cru:

    .\build.ps1 -Run -Smp 4 -Disk    # cria out\disk.img (64 MiB, zerado)
    mkfs /dev/sdb ORION              # formata em FAT16, dentro do Orion
    mount /dev/sdb /mnt              # monta em /mnt (ou Ctrl+K -> mount -> Ctrl+K)
    fstest -a /mnt                   # bateria completa de escrita
    vfstest /mnt                     # montagens, .., rename, arquivo apagado e aberto
    umount /mnt                      # EBUSY se houver arquivo aberto ou cwd dentro
    mount -r /dev/sdb /mnt           # so leitura

### Testes
`test.ps1` roda o QEMU com `-display none`, serial em TCP 4444 e monitor em
TCP 4445. Ele envia comandos ao shell, injeta teclado e mouse pelo monitor,
tira screenshots (PPM convertido para PNG) e salva o log da serial.

    .\test.ps1 -Smp 4 -Commands @("ls /bin | wc","memtest")
    .\test.ps1 -Smp 4 -Screenshot out\shots\tela.ppm
    .\test.ps1 -Smp 4 -Disk -Script @("cmd:mkfs /dev/sdb","wait:10")

Passos aceitos em `-Script`: `cmd:<linha>`, `raw:<texto>`, `key:<hex>` (um
byte cru; `key:0B` e o Ctrl+K), `keys:<hex>`, `mon:<comando do monitor>`,
`wait:<segundos>`, `until:<segundos>:<texto>` (espera o texto aparecer depois
do ultimo comando) e `shot:<arquivo.ppm>`.

`regressao.ps1` roda cinco sequencias e falha se algum marcador esperado sumir:
A (texto, rede, musl, threads, dash, lua, tcc), B (grafica e Doom, com
screenshots), C (disco cru e escrita), D (boot direto na interface grafica) e
E (estresse: quatro `stress 3000 16` em pipeline com 4 CPUs).

    .\regressao.ps1 -OnlyA          # so uma sequencia (OnlyA..OnlyE)
    .\regressao.ps1 -NoBuild -OnlyE -StressRuns 5    # o estresse 5 vezes seguidas
    python tools\regress.py ABCDE   # a mesma regressao em Python (Linux e CI)

## Programas proprios em /bin
    sh init edit wm top ps free uptime time kill env printenv clear
    ls cat echo wc date sleep hello cp mv rm mkdir touch head tail grep sort uniq tee
    ifconfig ping nslookup dhcp mkfs mount umount
    testes: memtest fstest vfstest tmptest polltest forktest ptytest limtest stress sctest filetest
            auxtest fputest badptr crash fbtest evtest
    testes da musl: mhello mthreads mfutex sigtest

Sao programas da musl como os portados (`user/musl/<nome>`); o que e proprio do
Orion (framebuffer, `/dev/input`, pty, informacoes de processo e de rede) esta
em `user/include/orion.h`.

### Rede
O QEMU sobe uma e1000 com a rede de usuario (SLIRP): 10.0.2.15 para o Orion,
10.0.2.2 de gateway e 10.0.2.3 de DNS.

    dhcp                      pega endereco, mascara, gateway e DNS
    ifconfig                  mostra a interface e os contadores da placa
    ping 10.0.2.2 4           ICMP echo
    nslookup example.com      consulta A no DNS

A pilha faz ARP, IPv4, ICMP e UDP; sockets `SOCK_DGRAM` e `SOCK_RAW`. TCP vem
na fase 3. O `test.ps1` grava o trafego em `out\net.pcap`.

## Dispositivos em /dev
    /dev/fb0        framebuffer: ioctl FBIOGET_INFO / FBIOREDRAW, mmap
    /dev/input      eventos de teclado (todas as teclas, inclusive modificadoras) e mouse
    /dev/tty        terminal fisico (serial + console)
    /dev/sda /dev/sdb ...   discos e particoes

## Chamadas de sistema
Numeros e estruturas do Linux x86_64: arquivos (`openat` e os `*at`,
`getdents64`, `stat` de 144 bytes), memoria (`mmap` com arquivo e
`MAP_FIXED`, `mprotect`, `madvise`, `mremap`), processos e threads (`clone`,
`futex`, `exit_group`, `set_tid_address`), sinais com 64 sinais, `SA_RESTART`
e quadro `rt_sigframe`, tempo (`clock_gettime` com todos os relogios,
`clock_nanosleep`, `setitimer`, `getrusage`). A lista completa, com o que e
parcial, esta em `docs/SYSCALLS.md`. Quatro numeros sao proprios do Orion:
readdir (1000), openpty (1001), sysinfo (1002) e proclist (1003).

## Layout
    kernel/include/   headers (kernel/, lib/, drivers/, arch/x86_64/, mm/, fs/, boot/)
    kernel/src/       fontes, mesma divisao
    user/musl/        um diretorio por programa (main.c e/ou build.txt)
    user/include/     orion.h: a interface propria do Orion para os programas
    third_party/      Limine, dash, lua, sbase, doomgeneric, tinycc (como vieram);
                      Zig e Freedoom sao baixados (fora do git)
    tools/            Zig, Freedoom e Python; geradores de build; harness em Python
    linker.ld         layout do ELF do kernel (higher half, 0xffffffff80000000)
    limine.conf       configuracao do bootloader
    docs/             plano, decisoes, chamadas de sistema, diario e relatorios
    build/ out/       gerados (out/esp = pasta que o QEMU boota como disco FAT)

## Toolchain
Kernel: clang/lld como cross-compiler (`--target=x86_64-unknown-none-elf`), sem
libc, sem SSE/x87 no kernel, red zone desligada, modelo de codigo "kernel".
Programas: `zig cc -target x86_64-linux-musl -static`.
