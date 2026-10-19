# Orion

Sistema operacional x86_64 escrito do zero em C (gnu11), bootado pelo Limine.
Kernel com SMP, memoria virtual com areas (VMAs) e paginas sob demanda, fork
copy-on-write, threads, futex, sinais no formato do Linux, cache de paginas
unificado (read, exec e mmap de arquivo), VFS com vnodes e
montagens (`mount`/`umount`; raiz em ext2 numa particao GPT, com a particao EFI
em `/boot/efi`, FAT e ext2 em outros discos, devfs em `/dev`, tmpfs em `/tmp` e
`/run`, procfs em `/proc`), rede IPv4 (UDP, ICMP e TCP, com loopback), sockets Unix e um compositor de janelas em
espaco de usuario.

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
    .\build.ps1 -Image            # gera out\orion.img: GPT, EFI em FAT16, raiz em ext2
    .\build.ps1 -Run -Smp 4 -Image  # boota do out\orion.img (o que voce grava fica no disco)
    .\build.ps1 -Iso              # gera out\orion.iso: ISO hibrida (CD ou pendrive, BIOS ou UEFI)
    .\build.ps1 -Run -Iso [-Bios]  # boota a ISO como CD (UEFI; -Bios usa o SeaBIOS)
    .\build.ps1 -Clean             # limpa
    .\check.ps1                    # analise do kernel e dos programas (erros e warnings)
    .\regressao.ps1                # bateria automatica (exit 0 = verde)

