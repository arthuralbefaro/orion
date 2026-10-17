# Plano de implementacao - do Orion 1.9 a um sistema utilizavel

Este documento e o mapa das proximas versoes. Cada etapa cabe numa sessao de
trabalho; um turno noturno costuma fechar de 4 a 6 etapas. O diario de cada
turno continua em `docs/noturno-progresso.md`, e as decisoes em
`docs/DECISOES.md`.

## O que "utilizavel" quer dizer aqui

Cinco marcos, cada um com uma frase que da para testar:

| Marco | Ao final, da para... | Fases |
|---|---|---|
| **M1 - roda software de verdade** | rodar `dash`, `lua`, `tcc` e Doom compilados para Linux, sem mudar uma linha deles | 0 e 1 |
| **M2 - guarda o que voce faz** | instalar o Orion num disco, desligar, ligar de novo e achar seus arquivos | 2 |
| **M3 - fala com a internet** | baixar uma pagina com `wget`, servir uma com `httpd` | 3 |
| **M4 - desktop de verdade** | abrir terminal, gerenciador de arquivos, editor e visualizador de imagens como programas separados, com fonte TrueType | 5 e 6 |
| **M5 - roda num PC** | gravar uma ISO num pendrive e bootar num computador fisico | 4 e 7 |

## A decisao que define o plano: compatibilidade com o Linux

O Orion ja usa os numeros de syscall do Linux onde existe equivalente. Levar
isso ate o fim - mesma ABI de syscall, mesmo layout de `struct stat`, mesmo
`auxv` na pilha inicial - faz qualquer programa estatico compilado contra a
**musl** rodar aqui. Em vez de portar programa por programa, portamos a libc
uma vez e ganhamos `dash`, `lua`, `tcc`, `sbase`, Doom, jogos e utilitarios.
A libc propria (`user/lib`) continua existindo enquanto a migracao acontece e
some no fim da fase 1.

## Linguagens

O kernel central (escalonador, memoria, interrupcoes, processos, VFS) continua
em C: ja esta estavel e reescrever traria so risco. As outras linguagens entram
onde pagam o custo:

| Linguagem | Onde | Por que |
|---|---|---|
| **Assembly** | so onde e obrigatorio ou mais rapido: entradas de interrupcao e syscall, troca de contexto, copias de memoria de usuario com tabela de excecoes (0.2), `memcpy`/`memset` com `rep movsb` (8.7) | o clang ja gera codigo tao bom quanto o feito a mao no resto |
| **Zig (como ferramenta)** | `zig cc -target x86_64-linux-musl` compila os programas de usuario contra a musl a partir do Windows (1.5); `zig c++` faz o mesmo para C++ | o Zig ja traz a musl e os cabecalhos prontos, o que elimina o maior risco da fase 1 |
| **Rust** | piloto no ext2 (2.5); se aprovado, TCP (3.1 a 3.4), USB (5.5 a 5.7) e parsers de dados vindos de fora | seguranca de memoria onde um ponteiro errado vira falha grave; entra como biblioteca estatica `no_std` chamada pelo C |
| **C++** | `libgui` e apps do desktop (fase 6), no espaco de usuario | classes ajudam a organizar janelas, botoes e listas; nunca no kernel |
| **Lua** | scripts e configuracao do sistema, depois da 1.9 | linguagem pequena, embutivel, ja portada no plano |

Toda linguagem segue as mesmas regras do C: sem comentarios no codigo,
`.\check.ps1` limpo (ele passa a checar `.rs`, `.zig` e `.cpp` quando
existirem) e regressao verde.

## Regras que valem para todas as etapas

- Codigo sem comentarios, arquivos inteiros, `.\check.ps1` limpo.
- `.\regressao.ps1` verde ao fim de cada etapa, mais o estresse:
  `stress 3000 16 | stress 3000 16 | stress 3000 16 | stress 3000 16`.
