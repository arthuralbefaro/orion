# Intensivo Orion - progresso

Diario do trabalho sobre o `docs/PLANO.md`. Uma linha por etapa:
`[x]` feita, `[~]` em andamento, `[!]` bloqueada, `[ ]` nao comecada.
Este arquivo manda: uma sessao nova retoma da primeira etapa que nao esta `[x]`.

## Como retomar

    cd C:\dev\orion
    git log --oneline -10
    git status --short                            # limpa entre uma etapa e outra
    .\check.ps1                                   # precisa terminar com "Nenhum erro ou warning."
                                                  # (agora inclui cargo clippy e o fuzz do ext2, uns 20 s)
    .\regressao.ps1                               # A (texto, rede, musl, dash, lua, tcc), B (grafica, Doom),
                                                  # C (disco, ext2), D (GUI), E (estresse); exit 0 = verde
    .\regressao.ps1 -NoBuild -OnlyE -StressRuns 5 # estresse 5x seguidas (etapas de trava, memoria,
                                                  # processos, threads e sinais)
    python tools\regress.py ABCDE                 # a mesma regressao em Python (Linux e CI)

O build precisa de Python 3 (`tools\python.ps1` acha; `setup.ps1` instala), do
Rust com o alvo `x86_64-unknown-none` (`rustup target add x86_64-unknown-none`;
o `setup.ps1` faz) e baixa sozinho o Zig 0.16.0 (`tools\zig.ps1`) e o Freedoom
(`tools\freedoom.ps1`) na primeira vez, com SHA-256 conferido. Os programas
ficam em `user/musl/<nome>` (um `main.c` e/ou um `build.txt`); a interface
propria do Orion para eles esta em `user/include/orion.h`; o Rust do kernel em
`kernel/rust/<crate>`. A regressao demora uns 25 minutos; o estresse 5x, outros
10. Nunca rodar o `test.ps1` enquanto a regressao roda: ele mata o QEMU dela
(mesmas portas 4444/4445).

Estado (2026-10-18, 05:28, sessao 2): Fases 2, 3 e 4 inteiras (tags `v2.1` e
`v2.2` e `v2.3`), 8.7 commitada; marco da Fase 4 fechado. Versao 2.3.0.
SMEP, SMAP e ASLR ligados. O boot pede usuario e
senha (`root`/`orion`); o `test.ps1` e o `qrun.py` entram sozinhos como root.
A regressao inteira roda no `out\orion.img` (GPT, raiz em ext2), com 375
verificacoes; a sequencia A sobe o `tools/hostweb.py` no hospedeiro e usa
`hostfwd` (5555 e 8081).

Proxima etapa: 7.1 (ISO hibrida com ferramenta propria em C, `build.ps1
-Iso`), depois 7.2 (sistema live com a raiz em memoria) e 5.1 (uACPI). As
etapas 7.1 e 7.2, adiantadas pela ordem recomendada, viram 2.3.1 e 2.3.2 (a
Fase 7 inteira continua sendo a 3.0); a 5.1 abre a 2.4.0-a1.

## Checklist

| # | Etapa | Estado | Inicio | Fim | Commit |
| --- | --- | --- | --- | --- | --- |
| 0.1 | Commit da v1.9 e dos programas novos | [x] | 19:22 | 20:20 | `58bb747` |
| 0.2 | Copia segura de memoria de usuario | [x] | 20:22 | 22:15 | `8b3d171` |
| 0.3 | Mutex que dorme, AHCI | [x] | 22:16 | 22:41 | `4d55e72` |
| 0.4 | FAT e cache de blocos com mutex | [x] | 22:42 | 23:08 | `076285d` |
| 0.5 | Harness em Python e CI | [x] | 23:09 | 23:39 | `bc9a8e0` |
| 0.6 | Shell: `&`, jobs, fg, wait, `VAR=x cmd`, `&&`, `\|\|`, `;` | [x] | 23:40 | 00:00 | `d924ee2` |
| 0.7 | Limites: 256 fds, 256 processos, ulimit | [x] | 00:00 | 01:09 | `0af3cc4` |
| 1.1 | Pilha inicial do Linux (auxv) | [x] | 00:55 | 01:29 | `88ce390` |
| 1.2 | Syscalls de partida da musl | [x] | 01:29 | 01:51 | `e809b3a` |
| 1.3 | Arquivos no formato do Linux | [x] | 01:51 | 02:13 | `dd00ea2` |
| 1.4 | Memoria do jeito Linux (VMAs, mmap) | [x] | 02:13 | 02:51 | `0edfa7e` |
| 1.5 | Programas contra a musl com `zig cc` (plano B: musl pelo build.ps1) | [x] | 02:52 | 03:10 | `a37486a` |
| 1.6 | Threads (clone) | [x] | 03:10 | 03:50 | `5871386` |
| 1.7 | futex | [x] | 03:50 | 04:28 | `f40fab9` |
| 1.8 | Tempo e sinais completos | [x] | 04:28 | 05:37 | `a75d6ab` |
| 1.9 | dash, lua, sbase | [x] | 05:37 | 06:28 | `7208886` |
| 1.10 | Doom | [x] | 06:28 | 07:10 | `fe72b5e` |
| 1.11 | tcc dentro do Orion | [x] | 07:10 | 07:54 | `4f1b270` |
| 1.12 | Programas na musl, aposentar a libc propria | [x] | 07:54 | 08:50 | 171ba11 |
| 2.1 | VFS com inodes | [x] | 08:51 | 11:05 | 9f729b3 |
| 2.2 | tmpfs | [x] | 11:06 | 11:47 | 3b80cdc |
| 2.3 | procfs | [x] | 11:48 | 12:52 | b0a87e9 |
| 2.4 | Cache de paginas unificado | [x] | 12:53 | 14:55 | c485b85 |
| 2.5 | ext2 leitura, piloto em Rust | [x] | 14:56 | 16:36 | ver `git log` |
| 2.6 | ext2 escrita, mkfs.ext2, fsck | [x] | 16:36 | 19:00 | ver `git log` (3 partes) |
| 2.7 | Links simbolicos, chmod, chown, utime | [x] | 20:20 | 22:40 | `921c465` e ver `git log` (parte 2) |
| 2.8 | GPT e raiz em ext2 | [x] | 22:42 | 23:25 | ver `git log` (tag `v2.1`) |
| 3.1 | TCP parte 1 | [x] | 23:27 | 23:52 | ver `git log` |
| 3.2 | TCP parte 2 | [x] | 23:53 | 01:00 | ver `git log` |
| 3.3 | TCP parte 3 | [x] | 01:02 | 01:45 | ver `git log` |
| 3.4 | TCP parte 4 | [x] | 01:48 | 02:25 | ver `git log` |
| 3.5 | Loopback e AF_UNIX | [x] | 02:26 | 03:10 | ver `git log` |
| 3.6 | wget, nc, httpd, telnet | [x] | 03:11 | 03:35 | ver `git log` |
| 3.7 | DHCP fora do kernel | [x] | 03:36 | 04:15 | ver `git log` (tag `v2.2`) |
| 4.1 | uid/gid e permissoes | [x] | 04:16 | 04:46 | ver `git log` |
| 4.2 | passwd, shadow, login | [x] | 04:46 | 05:28 | ver `git log` |
| 4.3 | SMEP e SMAP | [x] | 05:28 | 05:55 | ver `git log` |
| 4.4 | ASLR basico | [x] | 05:55 | 06:26 | ver `git log` (tag `v2.3`) |
| 5.1 | uACPI | [ ] | | | |
| 5.2 | MSI/MSI-X | [ ] | | | |
| 5.3 | virtio-blk e virtio-net | [ ] | | | |
| 5.4 | NVMe | [ ] | | | |
| 5.5 | xHCI | [ ] | | | |
| 5.6 | USB HID | [ ] | | | |
| 5.7 | USB armazenamento | [ ] | | | |
| 5.8 | Audio HDA | [ ] | | | |
| 5.9 | Video BGA e virtio-gpu | [ ] | | | |
| 6.1 | Protocolo de janelas | [ ] | | | |
| 6.2 | libgui | [ ] | | | |
| 6.3 | Fontes TrueType | [ ] | | | |
| 6.4 | Imagens PNG/JPEG | [ ] | | | |
| 6.5 | Compositor com areas sujas | [ ] | | | |
| 6.6 | Apps | [ ] | | | |
| 6.7 | Area de trabalho | [ ] | | | |
| 7.1 | ISO hibrida | [ ] | | | |
| 7.2 | Live | [ ] | | | |
| 7.3 | Instalador | [ ] | | | |
| 7.4 | Hardware real | [ ] | | | |
| 7.5 | Pacotes | [ ] | | | |
| 8.1 | Fila por CPU e nice | [ ] | | | |
| 8.2 | Kernel preemptivo | [ ] | | | |
| 8.3 | Fuzzer de syscalls | [ ] | | | |
| 8.4 | OOM | [ ] | | | |
| 8.5 | Perfilador | [ ] | | | |
| 8.6 | Pilhas do kernel medidas | [ ] | | | |
| 8.7 | memcpy/memset/memmove do kernel em assembly (rep movsb/stosb) | [x] | 20:05 | 22:00 | `9ad91e7` |

## Diario

### Sessao 1 - 2026-10-16

Ponto de partida (19:22): `.\check.ps1` limpo; `.\regressao.ps1` VERMELHA,
14 de 81. Todas as falhas na sequencia A, do `ping` em diante: o kernel entrou
em panico (`assert falhou: sched_runq_held()` em `wait.c`) no segundo `ping`.

