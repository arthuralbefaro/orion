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
| 4.6 | Rede (v1.6) | [x] | 06:20 | 12:05 | `32b9937` |
| 4.7 | GUI: boot direto e apps (v1.7) | [x] | 12:05 | 17:40 | `9fcd874` |
| 4.8 | Extras | [ ] | 17:40 | - | - |

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

### 4.6 Rede - 06:20 as 12:05 - commit 32b9937 - FEITO
Driver e1000 (82540EM do QEMU): aneis de 32 descritores de RX e TX, MAC lido de
RAL/RAH depois do reset, interrupcao registrada mais um laco de recepcao que
espera 20 ms como rede de seguranca.
Pilha em `kernel/src/net`: uma interface, cache ARP de 16 entradas (a resolucao
dorme esperando a resposta, em vez de devolver "tente de novo"), IPv4 com
checksum, resposta a ICMP echo dentro do kernel, e demux de UDP por porta.
Sockets `SOCK_DGRAM` e `SOCK_RAW` com fila de 8 pacotes e `poll`; syscalls
socket(41), connect(42), sendto(44), recvfrom(45), bind(49). A configuracao da
interface vai e vem por ioctl no proprio socket, como no Unix.
Cliente DHCP no kernel (precisa mandar de 0.0.0.0 para 255.255.255.255 antes de
a interface ter endereco). Programas `ifconfig`, `ping`, `nslookup` e `dhcp`;
comandos `net`, `arp`, `dhcp` e `ifconfig` no shell do kernel.
Funcionando de ponta a ponta: DHCP pega 10.0.2.15/24 com gateway 10.0.2.2 e DNS
10.0.2.3; `ping 10.0.2.2` faz 3 de 3 com 0,7 ms; `nslookup example.com` devolve
172.66.147.243, ou seja, sai de verdade para a internet do hospedeiro.
Tres problemas achados, os dois primeiros graves:
(1) Ao habilitar a IRQ 11 da placa, o boot passou a travar de forma
intermitente. A IRQ 11 e compartilhada com o AHCI, que e um driver de polling e
nunca limpava `PxIS`: a linha (nivel, ativa alta) ficava presa em alto e a CPU
entrava em tempestade de interrupcoes. O AHCI passou a desligar as proprias
interrupcoes (`GHC.IE`, `PxIE`) e a limpar `PxIS`/`IS` depois de cada comando.
(2) `wait_sleep_timeout` pegava a `runq_lock` com `spin_lock` puro. Todos os
chamadores existentes ja vinham com interrupcao desabilitada (seguravam uma
trava tomada com `spin_lock_irqsave`), entao nunca deu problema; a rede foi a
primeira a chamar sem trava, com interrupcao ligada, e um tick do timer dentro
dessa janela travava a CPU contra ela mesma. Agora ela salva e restaura o
estado das interrupcoes, como o `poll_table_sleep` ja fazia.
(3) O console do kernel nao entendia sequencia ANSI nenhuma: o `edit` e o
historico do `sh` despejavam os escapes como texto, e cada tecla fazia a tela
rolar varias vezes - rolar 1280x800 e um memmove de 4 MiB, entao o editor
ficava segundos atras do teclado (era isso que fazia a sequencia C acusar 69
paginas a mais, com o `edit` ainda vivo no `mem` final). Ganhou o mesmo parser
CSI do terminal grafico; conferido por captura de tela.
Fora do escopo: TCP e `wget` (o passo pedia como opcional e a maquina de estados
com retransmissao nao cabia no tempo). A verificacao do `nslookup` na regressao
depende de o hospedeiro ter DNS funcionando.

### 4.7 GUI: boot direto e aplicativos - 12:05 as 17:40 - commit 9fcd874 - FEITO
`init` le `/etc/autostart`: uma linha por comando, `#` e comentario, e
`shell <caminho>` troca o programa que ele mantem vivo. O padrao continua
`shell /bin/sh`; `.\build.ps1 -Gui` grava um autostart com `/bin/dhcp` e
`shell /bin/wm`, e o sistema sobe direto na interface grafica, ja com IP.
Dois syscalls proprios em vez de um `/proc`: `sysinfo` (1002, memoria, uptime,
heap e numero de CPUs) e `proclist` (1003, ate 64 processos).
`wm` ganhou janelas de painel (um vetor de linhas de texto que o compositor
preenche): `Processos` (pid, ppid, paginas, nome, atualizada a cada segundo) e
`Arquivos` (readdir + stat, clicar num diretorio entra, `..` sobe). Menu no
botao "Orion 1.0" com terminal, processos, arquivos, sobre e encerrar, com
realce ao passar o mouse. A barra de topo mostra IP, memoria usada, data e hora,
e corta a dica da esquerda para nao colidir com o lado direito.
Atalhos novos: Ctrl+P (processos) e Ctrl+F (arquivos).
Tres problemas achados:
(1) `build.ps1` so considerava os cabecalhos de `user/lib` ao decidir se
recompila um programa. Mudar `wm/win.h` nao recompilava `wm/win.c`, e o binario
saia com o valor antigo compilado dentro - passei um tempo achando que a
mudanca de largura do menu nao tinha efeito. Agora os cabecalhos de dentro do
diretorio do programa contam.
(2) A sequencia D constroi com `-Gui`, o que deixa `out\esp\etc\autostart`
apontando para o `wm`. Como as sequencias A, B e C rodam com `-NoBuild`, a
proxima execucao de C bootava na interface grafica e todos os comandos iam para
o `sh` de dentro do compositor, sem sair pela serial: 14 verificacoes vermelhas
sem nenhuma falha de verdade. A sequencia D agora restaura o autostart padrao no
fim.
(3) Uma vez, e so uma vez em muitas execucoes, a sequencia C caiu com
`#PF Page Fault ... ring 0, busca de instrucao` em RIP=0 durante o `mkfs`, na
CPU 2, com o quadro de excecao inconsistente (RSP=0x400, CS=0x270) e RBP/R13
apontando para a regiao HHDM. Nao reproduziu em nenhuma das execucoes seguintes
da mesma sequencia. **Continua em aberto.** Como nao dava para reproduzir, o que
entrou foi diagnostico: o dump de excecao nao tratada agora imprime tid, nome,
CPU, os limites da pilha da thread e se o RSP esta dentro dela, alem do processo
dono. Na proxima vez da para dizer de imediato se e estouro de pilha de kernel.
(4) Depois de (3), a sequencia A travou uma vez logo depois de o `forktest -s`
terminar: nenhum panico, so o sistema parou de responder na serial. Rodando a
sequencia A sozinha duas vezes em seguida, as duas passaram (41 de 41). Junto
com (3), sao duas travadas raras e nao reproduziveis, as duas em momentos de
criacao/saida de processo com 4 CPUs. **E o problema numero um em aberto no
fim do turno.**

### 4.8 Extras - inicio 17:40