No QEMU a serial aparece no terminal. O boot pede usuario e senha: `root` ou
`orion`, senha `orion` nos dois (troque com `passwd`; `tools\mkpasswd.py`
gera um hash para editar o `etc\shadow` do repositorio). A entrada da serial e
do teclado vao para o mesmo shell; `Ctrl+K` alterna entre o shell de usuario (`/bin/sh`) e o shell
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
    sbase-box   97 utilitarios do sbase; 31 deles tambem em /bin (find, xargs, seq, tr, cut,
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
O `wm` tambem aceita comandos de outros programas pelo socket Unix
`/run/wm.sock`: `wmctl term` abre um terminal, `wmctl list` lista as janelas
(`procs`, `files <caminho>` e `ping` tambem existem).

Programas graficos sao processos separados: conectam no `/run/wmapp.sock`,
pedem uma janela (`janela <largura> <altura> <titulo>`), desenham num arquivo
do `/run/wm` mapeado com `MAP_SHARED` e recebem teclas e cliques pelo mesmo
socket (protocolo em `docs/DECISOES.md`, v2.4.0-a2). `hello-gui &` num
terminal grafico abre o exemplo. A `libgui` (`user/libgui`) da botoes, campos
de texto, caixas de opcao, listas com rolagem e menus, todos usaveis pelo
teclado (Tab, Enter, setas, F10); `guidemo &` mostra todos.
O terminal grafico, a barra e os titulos usam fonte vetorial (DejaVu, pelo
`stb_truetype`) com antialias; as fontes ficam em `/usr/share/fonts`. O fundo e
um papel de parede PNG (`/usr/share/wallpapers/orion.png`, gerado no build por
`tools/mkwallpaper.py`); `imginfo <arquivo>` decodifica PNG e JPEG. Apps podem
mandar so a area que mudou (`mostra x y l a`), e a `libgui` faz isso sozinha;
`cpuuso wm 10` mede o CPU do `wm` (parado, fica abaixo de 1%).

Apps graficos: `imgview <imagem>` mostra PNG e JPEG (setas trocam de imagem
na mesma pasta, `f` alterna ajustada e 1:1, `q` fecha).
`monitor` mostra CPU e memoria em graficos e os processos com o CPU de cada um,
atualizados a cada segundo; "Encerrar" manda `SIGTERM` ao processo escolhido.
`arquivos [pasta]` navega pelas pastas (Enter abre, Backspace sobe), abre imagens
no `imgview` e o resto no `editor`, cria pastas e apaga com confirmacao.
`editor <arquivo>` edita texto (setas, Home/End, PgUp/PgDn, Delete, clique
posiciona o cursor); Ctrl+S salva e o titulo marca o que nao foi salvo.
`config` mostra o sistema (versao, CPU, memoria, rede) e troca o papel de parede
na hora; a escolha fica em `/etc/wm.conf` (`fundo=<imagem>` ou `fundo=nenhum`),
que o `wm` le ao iniciar. `wmctl fundo <imagem|nenhum>` faz o mesmo sem gravar.
`terminal` e o terminal como app separado: um `sh` numa pty de 48x26 com
`COLUMNS` e `LINES` no ambiente, cores e cursor ANSI; fecha quando o `sh` sai.

Area de trabalho: a barra de tarefas embaixo lista as janelas (clique foca,
minimiza a que esta em foco ou restaura a minimizada); Alt+Tab passa pelas
janelas, inclusive as minimizadas; Alt+F9 ou o botao `_` minimiza; Alt+F10 ou
o botao de quadrado maximiza o terminal do `wm` na area de trabalho (126x33 em
1280x800) e volta ao tamanho normal. O menu "Orion" (clique ou Alt+F1, setas e
Enter) lanca os apps: terminal do `wm` ou app, arquivos, editor
(`/root/notas.txt`), monitor, imagens e configuracoes; Ctrl+P abre o `monitor`
e Ctrl+F o `arquivos`. Os paineis antigos de processos e arquivos do `wm`
continuam no `wmctl procs` e `wmctl files <caminho>`.
O terminal grafico entende as sequencias ANSI de cursor, apagamento e cores.

### Boot direto na interface grafica
O `init` le `/etc/autostart` no boot: uma linha por comando, `#` e comentario, e
`shell <caminho>` troca o programa que ele mantem vivo no terminal. O padrao
roda o `/bin/netup` (rede) e mantem o `shell /bin/sh`; `.\build.ps1 -Gui`
grava um autostart que roda o `/bin/netup` e sobe direto no `wm`.

### O disco do sistema
Sem `-Image`, o QEMU expoe o `out\esp` como um disco FAT (`fat:rw:`, o vvfat),
que aguenta pouca escrita. Com `-Image`, o build gera o `out\orion.img`: tabela
GPT, particao EFI de 64 MiB em FAT16 (Limine e `orion.elf`, montada em
`/boot/efi`) e a raiz em ext2 com `/bin`, `/lib`, `/usr`, `/etc` e o resto. O
kernel acha a raiz pelo tipo da particao GPT (raiz Linux x86-64) e monta com
escrita; o que voce grava sobrevive a desligar e ligar de novo (`sync` antes
de fechar o QEMU). Cada `.\build.ps1 -Image` gera a imagem de novo, a partir do
`out\esp`. `lsblk` (no shell do kernel) mostra as particoes.

    .\build.ps1 -Run -Smp 4 -Image
    echo ola > /nota.txt; sync      # depois feche o QEMU
    .\test.ps1 -NoBuild -Image -Commands @("cat /nota.txt")   # boota a mesma imagem

### Disco de escrita
Para exercitar o FAT de verdade use um disco cru:

    .\build.ps1 -Run -Smp 4 -Disk    # cria out\disk.img (64 MiB, zerado)
    mkfs /dev/sdb ORION              # formata em FAT16, dentro do Orion
    mount /dev/sdb /mnt              # monta em /mnt (ou Ctrl+K -> mount -> Ctrl+K)
    fstest -a /mnt                   # bateria completa de escrita
    vfstest /mnt                     # montagens, .., rename, arquivo apagado e aberto
    umount /mnt                      # EBUSY se houver arquivo aberto ou cwd dentro
    mount -r /dev/sdb /mnt           # so leitura

O ext2 (o driver e o piloto em Rust do kernel) monta com leitura e escrita;
`mount -r` monta so leitura. O proprio Orion formata e confere o disco:

    mkfs.ext2 -L DADOS /dev/sdb      # blocos de 1 KiB ate 512 MiB, 4 KiB acima (-b muda)
    fsck.ext2 /dev/sdb               # so confere; -y corrige o que for seguro
    mkdir /tmp/e
    mount -t ext2 /dev/sdb /tmp/e
    fstest -a /tmp/e
    umount /tmp/e

Links simbolicos funcionam no ext2 e no tmpfs (`ln -s alvo nome`,
`readlink nome`); o FAT nao tem links e devolve `EPERM`.

O ACPI passa pelo uACPI (`third_party/uacpi`): `poweroff`, `reboot` e `halt`
(como root) desligam e reiniciam de verdade, o botao de energia do QEMU
(`system_powerdown` no monitor) desliga, e a placa de rede usa a IRQ que o
`_PRT` do firmware indica.

O kernel liga SMEP e SMAP quando a CPU tem (o QEMU roda com
`-cpu qemu64,+smep,+smap`): ele nao executa nem le memoria de usuario fora das
copias conferidas. `crash smep` e `crash smap` (como root) provocam de proposito
o panico controlado de cada uma. A pilha e a base do `mmap` mudam de lugar a
cada `exec` (ASLR; `cat /proc/self/maps` duas vezes mostra).

Usuarios e permissoes valem desde a 4.1: cada processo tem `uid`/`gid`, a VFS
confere leitura, escrita e execucao, o `/tmp` tem o bit *sticky* e binario com
`setuid` troca o usuario efetivo. `id` mostra quem voce e; o `/etc/shadow`
(hash SHA-512, `$6$`) so o root le. `login` entra, `su [-] [-c cmd] [usuario]`
troca de usuario e `passwd [usuario]` troca a senha; o usuario `orion` tem o
`HOME` em `/home/orion`. Com `-Gui` o sistema vai direto para a interface
grafica, como root.

Modo, dono e datas ficam guardados no ext2 e no tmpfs: `chmod`, `chown`,
`chgrp` e `touch` gravam, `ls -l` mostra (os nomes de dono e grupo vem do
`/etc/passwd` e do `/etc/group`). Arquivo novo nasce com o modo pedido menos o
`umask` (022). No FAT so as datas e o somente leitura (`chmod 555`/`755`) sao
guardados; outros modos e donos dao `EPERM`, como no Linux.

    ls -l /tmp
    chmod 640 /tmp/x; chown 12:34 /tmp/x; touch /tmp/x

Os dois recusam disco montado. O `fsck.ext2` sai com 0 (limpo), 1 (corrigiu
tudo), 4 (sobrou problema) ou 8 (nao deu para conferir), como o `e2fsck`.
Imagens prontas tambem saem do `tools\mkext2.py`, a partir de um diretorio ou
da arvore de teste, e entram no QEMU com `test.ps1 -ExtraDisks`:

    python tools\mkext2.py out\e.img --test --size 32 --block 4096
    mount -t ext2 /dev/sdb /tmp/e    # dentro do Orion
    ext2test /tmp/e

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
do ultimo comando), `shot:<arquivo.ppm>`, `monlog:<comando do monitor>` (grava
a resposta no log), `reboot:` (fecha o QEMU e sobe de novo com os mesmos
discos, util com `-Image`; `reboot:bios,cd` troca firmware e midia) e `tcpprobe:<porta>` (conecta do
hospedeiro numa porta de `-HostFwd` e anota no log se o Orion fechou, ficou
calado ou mandou dados).

    .\test.ps1 -NoBuild -HostFwd @("tcp:127.0.0.1:5555-:7") -Script @("tcpprobe:5555")

