# Intensivo Orion - progresso

Diario do trabalho sobre o `docs/PLANO.md`. Uma linha por etapa:
`[x]` feita, `[~]` em andamento, `[!]` bloqueada, `[ ]` nao comecada.
Este arquivo manda: uma sessao nova retoma da primeira etapa que nao esta `[x]`.

## Como retomar

    cd C:\dev\orion
    .\check.ps1          # precisa terminar com "Nenhum erro ou warning."
    .\regressao.ps1      # A (texto e rede), B (grafica), C (disco), D (GUI)
    git log --oneline -10

Estresse (etapas que mexem em trava, escalonador, memoria, processos, threads
ou sinais), 5 vezes seguidas:

    .\test.ps1 -Smp 4 -Commands @("stress 3000 16 | stress 3000 16 | stress 3000 16 | stress 3000 16")

Proxima etapa: 1.2.

## Checklist

| # | Etapa | Estado | Inicio | Fim | Commit |
| --- | --- | --- | --- | --- | --- |
| 0.1 | Commit da v1.9 e dos programas novos | [x] | 19:22 | 20:20 | `58bb747` |
| 0.2 | Copia segura de memoria de usuario | [x] | 20:22 | 22:15 | `8b3d171` |
| 0.3 | Mutex que dorme, AHCI | [x] | 22:16 | 22:41 | `4d55e72` |
| 0.4 | FAT e cache de blocos com mutex | [x] | 22:42 | 23:08 | `076285d` |
| 0.5 | Harness em Python e CI | [x] | 23:09 | 23:39 | `bc9a8e0` |
| 0.6 | Shell: `&`, jobs, fg, wait, `VAR=x cmd`, `&&`, `\|\|`, `;` | [x] | 23:40 | 00:00 | (ver diario) |
| 0.7 | Limites: 256 fds, 256 processos, ulimit | [x] | 00:00 | 01:09 | (ver diario) |
| 1.1 | Pilha inicial do Linux (auxv) | [x] | 00:55 | 01:29 | (ver diario) |
| 1.2 | Syscalls de partida da musl | [ ] | | | |
| 1.3 | Arquivos no formato do Linux | [ ] | | | |
| 1.4 | Memoria do jeito Linux (VMAs, mmap) | [ ] | | | |
| 1.5 | Programas contra a musl com `zig cc` (plano B: musl pelo build.ps1) | [ ] | | | |
| 1.6 | Threads (clone) | [ ] | | | |
| 1.7 | futex | [ ] | | | |
| 1.8 | Tempo e sinais completos | [ ] | | | |
| 1.9 | dash, lua, sbase | [ ] | | | |
| 1.10 | Doom | [ ] | | | |
| 1.11 | tcc dentro do Orion | [ ] | | | |
| 1.12 | Programas na musl, aposentar a libc propria | [ ] | | | |
| 2.1 | VFS com inodes | [ ] | | | |
| 2.2 | tmpfs | [ ] | | | |
| 2.3 | procfs | [ ] | | | |
| 2.4 | Cache de paginas unificado | [ ] | | | |
| 2.5 | ext2 leitura, piloto em Rust | [ ] | | | |
| 2.6 | ext2 escrita, mkfs.ext2, fsck | [ ] | | | |
| 2.7 | Links simbolicos, chmod, chown, utime | [ ] | | | |
| 2.8 | GPT e raiz em ext2 | [ ] | | | |
| 3.1 | TCP parte 1 | [ ] | | | |
| 3.2 | TCP parte 2 | [ ] | | | |
| 3.3 | TCP parte 3 | [ ] | | | |
| 3.4 | TCP parte 4 | [ ] | | | |
| 3.5 | Loopback e AF_UNIX | [ ] | | | |
| 3.6 | wget, nc, httpd, telnet | [ ] | | | |
| 3.7 | DHCP fora do kernel | [ ] | | | |
| 4.1 | uid/gid e permissoes | [ ] | | | |
| 4.2 | passwd, shadow, login | [ ] | | | |
| 4.3 | SMEP e SMAP | [ ] | | | |
| 4.4 | ASLR basico | [ ] | | | |
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
| 8.7 | memcpy/memset/memmove do kernel em assembly (rep movsb/stosb) | [ ] | | | |

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