#### 0.1 Commit da v1.9 - 19:22 as 20:20 - commit 58bb747 - FEITO
Validado no Windows o trabalho da v1.9 que estava fora do git (trava no heap,
liberacao adiada com TLB shootdown sincrono, paginas de guarda, pilhas IST,
watchdog, detector de deadlock) e os programas `stress`, `true`, `ps`, `kill`,
`uptime`, `free`, `time`, mais `/dev/null`, `/dev/zero` e `/dev/random`.
Problema achado: no segundo `ping` seguido o kernel caia com
`assert falhou: sched_runq_held()` em `entry_detach`. Causa: `net_deliver_udp`
e `net_deliver_icmp` (desde a v1.6) chamavam `wake_up_all_locked` segurando so
a `sock_lock`; a fila de espera exige a `runq_lock`, entao a thread de
recepcao da rede mexia na fila ao mesmo tempo que o escalonador. Nunca tinha
aparecido porque nada conferia; o assert novo da v1.9 pegou. Correcao: usar
`wake_up_all`. Para achar o chamador o `panic` passou a imprimir a pilha de
chamadas (nao ha `llvm-addr2line` nesta maquina; `llvm-symbolizer
--obj=out/orion.elf <endereco>` resolve).
Outros ajustes: `uptime` mostrava sempre 0 processos (`sysinfo` nunca
preenchia `procs`); `head -c N`; passo novo `until:<segundos>:<texto>` no
`test.ps1`, que espera o texto aparecer depois do ultimo `cmd:` (o estresse
usa isso para esperar o fim de verdade em vez de um tempo fixo).
Regressao: 81 -> 92 verificacoes (ps, uptime, free, time, kill, os tres
dispositivos novos e um `stress 300 8`). Estresse 5x verde (15 a 31 s cada).
Observacao: no estresse so o ultimo `stress` do pipeline aparece na serial (os
outros escrevem no pipe); o criterio usado e a linha visivel com
`0 falha(s) ok`, o eco de um marcador depois do pipeline e nenhum
`PANIC`/`EXCECAO`/`deadlock` no log.

#### 0.2 Copia segura de memoria de usuario - 20:22 as 22:15 - commit 8b3d171 - FEITO
`copy_from_user`, `copy_to_user` e `strncpy_from_user` com tabela de excecoes
(`.ex_table` no `linker.ld`, `arch/x86_64/uaccess.S`, `mm/uaccess.c`); o
handler de `#PF`/`#GP` em ring 0 desvia para a retomada. Todas as syscalls que
recebem ponteiro foram convertidas (o `syscall.c` foi reescrito), mais os
ioctls do TTY, PTY, framebuffer e socket, `sendto`/`recvfrom`/`bind`/
`connect`, o quadro de sinal e o `execve` (que agora copia `argv`/`envp` para
o kernel antes de qualquer coisa). `read`/`write` usam um buffer intermediario
no kernel, entao nenhum driver recebe mais ponteiro de usuario.
Programa novo `badptr`: 36 casos (32 syscalls com ponteiro nao mapeado, do
kernel, nao canonico ou somente leitura devolvendo `EFAULT`, mais quadro de
sinal invalido, handler nao canonico, SIGSEGV ignorado e flag NT no handler).
Problemas achados no caminho:
(1) `rt_sigreturn` voltava por `sysret`, que sobrescreve RCX e R11: o codigo
interrompido por um sinal perdia esses registradores. Passou a voltar por
`iretq`. O `sigtest` ganhou um caso que prova isso (falha sem a correcao,
conferido).
(2) O `rt_sigreturn` aceitava RIP, RSP e RFLAGS do quadro sem conferir:
IOPL/TF/NT do usuario e RIP nao canonico (que vira `#GP` em ring 0 no
retorno). Higienizado.
(3) Encontrado por uma revisao adversarial do diff, antes do commit: NT ligado
pelo usuario sobrevive a entrada da syscall (o `FMASK` so zerava IF, TF e DF),
e o `iretq` novo com NT ligado da `#GP` em ring 0 com o GS do usuario
carregado - o sistema morria em silencio (conferido: sem a correcao o
`badptr` trava a maquina no caso do NT). Correcao: NT e AC no `FMASK`.
(4) Mesma revisao: falta sincrona com SIGSEGV ignorado/bloqueado virava laco
infinito de falta + log; `execve` que falhava deixava `brk` e contadores do
processo alterados; o `read` podia bloquear numa segunda leitura de pipe depois
de um pedaco cheio. Corrigidos.
(5) `poll` nunca devolvia `EINTR`: um sinal acordava a thread, que voltava a
dormir, e o sinal so era tratado quando algum fd ficasse pronto.
Tambem: carregador ELF com conferencia de estouro de inteiro e entrada fora do
espaco de usuario; `build_stack` recusa argumentos maiores que metade da pilha.
O `mem` do shell do kernel mostra quantas faltas a tabela de excecoes recuperou.
(6) O estresse 5x da 0.2 deu 4/5: numa rodada o ultimo `stress` sumiu sem a
linha final; em outra o pipeline travou para sempre depois de `2500/3000`, sem
panico nem watchdog. Para diagnosticar, o `stress` passou a reportar erros em
`stderr` (os estagios do meio escrevem no pipe e os erros sumiam) e o script de
estresse passou a despejar `procs`, `ps` e `smp` no fim. Numa rodada travada:
o estagio pid 4 esperava o filho 9295, que estava `running` sem thread
nenhuma; e o `sh` aparecia `zombie` com a thread `blocked`. Causa (achada por
uma investigacao em paralelo do codigo e confirmada): `thread_current()` fazia
duas leituras (`percpu_get()` e `->current`); uma thread preemptada entre as
duas e migrada de CPU lia o `current` da CPU antiga, e `process_current()`
devolvia outro processo - o filho terminava "como" o `sh`. Correcao: uma unica
instrucao `movq %gs:current`. Depois disso, 25/25 rodadas verdes.
A mesma investigacao confirmou e eu corrigi: uso apos liberacao no fechamento
de pipe e de PTY (acordar a outra ponta depois de soltar a trava), filhos de um
zumbi colhido com ponteiro `parent` pendurado (agora os filhos ficam orfaos na
hora em que o pai termina), `execve` publicando o espaco novo antes de trocar
o CR3 (uma preempcao no meio deixava a thread no espaco em construcao), `poll`
dormindo sem fila nenhuma quando um sinal chegava durante o registro, e
`getppid` lendo `parent` sem a trava.
Regressao: 92 -> 97 verificacoes. Estresse 5x verde na versao final (e 25/25
logo antes, com as correcoes de concorrencia). Observacao: o commit `8b3d171`
levou junto as edicoes que o dono fez durante a sessao no arquivo local de regras (regra de
comentarios para todas as linguagens) e em `docs/PLANO.md` (secao "Linguagens",
1.5 com `zig cc`, piloto Rust no ext2 em 2.5, etapa nova 8.7).

#### 0.3 Mutex que dorme - 22:16 as 22:41 - commit 4d55e72 - FEITO
`kernel/mutex.{h,c}`: dono, fila de espera, spinlock interno so do mutex,
estatisticas (aquisicoes, contencao, espera total e maxima), panico em
recursao e em disputa com interrupcoes desligadas, aviso a cada 5 s de espera
com o dono. Comando `mutex` no shell do kernel. Cada porta AHCI tem o seu
mutex (`ahci0`, `ahci1`) e a espera pelo fim do comando cede a CPU quando
possivel. Regressao 97 -> 99 (sequencia C confere a tabela do `mutex` e que o
`ahci0` foi pego e esta livre no fim). Estresse 5x verde.
Limite conhecido: ate a 0.4 o AHCI e chamado de dentro do spinlock do cache de
blocos, entao a contencao do `ahci*` fica em 0; quem disputa de verdade e o
cache, que a 0.4 troca por mutex.
A partir desta etapa, antes de cada commit roda o comando do dono que procura
`//` e `/*` nos arquivos de codigo staged.

#### 0.4 FAT e cache de blocos com mutex - 22:42 as 23:08 - commit 076285d - FEITO
A trava do cache de setores (`bcache`) e a de cada FAT (`fat-sda1`,
`fat-sdb`) viraram mutex; ordem `fat-*` -> `bcache` -> `ahci*`. A sequencia C
roda duas `fstest -a` ao mesmo tempo (diretorios /mnt/p1 e /mnt/p2), as duas
passam, e a tabela do `mutex` mostra disputa com espera dormindo (numa rodada:
`fat-sdb` 17 disputas, `bcache` 14). Regressao 99 -> 102, estresse 5x verde.
Como o `&` so chega na 0.6, o paralelismo usa um pipeline com a saida da
primeira instancia redirecionada para arquivo.
Ao commitar, o comando do dono que procura comentarios pegou quatro
comentarios antigos no `fat.c` (de antes do intensivo); removidos, o porque
deles ja estava no DECISOES (v1.3). Tambem tirei um BOM que um
`Set-Content -Encoding utf8` colocou no diario.

#### 0.5 Harness em Python e CI - 23:09 as 23:39 - commit bc9a8e0 - FEITO
`tools/qrun.py` (equivalente ao `test.ps1`, com os mesmos passos) e
`tools/regress.py`, que le os passos e as verificacoes do proprio
`regressao.ps1` em vez de manter uma segunda copia. Sequencia nova E no
`regressao.ps1` (estresse, `-OnlyE`, `-StressRuns N`); o estresse 5x das
proximas etapas passa a ser `.\regressao.ps1 -OnlyE -StressRuns 5`.
`python tools/regress.py ABCDE` verde no Windows (105 verificacoes, iguais as
do `.\regressao.ps1`). Workflow `.github/workflows/regressao.yml` (Ubuntu
24.04: `make` + `tools/regress.py ABCDE`).
O que faltou: o workflow so roda quando o dono fizer push, e nesta maquina nao
ha Linux para conferir o caminho do `make` (o WSL so tem a distro do Docker e
o daemon do Docker estava parado). O `qrun.py` foi escrito para os dois
sistemas, mas so o caminho do Windows foi exercitado.