`regressao.ps1` roda cinco sequencias e falha se algum marcador esperado sumir:
A (texto, rede, musl, threads, dash, lua, tcc), B (grafica e Doom, com
screenshots), C (disco cru e escrita), D (boot direto na interface grafica) e
E (estresse: quatro `stress 3000 16` em pipeline com 4 CPUs).

    .\regressao.ps1 -OnlyA          # so uma sequencia (OnlyA..OnlyE)
    .\regressao.ps1 -NoBuild -OnlyE -StressRuns 5    # o estresse 5 vezes seguidas
    python tools\regress.py ABCDE   # a mesma regressao em Python (Linux e CI)

## ISO

`.\build.ps1 -Iso` gera `out\orion.iso` com uma ferramenta propria
(`tools\mkiso.c`: ISO9660 com Rock Ridge e El Torito) e o `limine
bios-install`. A mesma ISO boota como CD ou gravada num pendrive (Rufus em
modo DD, ou `dd`), em BIOS ou UEFI. Ela e um sistema live: o sistema inteiro
(o mesmo da imagem de disco, Doom incluido) vai num initramfs que o kernel
desempacota num tmpfs usado como raiz, sem precisar de disco. Login `root` ou
`orion`, senha `orion`. Nada do que se grava sobrevive ao desligar.

