# Turno noturno - progresso

Checklist de uma linha por etapa. `[x]` feita, `[~]` parcial, `[ ]` nao comecada.
Este arquivo permite retomar o trabalho sem ler o historico da conversa.

Base do turno: commit `fe29851` (v1.0), `.\check.ps1` limpo, regressao verde.

| # | Etapa | Estado | Inicio | Fim | Commit |
| --- | --- | --- | --- | --- | --- |
| 4.1 | Filas de espera e poll (v1.1) | [x] | 22:52 | 02:35 | `5601c92` |
| 4.2 | Sinais (v1.2) | [x] | 02:35 | 14:55 | `80361d7` |
| 4.3 | Escrita no FAT e redirecionamentos (v1.3) | [x] | 14:55 | 20:10 | `a0da561` |
| 4.4 | Teclas estendidas e edicao de linha (v1.4) | [x] | 20:10 | 01:40 | `ff02fda` |
| 4.5 | Fork copy-on-write (v1.5) | [x] | 01:40 | 06:20 | `8e8b469` |
| 4.6 | Rede (v1.6) | [ ] | 06:20 | - | - |
| 4.7 | GUI: boot direto e apps (v1.7) | [ ] | - | - | - |
| 4.8 | Extras | [ ] | - | - | - |

## Como retomar

    cd c:\dev\orion
    .\check.ps1          # precisa terminar com "Nenhum erro ou warning."
    .\regressao.ps1      # sequencias A (texto), B (grafica) e C (disco); exit 0 = verde
    git log --oneline -5

A proxima etapa e a primeira linha com `[ ]` na tabela acima.

## Diario

### 4.1 Filas de espera e poll - 22:52 as 02:35 - commit 5601c92 - FEITO
`kernel/wait.{h,c}`: waitqueue com `struct wait_entry` na pilha do dormidor e
lista por thread (`t->waits`), o que permite dormir em varias filas (base do
poll). Ordem de travas do sistema: trava de dados -> `runq_lock`.
Migrados: disciplina de linha, PTY (master e slave), pipes, `waitpid` e
`/dev/input`. Nao sobrou nenhum `thread_sleep_ms(2..5)` de espera ocupada.
`SYS_POLL` (7) sobre `file_ops.poll`; `SYS_FCNTL` (72) com `O_NONBLOCK`; o `wm`
usa `poll` e nao usa mais o timeout via ioctl.
Dois problemas achados: (1) colher threads mortas dentro da `runq_lock` invertia
a ordem contra `proc_lock` do `waitpid` - a colheita foi movida para depois do
unlock em `sched_post_switch`; (2) uma fila de espera por processo exigiria
garantir a vida do `struct process` do pai na hora de colher o filho, entao
`waitpid` usa uma fila global unica de saidas.
Medido com o `wm` aberto e parado, 4 CPUs: 31 trocas de contexto em 3,2 s
(~10/s no total). Antes, cada leitor bloqueado acordava a cada 2 ms e o `wm` a
cada 20 ms.

### 4.2 Sinais - 02:35 as 14:55 - commit 80361d7 - FEITO
`kernel/signal.{h,c}`: pendentes, mascara, handlers por processo, quadro de
sinal empilhado na pilha do usuario (128 bytes abaixo do rsp, pulando a red
zone) e `rt_sigreturn`. Entrega no retorno de syscall, de IRQ e de excecao.
Syscalls: kill(62), rt_sigaction(13), rt_sigprocmask(14), rt_sigreturn(15),
alarm(37), setpgid(109), getpgid(121). O trampolim de retorno vem da libc
(`sa_restorer`), como no Linux.
Excecoes de ring 3 viram SIGSEGV/SIGILL/SIGFPE; `write` em pipe sem leitor gera
SIGPIPE; o pai recebe SIGCHLD. Chamadas bloqueantes devolvem `EINTR`.
Grupo de primeiro plano fica na disciplina de linha; `TIOCSPGRP` funciona no TTY
e no PTY; Ctrl+C manda SIGINT ao grupo. O `sh` usa grupos, refaz o `waitpid` em
EINTR e imprime `[morto por sinal N]`.
Status de saida passou ao formato do Linux (`WIFSIGNALED`/`WTERMSIG`).
Tres problemas achados: (1) o contador de interrupcoes de timer anda ~10x mais
devagar que o tempo real neste QEMU com as CPUs ociosas, esticando todo sleep e
alarm - `time_ticks()` agora sai do TSC; (2) `process_thread_reaped` zerava
`pml4_phys` fora do `proc_lock`, deixando um `waitpid` concorrente colher e
liberar o mesmo `struct process` (kfree duplo, so aparecia com o `wm` rodando,
que faz `waitpid(WNOHANG)` a cada volta); (3) uma edicao minha de `ldisc.c` nao
casou o padrao e falhou em silencio, deixando `interrupt_pgid` sempre 0 - por
isso o primeiro Ctrl+C nao matava nada.
Fora do escopo: zumbi orfao cujo pai morreu antes de colher fica registrado ate
o boot seguinte (aparece em `procs`); nao vaza paginas, so o `struct process`.