- Cada etapa ganha pelo menos uma verificacao nova na regressao.
- Um commit por etapa, com a versao no titulo.
- **Autoria dos commits e so do dono do projeto**, o usuario configurado no git.
- Etapa que mexe em trava, escalonador ou memoria roda o estresse 5 vezes
  seguidas com `-Smp 4` antes do commit.
- Tamanho: **P** = ate ~300 linhas, **M** = ~300 a 1000, **G** = mais que isso
  (as etapas G ja estao divididas em partes).

---

## Fase 0 - Fundacao (v1.9.x)

Fecha o que ficou aberto e tira os riscos que derrubam o kernel hoje.

| # | Etapa | Tam. | Pronto quando |
|---|---|---|---|
| 0.1 | Commit da v1.9 (estabilidade SMP) e dos programas `ps`, `kill`, `uptime`, `free`, `time`, `/dev/null`, `/dev/zero`, `/dev/random` | P | regressao verde, estresse 5x verde, commit feito |
| 0.2 | **Copia segura de memoria de usuario.** Hoje `user_range_ok` so confere a faixa; um `read(fd, 0x1000, 10)` com ponteiro nao mapeado leva o kernel a `#PF` em ring 0 e panico. Criar `copy_from_user`/`copy_to_user`/`strncpy_from_user` com tabela de excecoes (o handler de `#PF` ve que a falta veio de dentro da copia e devolve `-EFAULT`) e usar em todas as syscalls | M | programa novo `badptr` passa ponteiros invalidos para 20 syscalls e todas devolvem `EFAULT`, sem panico |
| 0.3 | Mutex que dorme (`kernel/mutex.{h,c}`) e trocar a trava do AHCI | P | `mutex` no shell do kernel mostra contencao; regressao C verde |
| 0.4 | Trocar a trava do FAT e do cache de blocos por mutex | M | estresse com `fstest -a` em paralelo, sem nenhuma espera girando |
| 0.5 | Harness de teste em Python (o `tools/qrun.py` e `tools/regress.py` usados na v1.9) no repo, para rodar a regressao em Linux/WSL e no GitHub Actions | P | `python tools/regress.py ABCDE` verde; workflow de CI rodando |
| 0.6 | Shell: `cmd &`, `jobs`, `fg`, `wait`, `VAR=x cmd`, `cmd1 && cmd2`, `cmd1 || cmd2`, `;` | M | verificacoes novas na sequencia A |
| 0.7 | Limites: 32 fds por processo vira 256, `PROCLIST_MAX` vira 256, `ulimit` basico | P | `stress` com 200 processos vivos ao mesmo tempo |

## Fase 1 - Compatibilidade Linux e musl (v2.0) -> marco M1