## Programas proprios em /bin
    sh init edit wm top ps free uptime time kill env printenv clear
    ls cat echo wc date sleep hello cp mv rm mkdir touch head tail grep sort uniq tee
    ifconfig ping nslookup wget nc telnet httpget httpd wmctl hello-gui guidemo imgview monitor arquivos editor config terminal lsusb imginfo cpuuso dhcp netup id login su passwd poweroff reboot halt mkfs mkfs.ext2 fsck.ext2 mount umount
    testes: memtest fstest vfstest tmptest proctest pctest ext2test linktest attrtest unixtest permtest aslrtest shmtest polltest forktest ptytest limtest stress sctest filetest
            auxtest fputest badptr crash fbtest evtest
    testes da musl: mhello mthreads mfutex sigtest

Sao programas da musl como os portados (`user/musl/<nome>`); o que e proprio do
Orion (framebuffer, `/dev/input`, pty, informacoes de processo e de rede) esta
em `user/include/orion.h`.

### Rede
O QEMU sobe uma e1000 com a rede de usuario (SLIRP): 10.0.2.15 para o Orion,
10.0.2.2 de gateway e 10.0.2.3 de DNS. No boot, o `/etc/autostart` chama o
`netup`, que le o `/etc/network` (`eth0 dhcp`, `eth0 static <ip> <mascara>
<gateway> [dns]` ou `eth0 off`). O DHCP e um programa comum, fora do kernel.

    dhcp                      pega endereco, mascara, gateway e DNS (e grava o /etc/resolv.conf)
    ifconfig                  mostra a interface e os contadores da placa
    ping 10.0.2.2 4           ICMP echo
    nslookup example.com      consulta A no DNS

A pilha faz ARP, IPv4, ICMP, UDP e TCP (em Rust, `kernel/rust/tcp`); sockets
`SOCK_STREAM`, `SOCK_DGRAM` e `SOCK_RAW`. O TCP conecta, manda e recebe
e serve (`listen`/`accept`) com retransmissao, controle de congestionamento,
reordenacao e `TIME_WAIT`. O comando `net` do shell
do kernel mostra os contadores, e `tcploss N` descarta 1 em N segmentos TCP
para testar a retransmissao (o SLIRP do QEMU nao perde nada).

    wget http://example.com           salva index.html (-O arquivo, -q; so http://)
    nc 10.0.2.2 8000 / nc -l 7000     liga a entrada e a saida a uma conexao TCP (-N fecha no fim da entrada, -z testa a porta)
    telnet host [porta]               cliente telnet simples
    httpget example.com               baixa e mostra a pagina (HTTP/1.0)
    httpget -o /tmp/x 10.0.2.2 8000 /arquivo   grava o corpo; 10.0.2.2 e o hospedeiro
    httpget -u /tmp/x 10.0.2.2 8000 /destino   envia o arquivo com PUT
    httpd -p 80 -r / &                servidor de arquivos (GET); com
                                      .\test.ps1 -HostFwd @("tcp:127.0.0.1:8081-:80")
                                      o navegador do hospedeiro abre http://127.0.0.1:8081/
 O `test.ps1` grava o