#### 0.6 Shell: listas e trabalhos - 23:40 as 00:00 - FEITO
`sh` reescrito na parte de execucao: a linha vira uma lista de pipelines
separados por `;`, `&&`, `||` e `&`; `$?`; `NOME=valor cmd` so para o comando;
`cmd &` com tabela de trabalhos, `jobs`, `fg` e `wait`; aviso `[n] pronto` no
prompt. Trabalho em segundo plano le de `/dev/null`. Programa novo `false`.
Os comentarios antigos do `sh/main.c` sairam junto com a reescrita.
Regressao 105 -> 118 (13 verificacoes novas na sequencia A, inclusive uma
negativa: `false && echo NAO-DEVIA` nao pode imprimir). O `regress.py`
aprendeu `ExpectNot`. Sem estresse: a etapa so mexe em espaco de usuario.
Fora: `bg`, Ctrl+Z e `2>&1` (o kernel ainda nao tem `SIGTSTP`/`SIGCONT`).

#### 0.7 Limites - 00:00 as 01:09 - FEITO
Tabela de descritores alocada, comecando com 32 e dobrando ate o
`RLIMIT_NOFILE` (256); `getrlimit`/`setrlimit`/`prlimit64`; `RLIMIT_NPROC`
(512) conferido no `fork`; `proclist` ate 256; `ulimit` no `sh`. Programa
novo `limtest` (253 descritores alem de 0/1/2 e `EMFILE`, limite 16, `dup2`
na borda 255/256, `EINVAL`/`EPERM`, `RLIMIT_NPROC` deixando passar 3 forks e
devolvendo `EAGAIN` nos outros). `stress N W ms` mantem os filhos vivos ms
milissegundos; a regressao confere 220 vivos ao mesmo tempo pelo `ps | wc`.
Regressao 118 -> 123, estresse 5x verde (`.\regressao.ps1 -OnlyE -StressRuns 5`).
Problemas achados:
(1) O passo `until` do `test.ps1` so procurava o texto quando chegavam dados
novos: se a saida inteira ja tinha chegado durante o `cmd:`, ele esperava o
tempo todo. Agora confere antes de esperar.
(2) Os `until` do `badptr` e do `limtest` usavam como marcador o inicio da
primeira linha (`badptr: `), entao o passo seguinte (um Ctrl+C) chegava com o
programa ainda rodando e o matava; tudo depois na sequencia A se deslocava (14
verificacoes vermelhas sem nenhum bug no kernel). As linhas finais ganharam um
marcador proprio (`badptr: fim`, `limtest: fim`).
(3) O primeiro `ps | wc` contou 194 em vez de 220+: 1 de cada 7 filhos do
`stress` faz `exec /bin/true` e terminava antes da espera. A espera passou a
vir antes do `exec`.

#### 1.1 Pilha inicial do Linux - 00:55 as 01:29 - FEITO
`argc`/`argv`/`envp` seguidos do vetor auxiliar completo (`AT_PHDR`,
`AT_PHENT`, `AT_PHNUM`, `AT_PAGESZ`, `AT_ENTRY`, `AT_RANDOM` com 16 bytes
novos a cada exec, `AT_HWCAP`, `AT_PLATFORM`, `AT_EXECFN`, ids e `AT_NULL`).
Junto, por ser pre-requisito da musl do Zig (1.5): SSE ligado em todas as
CPUs e estado de SSE por thread (`fxsave64`/`fxrstor64` na troca, no `fork`,
no `execve` e no quadro de sinal). Programas novos `auxtest` (16 verificacoes,
inclusive que o estado de SSE comeca limpo e que o `AT_RANDOM` muda entre um
exec e outro, lido por um filho via pipe) e `fputest` (4 processos com XMM e
MXCSR proprios, handler de sinal que estraga tudo sem efeito, `fork` herdando).
Regressao 123 -> 129, estresse 5x verde.
Problemas: (1) `fninit` nao zera os XMM e o firmware UEFI os usa, entao o
estado inicial capturado no boot tinha lixo - zerado a mao; (2) os programas
da libc propria nao tem os cabecalhos ELF dentro de um segmento carregado e o
`AT_PHDR` saia 0 - o kernel agora copia a tabela para a pilha nesse caso.

#### 1.2 Syscalls de partida da musl - 01:29 as 01:51 - FEITO
`set_tid_address`, `exit_group`, `uname`, `getuid`/`geteuid`/`getgid`/
`getegid`, `gettid`, `readv`, `writev`, `sigaltstack` (stub) e
`rt_sigprocmask`/`rt_sigaction` completos com `sigsetsize`. Os sinais passaram
para o formato do Linux (64 sinais, bit `sig-1`), com a tabela de handlers
alocada a parte (2 KiB, no slab). `struct termios` do `TCGETS` no layout do
kernel Linux. Tabela `docs/SYSCALLS.md` com o estado de cada syscall
(completa, parcial, stub, propria). Programa novo `sctest` (22 verificacoes).
Regressao 129 -> 134, estresse 5x verde.

#### 1.3 Arquivos no formato do Linux - 01:51 as 02:13 - FEITO
`struct stat` do Linux (144 bytes) em `stat`/`fstat`/`lstat`/`newfstatat`;
`openat`, `mkdirat`, `unlinkat`, `renameat`, `faccessat`, `readlinkat`
relativos a um descritor de diretorio (todo arquivo aberto por caminho guarda
o caminho); `getdents64`; `access`; `readlink` (EINVAL/ENOENT); `fcntl` com
`F_DUPFD`, `F_DUPFD_CLOEXEC`, `F_GETFD`, `F_SETFD`; `dup3`; `pipe2`;
`O_CLOEXEC`, `O_DIRECTORY`, `O_EXCL` e os erros certos no `open`. A libc
propria lista diretorios com `getdents64` e o `ls` continua funcionando (usa
`d_type` e `stat`). Programa novo `filetest` (33 verificacoes, inclusive um
neto que confere por `exec` que um descritor `FD_CLOEXEC` sumiu e outro nao).
Regressao 134 -> 139, estresse 5x verde.

#### 1.4 Memoria do jeito Linux - 02:13 as 02:51 - FEITO
Lista de areas por processo (`mm/vma.c`) no lugar do `mmap_next`; memoria
anonima, `brk` e pilha (agora 8 MiB) sob demanda pela falta de pagina;
`mmap` com dica, `MAP_FIXED`, `MAP_FIXED_NOREPLACE`, `MAP_SHARED` anonimo e
`MAP_PRIVATE` de arquivo (pagina lida do arquivo na falta); `munmap` parcial;
`mprotect` (inclusive `PROT_NONE` preservando o conteudo); `madvise` com
`MADV_DONTNEED`; `brk` com a semantica do Linux. As duas partes do plano
(areas e `mmap` de arquivo) entraram juntas. `memtest` reescrito (43
verificacoes, cada caso da etapa).
Uma revisao do diff antes do commit achou 10 problemas reais, todos
corrigidos e varios com caso novo no `memtest`: pagina privada so de leitura
compartilhada sem copy-on-write no `fork` (um `mprotect` depois vazava escrita
para o outro processo); `MAP_SHARED` anonimo tocado so depois do `fork` nao
era compartilhado; pagina orfa vazada alem do framebuffer; `brk` atravessando
outras areas; `vma_add_merge` pulando area; bits de cache do framebuffer
perdidos no `mprotect`; `MADV_DONTNEED` zerando o executavel; ultima pagina da
metade de usuario mapeavel (`sysret` nao canonico); CR2 relido depois de
religar as interrupcoes.
Regressao 139 -> 149, estresse 5x verde.
Fica para depois: sem memoria na falta de pagina o kernel ainda entra em
panico ao criar tabela de paginas (etapa 8.4); falta de arquivo com
interrupcoes desligadas e recusada (so acontece empurrando quadro de sinal).

#### 1.5 Programas contra a musl com o Zig - 02:52 as 03:10 - FEITO
Plano A do plano (o `zig cc`) funcionou; o plano B nao foi preciso. Zig 0.16.0
baixado pelo `tools/zig.ps1` (zip oficial, SHA-256 conferido) para
`third_party/zig`, fora do git; `build.ps1` compila `user/musl/<nome>/` com
`zig cc -target x86_64-linux-musl -static -O2 -s`; `check.ps1` confere esses
programas com o proprio Zig; `tools/musl_build.py` faz o mesmo no Linux/CI e o
workflow baixa o Zig para Linux. Programa novo `mhello` (contra a musl, 13
verificacoes: TLS com `__thread`, `errno`, `getenv`, `printf` com ponto
flutuante, `malloc` pequeno e grande, `qsort`, `stat`, `opendir`/`readdir`,
`uname`, `time`). Rodou no Orion de primeira, sem mudar o kernel - as etapas
1.1 a 1.4 cobriram a partida da musl. A unica syscall que faltou foi
`sched_getaffinity`, agora implementada; a sequencia A ganhou uma verificacao
de que nenhuma syscall desconhecida aparece. `third_party/LICENSES.md` criado.
Regressao 149 -> 154.

#### 1.6 Threads (clone) - 03:10 as 03:50 - FEITO
`clone(CLONE_THREAD)` com TLS, `PARENT_SETTID`, `CHILD_SETTID` e
`CHILD_CLEARTID`; `gettid` com tid por thread; `exit` encerra a thread e
`exit_group` (e morte por sinal) encerra todas; mascara de sinais por thread e
pendentes por processo e por thread; `execve` e `fork` com varias threads;
`mm_lock` por processo em volta de VMAs e tabelas de paginas, com shootdown de
TLB quando ha mais de uma thread; tabela de descritores sob o `tlock` e
arquivos presos durante a syscall (`close` em outra thread nao libera um
arquivo no meio de um `read`). Espera interrompivel nova (`wait_sleep_intr`)
fecha a corrida antiga do sinal que chegava entre a verificacao e o sono;
pipe, terminal, pty e `waitpid` usam ela, e `pause` virou syscall de verdade.
Programa novo `mthreads` (contra a musl, 13 verificacoes: 8 threads com
contador atomico e `pthread_mutex`, TLS, `gettid`, `pthread_exit`, sinal,
`exit` numa thread, `fork` com threads, `exit` e `execve` com thread presa
num `read`, `execve` a partir de outra thread).
Uma revisao do diff antes do commit achou 9 problemas, corrigidos: thread que
saia antes da ultima podia rodar de novo com o processo ja colhido (agora o
processo conta as structs de thread); `execve` concorrente travava e um
`execve` que falhava matava as threads; `dup3`, a copia de descritores do
`fork` e `F_GETFD`/`F_SETFD` sem trava; quadro de sinal numa pilha nova
falhando com interrupcoes desligadas; sinal de falta indo para outra thread;
sinal entregue duas vezes; sinal bloqueado depois de escolhido sem outra
thread acordada; `MAP_FIXED` com falha sem shootdown; escrita no pty sem
interrupcao. `docs/SYSCALLS.md` tambem recebeu as syscalls da 1.3 e da 1.4
que tinham ficado de fora da tabela.
Regressao 154 -> 164, estresse 5x verde.
Fica para a 1.7: sem `futex` a musl espera girando (a 202 aparece como
desconhecida e a regressao a tolera so ate la).