| # | Etapa | Tam. | Pronto quando |
|---|---|---|---|
| 1.1 | **Pilha inicial do Linux**: `argc`, `argv`, `envp` e `auxv` (`AT_PHDR`, `AT_PHENT`, `AT_PHNUM`, `AT_PAGESZ`, `AT_ENTRY`, `AT_RANDOM`, `AT_HWCAP`, `AT_NULL`), 16 bytes aleatorios para o `AT_RANDOM` | P | programa de teste le o `auxv` e confere cada campo |
| 1.2 | **Syscalls basicas que a musl chama na partida**: `set_tid_address`, `exit_group`, `uname`, `getuid`/`geteuid`/`getgid`/`getegid` (todos 0 por enquanto), `writev`, `readv`, `ioctl(TIOCGWINSZ)`, `rt_sigprocmask` completo, `sigaltstack` (stub) | M | tabela de syscalls em `docs/SYSCALLS.md` com estado de cada uma |
| 1.3 | **Arquivos no formato do Linux**: `struct stat` com o layout do Linux, `newfstatat`, `openat`, `getdents64`, `lseek` com `SEEK_END`, `access`, `faccessat`, `readlink` (devolve `ENOENT`), `fcntl(F_DUPFD, F_GETFD, F_SETFD)`, `dup3`, `pipe2`, `O_CLOEXEC` | M | `ls` da libc propria continua funcionando com o `getdents64` |
| 1.4 | **Memoria do jeito Linux**: lista de areas (VMA) por processo substituindo o `mmap_next`; `mmap` com `MAP_FIXED`, `MAP_PRIVATE` de arquivo, `mprotect`, `munmap` parcial, `madvise` (stub) | G (2 partes: VMAs; mmap de arquivo) | `memtest` novo cobrindo cada caso |
| 1.5 | **Programas contra a musl com o Zig**: instalar o Zig numa versao fixa (`winget install zig.zig` ou zip oficial em `third_party\zig`), e o `build.ps1` passa a compilar os programas de usuario com `zig cc -target x86_64-linux-musl -static`. Plano B: compilar os `.c` da musl pelo proprio `build.ps1` com o clang | M | `hello` compilado contra a musl roda no Orion |
| 1.6 | **Threads**: `clone` com `CLONE_VM|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD|CLONE_SETTLS|CLONE_PARENT_SETTID|CLONE_CHILD_CLEARTID`; processo deixa de ter uma thread so (`p->thread` vira lista) | G (2 partes: varias threads por processo; `clone`) | `pthread_create` da musl com 8 threads somando num contador |
| 1.7 | **futex** (`FUTEX_WAIT`, `FUTEX_WAKE`, com `FUTEX_PRIVATE_FLAG`) em cima das filas de espera | M | mutex e condvar da musl entre 8 threads, 1 milhao de operacoes |
| 1.8 | Tempo e sinais completos: `clock_nanosleep`, `gettimeofday`, `CLOCK_*` restantes, `sigsuspend`, `rt_sigtimedwait`, `tgkill`, `getrusage` | M | `sigtest` reescrito contra a musl passa |
| 1.9 | **Primeiros programas portados**: `dash` (shell POSIX), `lua` 5.4, `sbase` (coreutils simples) | M | `dash -c 'for i in 1 2 3; do echo $i; done'`; `lua -e 'print(2^10)'` |
| 1.10 | **Doom** via `doomgeneric` usando `/dev/fb0` e `/dev/input` | M | Doom roda a demo no QEMU; screenshot na regressao B |
| 1.11 | **tcc** dentro do Orion: compila e roda um `hello.c` dentro do proprio sistema | M | regressao compila e roda C dentro do Orion |
| 1.12 | Migrar os programas de `user/progs` para a musl e aposentar a libc propria (o `wm` por ultimo) | M | `user/lib` removido, regressao verde |

## Fase 2 - Sistema de arquivos e disco (v2.1) -> marco M2

| # | Etapa | Tam. | Pronto quando |
|---|---|---|---|
| 2.1 | **VFS com inodes**: cache de vnodes com contagem de referencia, tabela de montagens, `mount`/`umount` como syscalls, caminhos resolvidos componente a componente com `..` atravessando montagens | G (2 partes) | FAT e devfs montados pela nova VFS |
| 2.2 | **tmpfs** em `/tmp` e `/run` | P | 1000 arquivos criados e apagados sem vazar pagina |
| 2.3 | **procfs** em `/proc`: `/proc/<pid>/{status,cmdline,maps,fd}`, `/proc/meminfo`, `/proc/cpuinfo`, `/proc/uptime`, `/proc/mounts`; `ps`, `top`, `free` passam a ler dele e as syscalls 1002/1003 sao removidas | M | `cat /proc/self/maps` mostra as VMAs |
| 2.4 | **Cache de paginas unificado**: leitura de arquivo e `mmap` de arquivo usando as mesmas paginas | M | ler o mesmo arquivo duas vezes nao vai ao disco na segunda |
| 2.5 | **ext2 com leitura, piloto em Rust**: `kernel/rust/ext2` como `staticlib` `no_std` (`--target x86_64-unknown-none`, `panic = "abort"`), alocador ligado ao `kmalloc`, interface C declarada num `.h`; `build.ps1` chama o `cargo` e liga o `.a`. Ao fim, registrar em `DECISOES.md` se o Rust continua (tempo de build, tamanho, facilidade de depurar) | M | disco ext2 criado no host (ou pelo 2.6) montado e lido; decisao do piloto registrada |
| 2.6 | **ext2 com escrita**, `mkfs.ext2` proprio, `fsck` basico | G (2 partes) | `fstest -a` passa em ext2 |
| 2.7 | Links simbolicos, `chmod`, `chown`, `utime`, permissoes guardadas no ext2 | M | `ln -s`, `ls -l` mostra dono e modo |
| 2.8 | **GPT** e o disco do sistema: tabela GPT, particao EFI (FAT) + raiz (ext2); `build.ps1 -Image` gera `out\orion.img` pronto para bootar; o Orion boota com a raiz em ext2 e o `vvfat` sai da rotina | M | regressao inteira rodando com raiz em ext2, arquivos sobrevivem a reboot |