trafego em `out\net.pcap`.

## Dispositivos em /dev
    /dev/fb0        framebuffer: ioctl FBIOGET_INFO / FBIOREDRAW, mmap
    /dev/input      eventos de teclado (todas as teclas, inclusive modificadoras) e mouse
    /dev/tty        terminal fisico (serial + console)
    /dev/random /dev/urandom   numeros aleatorios
    /dev/sda /dev/sdb ...   discos e particoes

## /proc
    /proc/<pid>/status cmdline maps stat fd/   cada processo (self = o proprio)
    /proc/meminfo cpuinfo uptime loadavg mounts version diskstats stat interrupts usb
    (AHCI e rede por MSI, cada uma numa CPU; a placa padrao dos testes e a e1000e)

Virtio: `test.ps1 -Image -Bus virtio` liga a imagem como `virtio-blk-pci` e
`-Nic virtio-net-pci` troca a placa de rede; o kernel acha o disco (`vda`),
monta a raiz em `vda2`, sobe a rede pela virtio-net e usa MSI-X nos dois. A
sequencia V da regressao roda assim.

NVMe: `test.ps1 -Image -Bus nvme` liga a imagem como controlador NVMe do QEMU;
o disco vira `nvme0n1` (particoes `nvme0n1p1`, `nvme0n1p2`, como no Linux), a
raiz monta no NVMe e a fila de I/O usa MSI-X. A sequencia N roda assim.

USB: o kernel liga o controlador xHCI (codigo em Rust, `kernel/rust/usb`),
enumera os dispositivos e `lsusb` (ou `/proc/usb`) mostra fabricante, produto,
versao USB, velocidade e classe. Teclado e mouse USB (HID, protocolo de boot)
funcionam como os PS/2, inclusive numa maquina sem PS/2; `test.ps1 -Usb` poe um
xHCI com teclado e mouse USB no QEMU, `-NoPs2` tira o controlador PS/2, e a
sequencia U roda assim. Pendrive USB (Bulk-Only, SCSI) vira um disco `sdX`
comum: `mkfs /dev/sdb` formata em FAT e `mount -t vfat /dev/sdb /tmp/pen`
monta; `test.ps1 -Usb -UsbDisk` liga um pendrive de 16 MiB (`out/stick.img`).
O `ps`, o `top`, o `free`, o `uptime` e a janela de processos do `wm` leem
daqui. `/proc/<pid>/fd/N` ainda e um arquivo com o caminho (vira link
simbolico na 2.7).

## Chamadas de sistema
Numeros e estruturas do Linux x86_64: arquivos (`openat` e os `*at`,
`getdents64`, `stat` de 144 bytes), memoria (`mmap` com arquivo e
`MAP_FIXED`, `mprotect`, `madvise`, `mremap`), processos e threads (`clone`,
`futex`, `exit_group`, `set_tid_address`), sinais com 64 sinais, `SA_RESTART`
e quadro `rt_sigframe`, tempo (`clock_gettime` com todos os relogios,
`clock_nanosleep`, `setitimer`, `getrusage`). A lista completa, com o que e
parcial, esta em `docs/SYSCALLS.md`. Dois numeros sao proprios do Orion:
readdir (1000) e openpty (1001); memoria e processos vem do `/proc`.

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
Rust no kernel (`kernel/rust/`, um workspace: `ext2` e `tcp` ligados por um
unico `staticlib`, o `orion`): `cargo` com o alvo `x86_64-unknown-none`
(`rustup target add x86_64-unknown-none`; o `setup.ps1` faz isso).