#### 1.7 futex - 03:50 as 04:28 - FEITO
`futex` com `WAIT` (timeout relativo), `WAKE`, `REQUEUE` e `CMP_REQUEUE`,
privado e compartilhado: trava global, 64 baldes, esperador na pilha com fila
propria, chave por (processo, endereco) ou pelo endereco fisico numa area
`MAP_SHARED`. `clear_child_tid` acorda o `pthread_join`. O comando `mutex` do
shell do kernel mostra quantas threads estao no futex. Programa novo `mfutex`
(contra a musl, 20 verificacoes): 1 milhao de operacoes de `pthread_mutex`
entre 8 threads (cerca de 0,5 s no QEMU), condvar produtor/consumidor, barreira,
semaforo, `timedwait`, `EAGAIN`/`ETIMEDOUT`/`EINTR`/`EINVAL`, `CMP_REQUEUE`,
mutex `PROCESS_SHARED` entre dois processos e `fork` com thread no futex.
A verificacao "nenhuma syscall desconhecida" voltou a valer sem excecao.
Duas revisoes rodaram antes do commit, cada achado conferido por dois
revisores independentes. A do futex achou 12 problemas, corrigidos: conta do
prazo estourando com timeout enorme, ordem LIFO, `EFAULT` espurio na segunda
leitura, retornos diferentes do Linux, e testes que dependiam de `usleep`
(agora contam os esperadores). A segunda revisao do codigo da 1.6 achou 18
problemas, 16 corrigidos aqui. O mais serio: paginas voltavam ao alocador
antes do shootdown de TLB, e outra CPU podia escrever numa pagina ja
reaproveitada. Os outros: thread nova liberada antes do criador terminar de
solta-la, `SIGCHLD` antes de o filho poder ser colhido, sinal nao redirecionado
quando a thread sai ou o bloqueia, `cwd`, `brk` e descritores sem trava no
`fork`/`chdir`/`pipe2`, NMI entregando sinal. `mthreads` ganhou
`mmap`/`munmap` concorrente e handler de `SIGCHLD` com `WNOHANG`.
Regressao 164 -> 179, estresse 5x verde.
Fica para a 1.8: `SA_RESTART` (toda espera interrompida devolve `EINTR`) e
`nanosleep` interrompivel com o tempo restante. Fica para a robustez (fase 8):
NMI na janela antes do `swapgs` da syscall.