## Fase 3 - Rede de verdade (v2.2) -> marco M3

| # | Etapa | Tam. | Pronto quando |
|---|---|---|---|
| 3.1 | TCP parte 1: cabecalho, checksum, tabela de conexoes, `RST` para portas fechadas | M | `nmap` do hospedeiro ve a porta fechada |
| 3.2 | TCP parte 2: `connect`, handshake, envio e recebimento com janela fixa | M | cliente HTTP simples baixa `example.com` |
| 3.3 | TCP parte 3: retransmissao, temporizadores, janela deslizante, `FIN` e `TIME_WAIT` | G | baixar 10 MiB pelo SLIRP sem corromper (hash confere) |
| 3.4 | TCP parte 4: `listen`, `accept`, `backlog`, `shutdown`, `SO_REUSEADDR`, `getsockopt`/`setsockopt`, `select` sobre o `poll` | M | `httpd` no Orion responde a `curl` do hospedeiro (com `hostfwd`) |
| 3.5 | Loopback (`127.0.0.1`) e sockets Unix (`AF_UNIX`) | M | o `wm` e os apps conversam por socket Unix (prepara a fase 6) |
| 3.6 | Programas: `wget`, `nc`, `httpd`, `telnet`; `getaddrinfo` da musl lendo `/etc/resolv.conf` | M | `wget http://example.com` salva o arquivo |
| 3.7 | DHCP fora do kernel (vira programa de usuario) e configuracao em `/etc/network` | P | boot pega IP sem codigo de DHCP no kernel |

## Fase 4 - Usuarios e seguranca (v2.3)

| # | Etapa | Tam. | Pronto quando |
|---|---|---|---|
| 4.1 | `uid`/`gid` reais por processo, `setuid`/`setgid`, permissoes conferidas na VFS | M | usuario comum nao le `/etc/shadow` |
| 4.2 | `/etc/passwd`, `/etc/shadow` com hash (SHA-256 da musl ou proprio), `login`, `su`, `passwd`; o `init` sobe o `login` em vez do `sh` | M | boot pede usuario e senha |
| 4.3 | SMEP e SMAP (com `stac`/`clac` dentro das copias da etapa 0.2) | P | `crash` que tenta executar codigo de usuario em ring 0 vira panico controlado |
| 4.4 | ASLR basico (pilha e `mmap` com deslocamento aleatorio) | P | dois `cat /proc/self/maps` seguidos mostram enderecos diferentes |

## Fase 5 - Drivers e hardware (v2.4) - parte do caminho para o M5