### 4.3 Escrita no FAT e redirecionamentos - 14:55 as 20:10 - commit a0da561 - FEITO
FAT com escrita completa: alocacao/liberacao de clusters (12/16/32), entradas de
diretorio com LFN, `write` com extensao, `truncate`, `unlink`, `mkdir` (com `.` e
`..`), `rename`, datas do RTC. Camada de blocos ganhou escrita (AHCI
`WRITE DMA EXT`) e um cache de 256 setores write-back; `bdflush` (no VFS)
descarrega FAT e cache a cada 2 s.
VFS: create/write/unlink/mkdir/rename/truncate/sync. `open` com O_CREAT/O_TRUNC/
O_APPEND. Syscalls mkdir(83)/rmdir(84)/unlink(87)/rename(82)/fsync(74)/
ftruncate(77)/sync(162). `fopen` com w/a/r+.
`sh` reescrito: redirecionamentos `>`, `>>`, `<`, `2>` e pipelines de N estagios.
Programas novos: mkdir, rm -r, cp, mv, touch, head, tail, grep, sort, uniq, tee,
mkfs, fstest.
**Limitacao do ambiente**: o `vvfat` do QEMU (`fat:rw:out/esp`) nao aguenta
escrita de verdade - aborta o proprio QEMU com
`check_directory_consistency: assertion failed` ao criar muitos arquivos, ou
falha com `Error handling renames (-2)`. Por isso entrou a opcao `-Disk`:
`out\disk.img` de 64 MiB cru, formatado pelo `mkfs /dev/sdb` dentro do Orion
(precisou de dispositivos de bloco em `/dev`) e montado em `/mnt` pelo comando
`mount` do shell do kernel. Nesse disco o teste pesado passa inteiro: 300
arquivos com nome longo, readdir de todos, e um arquivo de 3 MiB.
Dois problemas alem do vvfat: (1) gerar nome curto tentando `NOME~1`, `NOME~2`...
varre o diretorio por candidato e fazia criar 300 arquivos virar O(n^3) - trocado
por duas letras + hash do nome longo, como o Windows faz; (2) a opcao `-Disk`
inseria argumentos vazios na linha de comando do QEMU quando desligada, e o QEMU
saia sem mensagem - a regressao inteira caiu ate eu achar isso.