#### 1.8 Tempo e sinais completos - 04:28 as 05:37 - FEITO
Reinicio de syscall como no Linux (`ERESTARTSYS`, `ERESTARTNOHAND`,
`ERESTARTNOINTR`, `SA_RESTART` e "restart block" com `restart_syscall` para
esperas com prazo); `rt_sigsuspend`, `rt_sigpending`, `rt_sigtimedwait`,
`tkill`, `tgkill`; quadro de sinal no formato do `rt_sigframe` do Linux, com
`siginfo` (origem de cada sinal: pid de quem mandou, `SI_TKILL`, `SIGCHLD` com
pid e status, falta com endereco) e `ucontext` que o handler pode alterar
(`pthread_cancel` da musl funciona); `SA_SIGINFO`, `SA_NODEFER`,
`SA_RESETHAND`; todos os relogios do `clock_gettime`, `clock_getres`,
`gettimeofday`, `time`, `nanosleep` e `clock_nanosleep` interrompiveis com o
tempo restante, `getrusage`, `times`, `wait4` com `rusage`,
`setitimer`/`getitimer` com intervalo. Tempo de CPU medido pelo TSC na troca
de contexto: no QEMU TCG o timer chega a uns 70 Hz em vez de 1000, e contar
ticks dava 19 ms para um laco de 300 ms. `sigtest` reescrito contra a musl
(67 verificacoes, cada grupo num filho com prazo; escrito por um subagente a
partir da especificacao do Linux, em paralelo com o kernel); o da libc
propria saiu.
Tres rodadas de revisao antes do commit, cada achado conferido por dois
revisores. A primeira achou 15 problemas, o mais serio uma corrida em que outra
thread trocava o handler entre a validacao e o uso e o `sysret` voltava para
endereco nao canonico (#GP em ring 0 com pilha do usuario). A verificacao das
correcoes achou mais 8 (restart block sobrevivendo a um handler, FPU
desalinhado no quadro, DF no handler...). A regressao achou um panic: o
Ctrl+C do terminal manda `kill` de dentro do kernel, onde nao ha processo
atual. Tudo corrigido.
Regressao 179 -> 196, estresse 5x verde.
Pendente: um NMI na janela antes do `swapgs` (fase 8); `ITIMER_VIRTUAL` e
`ITIMER_PROF`; relogios de CPU no `clock_nanosleep`.

#### 1.9 dash, lua, sbase - 05:37 as 06:28 - FEITO
Tres programas de verdade, compilados para Linux e sem mudar uma linha deles:
`dash` 0.5.13.5, `lua` 5.4.9 e o `sbase` (commit `c546c3a`, 97 utilitarios
num multicall `sbase-box`; 29 deles copiados em `/bin`). Fontes conferidas por
SHA-256 (e, no sbase, pelo hash do commit e da arvore) em `third_party`.
O `build.txt` ganhou `gen`, `gensrc`, `@gen`/`@root` e `link`; os arquivos que
o `configure`/`make` deles geraria saem de `tools/dash_gen.py` (`config.h` para
a musl, tabelas de tokens e de builtins, nomes dos sinais, e os geradores em C
do proprio dash rodando no host) e `tools/sbase_box.py`. O build passou a
precisar de Python 3 (`tools/python.ps1` acha; `setup.ps1` instala).
Kernel: `faccessat2`. O `sh` proprio ganhou `-c` (o `system()`/`popen()` da
musl, usados pelo `os.execute` do lua, chamam `/bin/sh -c`). Criterios do
plano: `for i in 1 2 3` no dash e `lua -e 'print(2^10)'` passam; a regressao
entra no dash e testa laco, substituicao de comando, aritmetica, funcao,
`case`, `while read`, `test -x`, `os.execute` do lua, `seq`/`paste`/`tr`/`cut`/
`find` do sbase e o codigo de saida. A pesquisa das fontes rodou num subagente
em paralelo. Uma revisao achou 12 problemas (carimbo do gerador por data,
links nao recopiados, `sh -c` cortando comandos longos...), corrigidos. Achado
no caminho: o `rev` do sbase nesse commit esta quebrado no upstream (fica fora
de `/bin`).
Regressao 196 -> 207, estresse 5x verde.
Pendente: o escalonador nao acorda CPU ociosa quando entra trabalho na fila
(um filho novo espera o proximo tick; no TCG uns 15 ms); o `dash` fica sem
edicao de linha (`SMALL`, sem libedit).

#### 1.10 Doom - 06:28 as 07:10 - FEITO
Doom pelo `doomgeneric` (commit `e257a09`, intocado em `third_party`) com o
Freedoom 0.13.0 (`freedoom1.wad`, baixado e conferido por `tools/freedoom.ps1`
ou `tools/freedoom.py`, fora do git). Nosso e so a plataforma
(`user/musl/doom/doomgeneric_orion.c`): framebuffer por `mmap` do `/dev/fb0`
com escala inteira e conversao pelas mascaras de cor, teclado por
`/dev/input` com `poll`, e `-orionquit <s>` para a regressao. Roda a demo a
uns 38 quadros por segundo no QEMU TCG (limite do jogo: 35). A sequencia B
roda o Doom 30 s, tira a captura e confere que ela tem a cena do jogo
(91 cores numa amostra; tela preta ou console daria poucas).
A revisao achou que o teclado nao funcionava no jogo: o driver nao mandava
Shift/Ctrl/Alt/Caps para `/dev/input` (correcao no kernel, que tambem passa a
tratar Ctrl/Alt da direita como modificador do terminal) e a plataforma lia a
flag de tecla estendida errado. Corrigido e conferido no QEMU: Enter abre o
menu, setas andam, Ctrl atira. A sequencia A confere Ctrl e seta no `evtest`.
Outros: tecla do jogo por tabela fixa (soltar com Shift prendia a tecla),
WAD tambem no Linux/CI, `__pycache__` fora do git.
Regressao 207 -> 213, estresse 5x verde.
Pendente: a tecla Pause (sequencia E1 que o driver nao trata; Scroll Lock
faz a pausa); som.

#### 1.11 tcc dentro do Orion - 07:10 as 07:54 - FEITO
TinyCC (ramo `mob`, commit `e23dabe`) compila C dentro do Orion: `tcc
/lib/tcc/hello.c -o /tcchello && /tcchello` imprime a mensagem, e um programa
escrito na hora pelo dash devolve o codigo de saida certo. `tools/tcc_gen.py`
monta o que o tcc precisa no sistema (3,3 MB): cabecalhos e `libtcc1.a` do
tcc, cabecalhos da musl que vem com o Zig, `crt1.o` e uma `libc.a` completa
(a do Zig 0.16 precisou receber os objetos da `libzigc` e da `compiler_rt`,
para onde foram `memcpy`, `strlen` e a matematica), e o `build.txt` ganhou
`install` para levar isso ao ESP. O tcc mostrou o `mremap` faltando (o
`realloc` grande da musl usa): virou syscall de verdade (encolhe, cresce no
lugar ou move as paginas), testada no `mhello`. A pesquisa (versao certa do
tcc, o que falta na libc do Zig) rodou num subagente antes. A revisao achou 8
problemas, 6 corrigidos (`mremap` que podia falhar no meio, parser do link do
Zig com espacos, `install` que nao apagava sobras...).
Regressao 213 -> 218, estresse 5x verde.
Pendente: `mremap` com `MREMAP_FIXED` ou em area de arquivo; panico sem
memoria para tabela de pagina (8.4).

#### 1.12 Programas proprios na musl - 07:54 as 08:50 - FEITO (fecha o M1)
Os 53 programas de `user/progs` foram para `user/musl` e a libc propria
(`user/lib`) e o `user/linker.ld` sairam do repositorio, do `build.ps1`, do
`check.ps1`, do `Makefile` e do CI. A conversao rodou em seis subagentes em
paralelo (shell, wm, testes de syscall, testes de processo e dispositivo, rede
e sistema, utilitarios), cada um compilando o seu lote contra a musl sem
nenhum aviso; a integracao e a regressao foram feitas depois. A interface
propria do Orion virou `user/include/orion.h` (framebuffer, `/dev/input`,
`sysinfo_orion`/`proclist_orion`, `orion_openpty`, ioctls de rede, IP em ordem
de host e as mensagens de erro em portugues da libc antiga). A regressao
inteira passou na primeira rodada com os programas novos. A revisao achou 5
problemas, corrigidos: dois testes do `filetest` que a musl emulava sozinha
(passariam com o kernel quebrado) agora usam syscall crua, o `forktest` ganhou
um teste do heap do `brk` atraves do `fork`, e mudar o `orion.h` recompila os
programas.
Regressao 218 -> 218 (mesmas verificacoes, agora sobre a musl), estresse 5x
verde. Versao 2.0.0 e tag `v2.0`: fases 0 e 1 completas, marco M1 ("rodar
`dash`, `lua`, `tcc` e Doom compilados para Linux, sem mudar uma linha deles").

#### 2.1 VFS com vnodes e montagens - 08:51 as 11:05 - FEITO
O VFS de caminho fixo (prefixos `/dev` e `/mnt` no codigo, copia do no em
cada descritor, `..` resolvido como texto) virou uma camada de vnodes: cache
com contagem de referencia, tabela de montagens com `mount`/`umount` (165/166),
caminho resolvido componente a componente com `..` atravessando montagens,
processo com o vnode do diretorio atual. FAT e devfs viraram tipos de sistema
de arquivos montados pela VFS nova (raiz, `/dev`, e o segundo disco em `/mnt`
no boot ou com `/bin/mount`); `/bin/mount`, `/bin/umount` e `vfstest` sao
novos, e `pread64`, `pwrite64`, `truncate` e `fchdir` entraram. No FAT:
`rename` entre diretorios e por cima de destino, arquivo apagado continua
legivel enquanto aberto, buracos leem zeros, `O_APPEND` atomico, posicao
estavel no `getdents64`. Foram quatro rodadas de revisao adversarial (a cada
rodada, os achados conferidos por outro agente e as correcoes revisadas na
seguinte): 16 problemas reais na primeira (dois corrompiam o disco), 10 na
segunda, 6 na terceira e 2 na quarta, todos corrigidos e com teste no
`vfstest` quando dava para provocar.
Regressao 218 -> 243, estresse 5x verde. Versao 2.1.0-a1.
Pendente: o mutex nao e justo e, sem IPI para acordar CPU ociosa, o
`bdflush` chega a esperar 10 s atras de escritas seguidas no FAT; na partida
do estresse o `exec` simultaneo de 64 processos espera a trava do FAT (le o
ELF inteiro com ela; resolve na 2.4); `/proc/mounts` (o `mount` sem argumentos
ja le dele) chega na 2.3.

#### 2.2 tmpfs - 11:06 as 11:47 - FEITO
`tmpfs` montado no boot em `/tmp` e `/run` e pelo `mount -t tmpfs` (opcoes
`size=` e `nr_inodes=`). Dados numa arvore radix de altura variavel (arquivo
de uma pagina custa uma pagina; arquivo esparso de 256 GiB custa so as
tabelas do que foi escrito), cada pagina contada no limite da montagem e
recusada quando a memoria livre cai abaixo de 1/16 da RAM. Arquivo apagado e
aberto guarda as paginas ate o ultimo `close`. O criterio da etapa passa
exato: o `tmptest` cria e apaga 1000 arquivos duas vezes e as paginas livres
voltam ao mesmo numero. `vfstest /tmp` passa inteiro. Duas rodadas de
revisao: 7 problemas reais na primeira (o vetor de paginas fora do limite, a
recursao do `umount` que estourava a pilha, `rename` que fazia o `readdir`
nao terminar...), 2 na segunda (os dois tmpfs juntos podiam esgotar a RAM e
derrubar o kernel; tabelas sobrando depois de escrita que falha).
Regressao 243 -> 254, estresse 5x verde. Versao 2.1.0-a2.
Licao de processo: rodar o `test.ps1` enquanto a regressao roda derruba o
QEMU da regressao (as portas 4444/4445 sao as mesmas); uma sequencia C
"falhou" por isso.

#### 2.3 procfs - 11:48 as 12:52 - FEITO
`/proc` montado so leitura no boot: `/proc/<pid>/{status,cmdline,maps,stat,fd/}`,
`self`, `meminfo`, `cpuinfo`, `uptime`, `loadavg`, `mounts`, `version`. O
conteudo e gerado inteiro no `open`. Ler outro processo pediu pinos no
processo (a liberacao espera o ultimo leitor) e VMAs trocadas sempre com
`mm_lock` (o `exec` e a saida nao pegavam). `ps`, `top`, `free`, `uptime`, a
janela de processos do `wm` e os testes de memoria leem do `/proc` pelo
`orion.h`; as syscalls 1002 e 1003 sairam. `proctest` novo; o criterio da
etapa (`cat /proc/self/maps` mostra as VMAs) esta na regressao. Duas rodadas
de revisao: 6 problemas reais na primeira (uso depois de liberar no
`/proc/<pid>/fd` de processo saindo, ponteiro de pai pendurado, erro errado
no `open`, ordem do `ps`...), 2 na segunda (lista com mais de 256 processos,
nome de executavel forjando campos).
Regressao 254 -> 263, estresse 5x verde. Versao 2.1.0-a3.

#### 2.4 Cache de paginas unificado - 12:53 as 14:55 - FEITO
Cada vnode tem um cache de paginas (radix de paginas fisicas); o FAT oferece
`readpage` e passa a ser lido so por ele: `read`, o carregamento do ELF no
`exec` e o `mmap` de arquivo usam as mesmas paginas (o `mmap` mapeia a pagina
do cache com copia na escrita). Escrita e truncate atualizam o cache dentro da
trava do FAT. Vnodes com cache ficam numa LRU depois do ultimo `close` (ate
512 ou 1/4 da RAM), despejados sob pressao de memoria e no `umount`.
`/proc/diskstats` conta as leituras reais de cada disco; o `pctest` le o
mesmo arquivo duas vezes e confere que a segunda nao vai ao disco (para isso
os clusters de diretorio passaram a usar o cache de blocos). O `mem` do shell
do kernel mostra as paginas usadas sem o cache. Quatro rodadas de revisao: 8
problemas reais na primeira (contador de referencia de 16 bits estourando
com 65536 `mmap`, atualizacao do cache fora da trava do FAT, umount com
`EBUSY` a toa...), 4 na segunda (o `fork` ainda estourava o contador, que
virou de 32 bits), 2 na terceira e 1 na quarta (desempenho).
Regressao 263 -> 271, estresse 5x verde. Versao 2.1.0-a4.
Observado: numa rodada do estresse o QEMU demorou mais de 60 s para mostrar o
prompt e o harness digitou o comando antes de o shell ler (os bytes se
perderam); o boot terminou normal e a rodada repetida passou inteira.

#### 2.5 ext2 leitura, piloto em Rust - 14:56 a 16:36
O leitor de ext2 e o primeiro Rust do kernel: `kernel/rust/ext2`, `staticlib`
`no_std` para `x86_64-unknown-none`, ligada pelo `build.ps1` e pelo `Makefile`;
o `check.ps1` roda `cargo clippy -D warnings`, procura comentario nos `.rs` e
roda o `cargo test` do crate no hospedeiro. A ponte com a VFS esta em
`kernel/src/fs/ext2.c` (vnode por inode, `readpage` no cache de paginas,
`readdir` com cookie, `readlink`, montado so leitura); `readlink` passou a
funcionar. `tools/mkext2.py` gera imagens ext2 (o Windows nao tem `mkfs.ext2`),
conferidas pelo 7-Zip; `test.ps1 -ExtraDisks` liga imagens extras ao QEMU, e o
`regress.py` faz o mesmo. A regressao C monta duas imagens (blocos de 4 KiB e de
1 KiB com 4 grupos, indireto triplo, esparsos, links rapido e lento) e roda o
`ext2test` e um binario direto do ext2. Duas rodadas de revisao (a imagem e
tratada como hostil): 14 problemas reais na primeira e 7 na segunda, todos
corrigidos; o teste no hospedeiro monta e percorre centenas de imagens com bytes
trocados ao acaso, com checagem de estouro, sem nenhum `panic`. Avaliacao do
piloto em `docs/DECISOES.md`: o Rust continua (build de 0,8 s, ~12 KB no
kernel, simbolos e `panic` funcionando, parser testavel fora do kernel).
Fechada pelo dono depois da pausa: regressao inteira verde e commit junto com a
parte 1 da 2.6.

#### 2.6 ext2 escrita, mkfs.ext2, fsck - 16:36 a 19:00
Feita fora do intensivo, em tres partes, cada uma com regressao verde e commit
proprio. Parte 1: montagem com escrita por padrao, `write` e `truncate` em
arquivo existente (alocacao de blocos e tabelas indiretas, esparsos, contadores
gravados no `sync` e no `umount`). Parte 2: `create`, `mkdir`, `unlink`,
`rmdir` e `rename`, arquivo apagado aberto liberado so no ultimo `close`,
diretorio indexado perde o indice ao mudar. Parte 3: `mkfs.ext2` e `fsck.ext2`
em `user/musl` (C, leem e gravam o dispositivo por `lseek` + `read`/`write`,
porque `pread` em dispositivo de bloco da `ESPIPE` no Orion), e a regressao C
passou a conferir com o `fsck.ext2` as duas imagens depois do `ext2test`,
formatar o `sdd` com o `mkfs.ext2`, rodar o `fstest -a` nele (o criterio da
etapa) e conferir de novo (+10 verificacoes). Validacao no hospedeiro: as
imagens do `mkfs.ext2` passam no `e2fsck -fn` oficial em 15 tamanhos e blocos
de 1, 2 e 4 KiB; o `fsck.ext2` concorda com o `e2fsck` em 70 imagens limpas,
corrige 12 tipos de corrupcao feitos com o `debugfs` (e o `e2fsck` concorda
depois), e o fuzz com ASan achou e ajudou a corrigir 3 erros dele; a rodada
final, com o codigo entregue, passou por 480 imagens mutadas sem erro de
memoria, sem travar e sem precisar de duas passadas para corrigir. Detalhes em
`docs/DECISOES.md`.

#### 8.7 memcpy, memset e memmove em assembly - 20:05 a 20:20
Feita fora do intensivo, como aquecimento antes da 2.7. As tres funcoes sairam
do `lib/string.c` (onde ja usavam `rep movsb`/`rep stosb` por assembly embutido)
para `arch/x86_64/mem.S`; o `memmove` com sobreposicao para tras usa `std`,
`rep movsb` e `cld`, em vez do laco de bytes em C. `string.c` e `string.h`
perderam os comentarios antigos. Testado no hospedeiro contra uma referencia
em 200 mil casos (tamanho 0, desalinhado, sobreposicao para os dois lados,
copias de ate 64 KiB), conferindo retorno e flag de direcao.
Validada no QEMU na sessao 2 (regressao inteira verde com 291 verificacoes,
estresse 5x verde) e commitada em `9ad91e7`.

#### 2.7 links simbolicos, chmod, chown, utime - 20:20 a 22:40 - FEITO
Parte 1 (links simbolicos): a VFS segue links na resolucao de caminho (40 no
maximo, `ELOOP`), `symlink`/`symlinkat`, `lstat` e `readlink` sem seguir o
ultimo link, `O_NOFOLLOW`, `O_CREAT` por link pendurado; links no tmpfs e no
ext2 (curto no inode, longo num bloco). Programa novo `linktest` na regressao
(tmpfs e ext2) e `ln -s` do sbase. Detalhes em `docs/DECISOES.md`.
Sessao 2 (21:40 a 22:05): primeira vez no QEMU. `check.ps1` limpo, regressao
inteira verde (291 verificacoes, 3 novas: `linktest` no tmpfs e no ext2 e
`ln -s`/`readlink` do sbase; o `fsck.ext2` final passou a vir depois do
`linktest`), estresse 5x
verde; nada precisou de correcao. Commit "v2.1: 2.7 (parte 1) - links
simbolicos" (`921c465`).

Parte 2 (sessao 2, 22:07 a 22:40): `getattr`/`setattr` na VFS; `chmod`,
`fchmod`, `fchmodat`, `fchmodat2`, `chown`, `fchown`, `lchown`, `fchownat`,
`umask`, `utime`, `utimes`, `futimesat` e `utimensat`; `stat` com modo, dono,
grupo, `st_nlink`, `st_blocks` e datas reais. O ext2 grava no inode (Rust:
`ext2_setattr`, dono de 32 bits); o tmpfs guarda tudo em nanossegundos e conta
paginas e subdiretorios; o FAT da as datas da entrada e grava datas e o bit de
somente leitura (o resto da `EPERM`, como o `vfat`). `open`/`mkdir` respeitam
o modo pedido e o `umask`; as opcoes `mode=`, `uid=` e `gid=` do tmpfs passaram
a valer. Programas: `ls -l`, `touch` que atualiza as datas, `chown` e `chgrp`
do sbase, `/etc/passwd` e `/etc/group` com o root, `attrtest` (novo). Teste
novo no crate (atributos sobrevivem a remontagem em imagens do `mkext2.py` e do
`mke2fs`) e `ext2_setattr` no fuzz de escrita. O `build.ps1` recompila o
`main.c` sempre que algo do kernel e recompilado, para a hora na linha de boot
valer como prova do kernel novo. Versao 2.1.0-a7.
Regressao 291 -> 300 verificacoes: `attrtest` no tmpfs (e `-k`), `ls -l` com
dono numerico, com nome do `/etc/passwd` e com data do FAT, `attrtest` no ext2
com `umount`/`mount` no meio e `fsck.ext2` limpo depois, `chmod 444` e `EPERM`
no FAT. Estresse 5x verde.
Problemas: (1) o `filetest` conferia `st_blocks == (tamanho + 511) / 512`, que
era o valor inventado de antes; agora o FAT informa os clusters alocados, como
o Linux, e o teste confere que o valor cobre o tamanho com no maximo 64 KiB de
sobra. (2) A sequencia A passou do limite de memoria (434 -> 452 paginas,
limite 16). Medido a parte: 434 -> 436 depois da primeira rodada de
`attrtest`/`ls -l`/`chown` e 436 de novo depois de mais 10 rodadas, entao e
custo fixo dos comandos novos (slabs e cache), nao vazamento; o limite da A
foi para 24, o mesmo da C. (3) O primeiro script de edicao do `syscall.c` foi
cortado pelo Bash (heredoc com aspas e barras); nada foi aplicado pela metade,
e os scripts passaram a ir para arquivo no scratchpad.
Ficou de fora: conferir permissao (todo processo e root ate a 4.1), `atime`
na leitura (comportamento de `noatime`), `chmod`/`chown` no devfs e no procfs
(`EPERM`), links fisicos (`link`, nao estao no plano).

#### 2.8 GPT e raiz em ext2 - 22:42 a 23:25 - FEITO (fecha o M2)
O kernel le GPT (MBR protetor, cabecalho e vetor de entradas com CRC32, copia
de reserva no fim do disco quando o principal esta ruim) e guarda o tipo de
cada particao. A raiz e a particao GPT de raiz Linux x86-64 montada como ext2;
sem ela, vale o caminho antigo (primeira particao FAT, o `vvfat`). Com a raiz
em ext2, a ESP do mesmo disco vai para `/boot/efi`. `tools/mkimage.py` (novo)
gera o `out\orion.img` no hospedeiro: GPT, ESP de 64 MiB em FAT16 escrita pelo
proprio script (Limine e `orion.elf`) e a raiz em ext2 pelo `mkext2.py` com o
que o build pos no `out\esp`, mais os pontos de montagem. `build.ps1 -Image`,
`-Run -Image`, `test.ps1 -Image` e `qrun.py --image`; os dois harness ganharam
o passo `reboot:`. A regressao inteira (A a E) passou a bootar da imagem: o
`vvfat` saiu da rotina. Verificacoes novas na A: GPT com as duas particoes,
raiz ext2 montada com escrita, ESP em `/boot/efi`, `ls -l` na raiz (binario
`0755`) e no FAT da ESP, e um arquivo gravado na raiz que sobrevive a um
reboot (o criterio da etapa). Regressao 300 -> 307, estresse 5x verde.
Versao 2.1.0, tag `v2.1`.
Problema achado: o ext2 montava somente leitura numa particao, porque o
`ext2v_mount` olhava `dev->write`, que e nulo em particao (a escrita passa
pelo disco pai); ate aqui ele so tinha sido montado em disco inteiro.
`blockdev_writable` resolve o disco de baixo, e o FAT passou a usar a mesma
funcao. Fora isso a regressao inteira passou de primeira na raiz ext2, o que
exercitou o ext2 com todos os testes que escrevem em `/`.
Ficou de fora: opcao `root=` na linha de comando (a raiz e escolhida pelo tipo
da particao), ESP em FAT32 (a especificacao UEFI pede FAT32; o QEMU e o Limine
aceitam FAT16, a 7.3/7.4 trocam se algum firmware recusar) e desligar limpo
(sem `poweroff` ate a 5.1, o ext2 fica marcado como nao desmontado; o `sync`
antes de fechar garante os dados).

#### 3.1 TCP parte 1 - 23:27 a 23:52 - FEITO
Primeiro Rust novo depois do ext2. `kernel/rust` virou um workspace: `ext2` e
`tcp` sao `rlib` e `orion` e o unico `staticlib` (com o alocador e o `panic`
do kernel, antes no ext2), porque dois `staticlib` duplicariam `core`,
alocador e `panic`. `build.ps1` e `Makefile` ligam o `liborion_rust.a`. O
crate `tcp` faz cabecalho (opcoes puladas), checksum com pseudo-cabecalho,
tabela de conexoes (exata, depois quem escuta) e `RST` para porta fechada
pela RFC 793; o C entrega so o que e para o IP da interface, chama o Rust sob
um mutex e manda a resposta depois de solta-lo (o envio pode dormir no ARP).
Contadores no comando `net`. Testes no crate: checksum contra uma
implementacao simples em 200 tamanhos, campos e erros do `parse`, opcoes,
casos do `RST` (com e sem `ACK`, FIN com dados e volta no 2^32, `RST` nunca
responde), tabela, e 20 mil segmentos aleatorios sem `panic`. Criterio ("o
hospedeiro ve a porta fechada") sem `nmap`: `test.ps1 -HostFwd` e o passo
`tcpprobe:` (tambem no `qrun.py` e no `regress.py`) conectam do hospedeiro
por `hostfwd`; conferido tambem no `out\net.pcap` que os `RST|ACK` saem com
`seq 0`, `ack` certo e checksum valido. Regressao 307 -> 309 (sondagem da
porta 7 e contadores do TCP), estresse 5x verde. Versao 2.2.0-a1.
Problemas: o clippy novo recusou `chunks_exact(2)` (quer `as_chunks`), e
`Box::try_new` ainda e instavel (a pilha TCP e alocada com
`alloc::alloc::alloc` e conferida contra nulo).

#### 3.2 TCP parte 2 - 23:53 a 01:00 - FEITO
O crate `tcp` ganhou conexoes ativas (do `SYN_SENT` ao `TIME_WAIT`), buffers
de 32 KiB nos dois sentidos, opcao MSS, recepcao em ordem com ACK de tudo,
janela do outro lado pela regra `SND.WL1/WL2` e ACK de atualizacao quando a
leitura reabre a janela. O C ganhou sockets `SOCK_STREAM` (`connect`,
`read`/`write`, `send`/`recv`, `poll`, `close`), uma espera por contador de
eventos que nao perde acordar, e o `net.c` chama o `tcp_output` da conexao
depois de cada segmento recebido. Programa novo `httpget`; `tools/hostweb.py`
serve uma pagina e 1 MiB fixo no hospedeiro (o convidado chega pelo
10.0.2.2), e a regressao tambem baixa `example.com` (o criterio do plano).
`sendmsg`/`recvmsg` e `/etc/resolv.conf` entraram para o DNS da musl.
Testes no crate: handshake, envio dentro da janela e do MSS com o seq dando a
volta no 2^32, janela zero, recepcao com fora de ordem e retransmissao
sobreposta, janela de recepcao cheia e reaberta, fechamento ativo e passivo,
`RST` e recusa, portas efemeras, e 200 conexoes recebendo segmentos
aleatorios sem `panic`. Regressao 309 -> 314, estresse 5x verde. Versao
2.2.0-a2.
Problemas: (1) um estouro (0 - 1) no calculo dos bytes em voo depois de o
`FIN` ser reconhecido, que os testes nao pegavam: achado lendo o codigo,
corrigido e com teste novo. (2) A primeira regressao achou o `recvmsg` (47)
faltando: o `getaddrinfo` da musl usa e o `example.com` nao resolvia. (3) O
`tmptest` falhou por ruido: ele media o tmpfs pelo `MemFree`, que muda quando
o cache de binarios da raiz ext2 e despejado e relido; a medida virou
`MemFree + Cached` (ver `DECISOES.md`). (4) O `wc` proprio nao tem `-c`; o
teste usa `wc` sem opcao. (5) O clippy novo pediu `is_multiple_of`.
Ficou de fora: retransmissao (o `connect` para porta fechada do hospedeiro
espera 20 s, porque o SLIRP do Windows nao responde), `SIGPIPE`,
`bind`/`listen`/`accept` em TCP (3.4).
Plano da 3.3: `now` em milissegundos passado pelo C ao `input`, `output` e
`connect`, e um `tcp_tick` chamado por uma thread do kernel a cada 50 ms; RTO
com RTT de Jacobson/Karels e Karn, backoff, desistencia com `ETIMEDOUT`;
retransmissao do `SYN`, dos dados a partir de `SND.UNA` e do `FIN`;
retransmissao rapida com 3 ACKs repetidos e Reno (`cwnd`, `ssthresh`); fila
de reordenacao na recepcao; sonda de janela zero; `TIME_WAIT` com 2*MSL. Para
ter perda de verdade pelo SLIRP, um comando `tcploss N` no shell do kernel
que descarta 1 em N segmentos TCP nos dois sentidos; a regressao baixa 10 MiB
com e sem perda e confere o CRC32.

#### 3.3 TCP parte 3 - 01:02 a 01:45 - FEITO
Retransmissao com RTO da RFC 6298 (Jacobson/Karels, Karn, backoff, de 200
ms a 60 s), volta a `SND.UNA` com `SND.MAX` separando envio novo de
retransmissao, `SYN` e `FIN` retransmitidos, desistencia com `ETIMEDOUT`
(5 tentativas no `SYN`, 8 nos dados), Reno (`cwnd` inicial de 10 MSS,
retransmissao rapida com 3 ACKs repetidos), reordenacao na recepcao, sonda de
janela zero e `TIME_WAIT` de 2*MSL. O relogio vem do C (`now` nas chamadas e
uma thread `tcptimer` chamando `tcp_tick` a cada 50 ms). Comando `tcploss N`
no shell do kernel descarta 1 em N segmentos TCP, porque o SLIRP nao perde
nada; `hostweb.py` ganhou `PUT` (responde tamanho e CRC32) e o `httpget`
ganhou `-u`. Criterio: 10 MiB do hospedeiro com o CRC32 certo (0,7 s); com
1 em 10 descartado, 1 MB enviado e 1 MiB recebido inteiros. 10 testes novos
no crate, incluindo duas simulacoes com perda aleatoria de ate 30% conferindo
o conteudo inteiro, e todos passaram de primeira. Regressao 314 -> 318,
estresse 5x verde. Versao 2.2.0-a3.
Problemas: (1) revisando o rascunho antes de aplicar, o reconhecimento do
`FIN` dependia de `fin_sent`, que volta a falso depois de um RTO, e o
`take_ooo` usava um indice calculado antes de um `retain`; os dois corrigidos
antes do primeiro teste. (2) O `tmptest` continuou falhando por ruido de
paginas de slab (ver `DECISOES.md`); a tolerancia foi para 256 paginas e ele
mostra os componentes. (3) O heredoc do Bash estragou mais tres scripts com
barra invertida ou aspas soltas (nada aplicado pela metade); scripts com
esse tipo de texto passaram a ir sempre por arquivo.
Ficou de fora: ACK atrasado (cada segmento recebe ACK na hora), SACK, escala
de janela e timestamps (a janela fica em 32 KiB), keepalive.

#### 3.4 TCP parte 4 - 01:48 a 02:25 - FEITO
Abertura passiva no crate `tcp` (`LISTEN`, `SYN_RECEIVED`, `SYN|ACK`
retransmitido, fila de `accept` com `backlog`, ISS sorteado), e no C `bind`
TCP com conferencia de porta e `SO_REUSEADDR`, `listen`, `accept`/`accept4`,
`shutdown`, `getsockname`, `getpeername`, `setsockopt`/`getsockopt` e
`select` sobre o `poll` (o laco do `poll` virou funcao interna). Programa
novo `httpd` (GET de arquivos e diretorios, `select` com prazo, `accept4`,
`getpeername`, `shutdown` com `SHUT_WR`). Passo novo `hostget:` nos harness:
GET feito do hospedeiro por `hostfwd`, anotado no log. Criterio: o `httpd` do
Orion responde ao hospedeiro (200, 404 e lista de diretorio), e um segundo
`httpd` reabre a porta 80 com as conexoes do primeiro em `TIME_WAIT`. 6 testes
novos no crate (passaram de primeira). Regressao 318 -> 324, estresse 5x
verde. Versao 2.2.0-a4.
Problema: os `httpd &` da regressao gastaram os numeros de trabalho [1] e [2]
do `sh`, e os testes de `jobs` que vem depois falharam. O `sh` numerava
sempre para frente; passou a numerar como o bash (o maior em uso mais 1, de
volta a 1 com a tabela vazia), e os dois padroes que dependiam da numeracao
antiga foram ajustados.
Ficou de fora: `pselect6`, `SO_RCVTIMEO`/`SO_SNDTIMEO`, `SO_KEEPALIVE`,
`connect` usando a porta do `bind`, `RST` (em vez de `FIN`) para conexoes nao
aceitas quando quem escuta fecha.

#### 3.5 Loopback e AF_UNIX - 02:26 a 03:10 - FEITO
Loopback: o `net_send_ip` para 127.0.0.0/8 ou para o proprio IP poe o pacote
numa fila que a thread de recepcao processa antes da placa; ping, UDP e TCP
em 127.0.0.1 funcionam (o TCP aceita 127.x como local e conecta sem rede
configurada). Sockets `AF_UNIX` `SOCK_STREAM` novos (`net/unix.c`): no
`S_IFSOCK` na VFS pelo `vfs_mknod` (tmpfs e ext2; o FAT recusa), `bind`,
`listen`, `connect`, `accept`, `socketpair`, `shutdown`, `poll`,
`getsockname`/`getpeername`, buffers de 32 KiB. Criterio: o `wm` escuta em
`/run/wm.sock` e o programa novo `wmctl` manda comandos (`term`, `procs`,
`files`, `list`, `ping`); a regressao D digita `wmctl` no terminal do `wm`
pelo passo novo `type:` dos harness e confere que o terminal foi aberto a
pedido. Programa novo `unixtest` (tmpfs e raiz ext2, inclusive 200 KB
passando de um processo filho). Regressao 324 -> 333, verde de primeira;
estresse 5x verde. Versao 2.2.0-a5.
Ficou de fora: `SOCK_DGRAM` Unix, caminho abstrato, passagem de descritores
(`SCM_RIGHTS`), credenciais (`SO_PEERCRED`).

#### 3.6 wget, nc, httpd, telnet - 03:11 a 03:35 - FEITO
Programas proprios: `wget` (so `http://`, HTTP/1.0, segue redirecionamentos,
`-O`, `-q`), `nc` (cliente, `-l`, `-z`, `-w`; fim da entrada vira
`shutdown(SHUT_WR)`) e `telnet` (recusa as opcoes, dobra o 255, `\r\n` no fim
da linha); o `httpd` e o da 3.4 e o `getaddrinfo` ja le o `/etc/resolv.conf`
desde a 3.2. Criterio: `wget http://example.com` salva o `index.html` com a
pagina. Tambem na regressao: 1 MiB do `hostweb` com o SHA-256 certo, 404,
`nc` pelo loopback e com o hospedeiro, `nc -z`, e `telnet` contra um `nc -l`
que oferece `ECHO`, conferindo a tela e os bytes que voltam (`IAC DONT ECHO`,
`oi\r\n`). Regressao 333 -> 342, verde de primeira. Versao 2.2.0-a6. Sem
estresse (a etapa nao mexe em trava, memoria nem processos).
Problema: o primeiro roteiro de teste usava aspas, `grep -c` e `od -An`; o
`sh` daqui nao trata aspas, o `grep` proprio nao tem `-c` e o `od` do sbase
nao aceita `-An`. Os testes foram escritos sem esses recursos (`echo` em vez
de `printf` com aspas, `od -b`).
Ficou de fora: HTTPS (sem TLS no sistema), `chunked` (o `wget` pede HTTP/1.0),
modo caractere e eco local no `telnet`, UDP no `nc`.

#### 3.7 DHCP fora do kernel - 03:36 a 04:15 - FEITO (fecha o M3)
O `net/dhcp.c` saiu do kernel, junto com o `ioctl` `SIOCDHCP_ORION`, o
desvio da porta 68 e o comando `dhcp` do shell do kernel. O programa `dhcp`
faz DISCOVER/OFFER/REQUEST/ACK por um socket UDP comum (porta 68,
`SO_BROADCAST`, que agora e exigido para mandar a broadcast, como no Linux),
configura a interface pelo `SIOCSIFCONF_ORION` e grava o `/etc/resolv.conf`.
O `netup` (novo) le o `/etc/network` (`eth0 dhcp`, `static` ou `off`) e o
`/etc/autostart` o chama no boot; o `ifconfig dhcp` chama o `/bin/dhcp`.
Criterio: o boot pega IP pelo DHCP de usuario, e nenhuma linha do DHCP do
kernel aparece. Regressao 342 -> 345, estresse 5x verde. Versao 2.2.0, tag
`v2.2`.
Problemas: (1) com o `netup` e o `dhcp` rodando antes do `sh`, o `sh` deixou
de ser o pid 2, e dois padroes (do `top` e do `procs`) fixavam esse numero;
passaram a aceitar qualquer pid. (2) O teste do `nc -l` da 3.6 falhou nesta
rodada: o servidor em segundo plano herda o terminal como entrada, e o
meio-fechamento automatico no fim da entrada fazia o cliente sair antes de
mandar. O `nc` passou a so meio-fechar com `-N`, como o do OpenBSD, e o teste
da ao servidor `/dev/null` de entrada.
Ficou de fora: renovacao da concessao DHCP, mais de uma interface.

#### 4.1 uid, gid e permissoes - 04:16 a 04:46 - FEITO
Credenciais por processo (`kernel/cred.c`: real, efetivo e salvo de usuario e
grupo, mais ate 32 grupos suplementares), as dez syscalls `set*`/`get*` que
faltavam com as regras do Linux, `setuid`/`setgid` no `exec` e permissoes
conferidas na VFS: busca em diretorio, `open`, criar, apagar e renomear (com
o *sticky* do `/tmp`), `exec`, `chdir`, `truncate`, `access` com os ids
reais, `chmod`/`chown`/`utimensat` so para o dono, `kill` so para o mesmo
usuario e `bind` abaixo de 1024 so para root. Arquivo novo fica com o
`euid`/`egid` (e o grupo do diretorio quando ele tem `S_ISGID`). O
`/etc/shadow` entrou na imagem como `0600`. Criterio: o `permtest` (novo)
vira o usuario 1000 e confere umas 35 regras, inclusive "usuario comum nao le
o `/etc/shadow`", no tmpfs e na raiz ext2; `id` (novo) mostra os ids.
Regressao 345 -> 351, estresse 5x verde. Versao 2.3.0-a1.
Problemas: (1) com o `exec` exigindo um bit `x`, os binarios do FAT (o
`vvfat` do `build.ps1 -Run`) parariam de rodar, porque o FAT mostrava
arquivos como `0644`; passaram a `0755`, como o `vfat` do Linux, e o teste do
somente leitura do FAT virou `chmod 555`/`755`. (2) O `cp` criava tudo
`0644`; um binario copiado deixava de ser executavel. Passou a copiar os bits
da origem.
Ficou de fora: `nosuid` na montagem, capacidades (root e tudo ou nada),
`setfsuid`.

#### 4.2 passwd, shadow, login - 04:46 a 05:28 - FEITO
`login`, `su` e `passwd` (um programa so, `user/musl/login`, que escolhe o
papel pelo nome) com o `crypt` SHA-512 (`$6$`) e o `getspnam` da musl; o
`init` sobe o `login` e o reinicia quando o shell termina. `/etc/passwd` e
`/etc/shadow` trazem `root` e `orion` (uid 1000, grupo `users`,
`/home/orion`), senha `orion` nos dois, hashes gerados pelo
`tools/mkpasswd.py` (novo, conferido contra o vetor da especificacao). `su` e
`passwd` sao `setuid` root na imagem; o `passwd` regrava o arquivo num
temporario com o mesmo dono e modo, faz `fsync` e `rename`. Entrou o
`/dev/urandom` (sal). O `test.ps1` e o `qrun.py` respondem ao `login:`
sozinhos (`-LoginUser`/`-LoginPassword`). Criterio: o boot pede usuario e
senha, e a senha nao aparece na tela. Regressao 351 -> 369, estresse 5x
verde. Versao 2.3.0-a2.
Problemas: (1) o `getgrouplist` da musl devolve o grupo primario duas vezes
quando o usuario tambem esta listado nele no `/etc/group`; o `id` mostrava
`grupos=100(users),100(users)`. O `login` tira as repeticoes antes do
`setgroups`. (2) Na primeira versao o `/etc/shadow` regravado pelo usuario
`orion` ficava com o grupo `users` (o `egid` de quem roda o `passwd`); o
temporario passou a receber o dono, o grupo e o modo do original. (3) O
`grep` do sistema nao tem ancora `^`; o teste usa `grep orion:`. (4) O delta
de memoria da sequencia A passou de 24 paginas (25). Para separar cache de
vazamento, 120 `su` seguidos e 120 `passwd` seguidos (num `dash`, porque o
`sh` do sistema nao tem laco) deixaram o kernel em 423 paginas usadas do
comeco ao fim; o limite subiu para 32 por causa das slabs dos testes novos.
Ficou de fora: tela de login grafica (o `-Gui` continua entrando como root),
`/etc/login.defs`, envelhecimento de senha, `newgrp`.

#### 4.3 SMEP e SMAP - 05:28 a 05:55 - FEITO
O kernel liga SMEP e SMAP em cada CPU (CPUID folha 7); as duas rotinas de
`uaccess.S` abrem o acesso com `stac` e fecham com `clac` (tambem no caminho
de falha), e a entrada comum de interrupcoes faz `clac`. Um page fault de
ring 0 num endereco de usuario por busca de instrucao, ou com o `AC`
desligado, vira panico com a causa escrita ("SMEP: o kernel tentou executar
codigo de usuario", "SMAP: ... fora das copias de uaccess"), sem passar pelo
`vma_fault`. Syscall de teste `kfault` (1002, so root) e `crash smep` /
`crash smap`. O QEMU passou a `-cpu qemu64,+smep,+smap` no `build.ps1`, no
`test.ps1` e no `qrun.py`. Criterio: `crash smep` faz o kernel tentar
executar uma pagina de usuario e o resultado e o panico controlado (e o
mesmo para `crash smap`), na sequencia B, com um `reboot:` entre os dois.
Toda a regressao roda com SMAP ligado. Regressao 369 -> 373, estresse 5x
verde. Versao 2.3.0-a3.
Problemas: nenhum de comportamento; a regressao inteira passou com SMAP ligado na primeira rodada, o que confirma que a 0.2 tinha deixado todo acesso do kernel a memoria de usuario dentro do `uaccess`. O cuidado foi no tratador de page fault: sem o desvio novo, um acesso proibido a uma pagina presente iria para o `vma_fault`, que a acharia valida, e a instrucao repetiria a falta para sempre.
Ficou de fora: remendo de `stac`/`clac` em tempo de execucao (hoje um teste
de variavel por copia), UMIP, KPTI.

#### 4.4 ASLR basico - 05:55 a 06:26 - FEITO (fecha a Fase 4)
A cada `exec`, o topo da pilha desce de 0 a 2^22 - 1 paginas e a base de
busca do `mmap` sobe de 0 a 2^28 - 1 paginas (os mesmos 22 e 28 bits do
Linux), pelo `random_u64` do kernel. A base do `mmap` e do processo: o
`fork` copia, o `exec` sorteia e devolve a antiga se falhar, e a busca
recomeca do inicio da faixa quando nao ha espaco acima da base. Criterio:
dois `cat /proc/self/maps` seguidos mostram a pilha em enderecos diferentes;
o `aslrtest` (novo) roda a si mesmo 4 vezes e confere pilha e `mmap`
diferentes e dentro das faixas. O padrao antigo que fixava a pilha em
`7ffffffff000` virou uma faixa. Regressao 373 -> 375, estresse 5x verde.
Versao 2.3.0, tag `v2.3`.
Problemas: nenhum de comportamento. Um susto: o `proctest` rodado a mao
acusou o `cmdline`, mas era so o teste rodado sem os argumentos `um dois`
que a regressao passa.
Ficou de fora: executavel em posicao aleatoria (os programas sao `ET_EXEC`
estaticos; precisa de PIE), `brk` aleatorio, desligar o ASLR por processo.

### Sessao 2 - 2026-10-17

Ponto de partida (21:40): a arvore batia com o "Como retomar" (8.7 e 2.7
parte 1 sem commit, mais nada). `.\check.ps1` limpo. Primeiro trabalho:
validar no QEMU e commitar a 8.7 e a 2.7 parte 1 (entradas acima).