| # | Etapa | Tam. | Pronto quando |
|---|---|---|---|
| 5.1 | **uACPI** (biblioteca C de ACPI com interpretador AML): desligar e reiniciar de verdade, botao de energia, rotas de IRQ do PCI pelo `_PRT` | M | comandos `poweroff` e `reboot` funcionam no QEMU e no VirtualBox |
| 5.2 | **MSI/MSI-X** para AHCI e rede; IRQs distribuidas entre as CPUs | M | `irq` mostra interrupcoes em mais de uma CPU |
| 5.3 | **virtio-blk** e **virtio-net** (mais rapidos que AHCI e e1000 no QEMU) | M | regressao roda nos dois modos |
| 5.4 | **NVMe** | M | boot com a raiz num disco NVMe do QEMU |
| 5.5 | **xHCI (USB 3)** parte 1: controlador, enumeracao de dispositivos | G | `lsusb` lista os dispositivos do QEMU |
| 5.6 | USB parte 2: teclado e mouse HID (PCs novos nao tem PS/2) | M | QEMU com `-device usb-kbd` sem PS/2 funciona |
| 5.7 | USB parte 3: armazenamento em massa (pendrive) | M | montar um pendrive FAT |
| 5.8 | **Audio HDA** + `/dev/dsp`; `play` toca um WAV; Doom ganha som | M | `play` no QEMU com `-device intel-hda` |
| 5.9 | Video: **Bochs/BGA** e **virtio-gpu** para trocar de resolucao | M | `fbset 1920x1080` muda a tela |

## Fase 6 - Desktop (v2.5) -> marco M4

| # | Etapa | Tam. | Pronto quando |
|---|---|---|---|
| 6.1 | Protocolo de janelas: apps sao processos separados, desenham em memoria compartilhada (`mmap` de um objeto do `wm`) e falam com o `wm` por socket Unix | G (2 partes) | `hello-gui` abre uma janela propria |
| 6.2 | `libgui`: botoes, campos de texto, listas, menus, rolagem | G | app de exemplo usando todos os componentes |
| 6.3 | Fontes TrueType (`stb_truetype`) com cache de glifos e antialias | M | terminal e barra com fonte vetorial |
| 6.4 | Imagens PNG/JPEG (`stb_image`), papel de parede, icones | P | papel de parede PNG |
| 6.5 | Compositor com double buffering e areas sujas (so redesenha o que mudou), cursor de mouse proprio | M | `wm` gasta menos de 5% de CPU parado |
| 6.6 | Apps: terminal, gerenciador de arquivos, editor de texto, visualizador de imagens, monitor do sistema, configuracoes | G (um app por etapa) | cada app tem screenshot na regressao B |
| 6.7 | Area de trabalho: barra de tarefas, alt+tab, minimizar e maximizar, lancador de apps | M | screenshot da regressao D com 3 apps abertos |

## Fase 7 - Distribuicao (v3.0) -> marco M5

| # | Etapa | Tam. | Pronto quando |
|---|---|---|---|
| 7.1 | **ISO hibrida**: ferramenta propria em C (roda no Windows) que monta ISO9660 + El Torito com o Limine para BIOS e UEFI; `build.ps1 -Iso` | M | ISO boota no QEMU em modo BIOS e UEFI, e no VirtualBox |
| 7.2 | **Live**: a ISO boota com a raiz em memoria (tmpfs a partir de um initramfs) | M | sistema inteiro usavel sem disco |
| 7.3 | **Instalador**: particiona o disco (GPT), formata, copia o sistema, instala o Limine | M | instalar no QEMU, tirar a ISO, bootar do disco |
| 7.4 | Teste em hardware real: notebook ou PC antigo via pendrive; relatorio com o que funcionou | P | `docs/hardware.md` com o resultado |
| 7.5 | Pacotes: formato `tar` + manifesto, comando `pkg install` baixando por HTTP de um repositorio estatico | M | `pkg install lua` num sistema recem-instalado |

## Fase 8 - Desempenho e robustez (continua, em paralelo)