### 4.4 Teclas estendidas e edicao de linha - 20:10 as 01:40 - commit ff02fda - FEITO
Teclado: mapa de teclas estendidas (prefixo 0xE0) traduzido para sequencias ANSI
(setas, Home, End, Delete, Insert, PageUp, PageDown), alem de Ctrl e Alt. O mesmo
byte chega pelo teclado fisico, pela serial e pela PTY.
Termios de verdade: `TCGETS`/`TCSETS` no TTY e na PTY, `ICANON` e `ECHO`
separados, `cfmakeraw` na libc, `TIOCGWINSZ` (o `wm` informa o tamanho da janela
a PTY).
`sh`: `edit.c` novo com edicao de linha em modo raw - setas esquerda/direita,
Home, End, Delete, backspace no meio, Ctrl+L, Ctrl+D - e historico de 50 linhas
nas setas cima/baixo, com o builtin `history`. Redesenho sempre por
`\r ESC[K linha inteira`, que e O(largura) e nao erra o cursor.
Terminal grafico do `wm` reescrito: parser CSI completo (CUU/CUD/CUF/CUB, CHA,
CUP, ED, EL, SGR, `ESC[?25h/l`) e 16 cores com video reverso; cada celula guarda
caractere, frente e fundo.
Programa novo `edit`: editor de tela cheia (abre, navega, edita, salva com
Ctrl+S, sai com Ctrl+X), usa `TIOCGWINSZ` e barra de status em video reverso.
Funciona igual no TTY e na janela do `wm`.
Um problema achado: o `edit` saia com Ctrl+Q, mas o `wm` reserva Ctrl+Q para
fechar a janela focada - dentro do terminal grafico a tecla fechava a janela e o
editor nunca via nada. Trocado para Ctrl+X.
A regressao ganhou os passos `keys:<hex>` (varios bytes de uma vez, porque uma
seta sao tres bytes) e 9 verificacoes novas: recuperar comando do historico e
anexar texto, inserir no meio com a seta esquerda, Home/End/backspace, Home no
inicio, seta direita + Delete, listagem do `history`, e o ciclo completo do
`edit` (grava, sai, `cat` mostra o conteudo).
Segundo problema, achado pela regressao: a verificacao de memoria da sequencia C
acusou 69 paginas a mais. Nao era vazamento - o `edit` ainda estava vivo quando o
`mem` final rodou, segurando os 128 KiB de buffer. A causa e que o `edit`
redesenhava as 24 linhas a cada tecla (~2 KiB), e a serial de 115200 baud nao
dava conta: o editor ficava segundos atras das teclas injetadas. Passou a
redesenhar a tela toda so quando as linhas se deslocam; nas demais teclas
reescreve so a linha do cursor e o status. Depois disso a sequencia C fecha com
delta de 1 pagina.

### 4.5 Fork copy-on-write - 01:40 as 06:20 - commit 8e8b469 - FEITO
Contagem de referencias por pagina fisica no PMM (vetor de `uint16_t`, ~230 KiB
para 458 MiB de RAM; 0 = dono unico, entao quem nunca compartilhou nao paga
nada). `pmm_free_page` decrementa em vez de liberar quando a pagina e
compartilhada, o que fez destruir espaco de enderecos, `munmap` e `exec`
funcionarem com COW sem mudar uma linha.
`PTE_COW` no bit 10. A `fork` compartilha as paginas e tira o `PTE_WRITABLE` dos
dois lados; pagina so de leitura e compartilhada sem marcar COW, para que uma
escrita nela continue virando SIGSEGV. No fim recarrega o CR3 em vez de fazer
`invlpg` centenas de vezes.
A falta e resolvida em `handle_exception`, antes do caminho do SIGSEGV: uma
referencia so devolve o `PTE_WRITABLE` sem copiar; mais de uma copia a pagina.
O PMM ganhou spinlock: antes so desabilitava interrupcoes, o que nunca foi
correto com 4 CPUs, e o COW multiplica as alocacoes concorrentes.
Medido com 8 filhos vivos que so leem: 9 processos mapeando 87 paginas cada
(783 mapeamentos) sobre 89 paginas fisicas, e a memoria usada subiu de 254 para
469 paginas - quase tudo tabela de paginas e pilha de kernel dos filhos. Com a
copia adiantada seriam ~900.
Um problema achado, e serio: com `CR0.WP` ligado (esta), uma syscall que escreve
no buffer do usuario (`read`, `getcwd`) tambem leva falta de escrita na pagina
COW, so que em ring 0. A primeira versao do handler exigia `err & 7 == 7` (bit
de usuario) e o kernel entrava em panico - reproduzido de proposito antes de
corrigir: `*** EXCECAO NAO TRATADA: #PF Page Fault ... violacao de protecao,
escrita, ring 0`. Passou a aceitar `err & 3 == 3`; o `PTE_COW` so existe em
pagina de usuario, entao nao afrouxa nada do lado do kernel. O `forktest` tem
uma verificacao dedicada a isso (o filho chama `getcwd` num buffer que ele nunca
tocou em modo usuario).
`forktest` com 16 verificacoes: isolamento de bss, pilha e heap, fork dentro de
fork, fork+execv, 40 forks seguidos, 8 filhos so de leitura ao mesmo tempo. O
`mem` do shell do kernel passou a mostrar paginas compartilhadas, faltas e
copias, que e o que permite a regressao provar que o fork ficou barato e que o
compartilhamento volta a zero no fim.

### 4.6 Rede - inicio 06:20