| # | Etapa | Tam. |
|---|---|---|
| 8.1 | Fila de prontos por CPU com balanceamento e prioridades (`nice`) | M |
| 8.2 | Kernel preemptivo com contagem de travas seguras | M |
| 8.3 | Fuzzer de syscalls (programa que chama syscalls com argumentos aleatorios por 10 min) na regressao noturna | P |
| 8.4 | Falta de memoria: o kernel recusa em vez de travar; matador de processo (OOM) | M |
| 8.5 | Perfilador por amostragem (NMI do timer guarda RIPs; `perf` mostra as funcoes mais quentes) | M |
| 8.6 | Pilhas do kernel de 16 KiB medidas com o canario; trocar para 32 KiB se alguma passar de 75% | P |
| 8.7 | `memcpy`, `memset` e `memmove` do kernel em assembly com `rep movsb`/`rep stosb` | P |

---

## Ordem recomendada

1. **Fase 0 inteira**, com prioridade para a 0.2: hoje qualquer programa
   derruba o kernel passando um ponteiro ruim.
2. **Fase 1 ate a 1.10** (Doom rodando e o marco M1). E a fase que mais
   multiplica: cada etapa dela libera dezenas de programas prontos.
3. **Fase 2** (disco de verdade) e **fase 3** (TCP). Uma depende pouco da
   outra; da para alternar.
4. **7.1 e 7.2** cedo, logo depois da fase 2: ter uma ISO que roda no
   VirtualBox muda a forma de testar e de mostrar o projeto.
5. **Fase 6** (desktop) depois do socket Unix (3.5) e do `mmap` de arquivo (1.4).
6. **Fases 4, 5 e 8** entram aos poucos entre as outras, uma ou duas etapas
   por turno.

Sugestao de turnos noturnos:

| Turno | Etapas | Resultado visivel de manha |
|---|---|---|
| 1 | 0.1 a 0.7 | kernel blindado contra ponteiro ruim, shell com `&` e `&&`, CI |
| 2 | 1.1 a 1.5 | `hello` da musl rodando |
| 3 | 1.6 a 1.9 | threads, `dash` e `lua` |
| 4 | 1.10 a 1.12 | Doom e `tcc` dentro do Orion |
| 5 | 2.1 a 2.4 | VFS nova, `/proc`, `/tmp` |
| 6 | 2.5 a 2.8 | raiz em ext2 e arquivos que sobrevivem a reboot |
| 7 | 7.1, 7.2, 5.1 | ISO bootando no VirtualBox, `poweroff` |
| 8 | 3.1 a 3.4 | TCP |
| 9 | 3.5 a 3.7, 4.1, 4.2 | `wget`, `httpd`, login |
| 10+ | fase 6, fase 5, 7.3 a 7.5 | desktop, USB, instalador |

## Riscos conhecidos

- **musl no Windows**: resolvido pelo `zig cc`, que ja traz a musl. Se a versao
  da musl do Zig tiver alguma diferenca que atrapalhe, o plano B e compilar os
  `.c` da musl pelo `build.ps1` com o clang, e o plano C e o WSL so para
  gerar o `libc.a`.
- **Rust no kernel**: exige o `rustup` com o alvo `x86_64-unknown-none`, um
  alocador e um `panic` que chamem o kernel. Se o piloto (2.5) nao valer a
  pena, o ext2 e reescrito em C e o Rust sai do plano.
- **Fase 1.6 (threads)** mexe no coracao do escalonador e dos processos. Vai
  com o estresse 5x e um teste de threads novo antes do commit.
- **USB (5.5)** e o driver mais trabalhoso do plano; sem ele, PCs novos sem
  PS/2 nao tem teclado. Teste em hardware real (7.4) diz se ele e urgente.
- **Hardware real** pode exigir coisas que o QEMU nao mostra (ACPI de
  fabricante, framebuffer com outro formato, AHCI em modo RAID). O relatorio
  da 7.4 vira uma fase propria se for preciso.
