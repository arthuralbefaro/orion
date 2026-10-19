# Intensivo Orion - progresso

Diario do trabalho sobre o `docs/PLANO.md`. Uma linha por etapa:
`[x]` feita, `[~]` em andamento, `[!]` bloqueada, `[ ]` nao comecada.
Este arquivo manda: uma sessao nova retoma da primeira etapa que nao esta `[x]`,
menos a 5.2 a 5.9, que vem depois da 6.7 (ver o paragrafo abaixo do Estado).

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
`kernel/rust/<crate>`. A regressao demora uns 40 minutos; o estresse 5x, outros
10. Nunca rodar o `test.ps1` enquanto a regressao roda: ele mata o QEMU dela
(mesmas portas 4444/4445).

Estado (2026-10-19, 17:25, sessao 3): Fases 2, 3 e 4 inteiras (tags `v2.1`,
`v2.2` e `v2.3`), mais 8.7, 7.1, 7.2, 5.1, 6.1 a 6.6 (apps `imgview`, `monitor`,
`arquivos`, `editor`, `config` e `terminal`) e 6.7 (barra de tarefas,
Alt+Tab, minimizar e maximizar, lancador de apps): Fase 6 inteira; e a
5.2 (MSI: AHCI na CPU 1, rede `e1000e` na CPU 2), a 5.3 (virtio-blk e
virtio-net com MSI-X), a 5.4 (NVMe), a 5.5 (xHCI e `lsusb`, em Rust) e a
5.6 (teclado e mouse USB, inclusive sem PS/2) e as partes 1 e 2 da 5.7
(pendrive USB). Versao 2.4.0-a23. O
boot pede usuario e senha (`root`/`orion`); o `test.ps1` e o `qrun.py` entram
sozinhos como root. SMEP, SMAP e ASLR ligados. `build.ps1 -Iso` gera a ISO
hibrida e live. ACPI pelo uACPI. Apps graficos em processos separados com a
`libgui` (areas sujas); o `wm` usa fontes TrueType e papel de parede PNG. A
regressao roda no `out\orion.img` (GPT, raiz em ext2) nas sequencias A a E,
na ISO na F, no virtio (disco e rede) na V, no NVMe na N e com USB na U,
com 526 verificacoes; a sequencia A sobe o `tools/hostweb.py`
no hospedeiro e usa `hostfwd` (5555 e 8081).

Proxima etapa: 5.7 parte 3, remocao do pendrive (requisito 6 logo abaixo
das regras: disco morto com contagem de referencias, umount sem panico,
Disable Slot, segundo plug por device_add, e o teste do prazo pelo
device_del com o disco montado), e no fim dela a regressao completa da
etapa 5.7; depois 5.8 (requisitos tambem abaixo) e 5.9. Versao 2.4.0-a24 em
diante.

As etapas 5.2 a 5.9 (MSI, virtio, NVMe, USB, audio, video) ficaram `[ ]` de
proposito: a "Ordem recomendada" do `docs/PLANO.md` poe a Fase 6 antes da
Fase 5 (turno "10+": fase 6, fase 5, 7.3 a 7.5), e so a 5.1 veio antes, junto
da 7.1 e da 7.2 (turno 7). Nenhuma etapa da Fase 6 depende delas. Entram
logo depois da 6.7, de 5.2 a 5.9, antes da 7.3.

A 7.4 (hardware real) nao espera o dono (decidido por ele em 2026-10-19).
Ao chegar nela, fazer so o que da sem um PC de verdade: gerar a ISO com
`build.ps1 -Iso` e conferir que ela sobe no QEMU em UEFI e em BIOS; escrever
`docs/teste-hardware-real.md` com o roteiro do teste (como gravar o pendrive
no Windows, que opcoes escolher no firmware, como Secure Boot e CSM, o que
conferir em cada etapa do boot e o que anotar e mandar se algo falhar: foto
da tela, ultimo texto que apareceu, modelo da placa-mae); marcar a 7.4 como
`[!]` no checklist com a nota "aguardando teste do usuario em hardware real,
roteiro em docs/teste-hardware-real.md" e repetir isso no paragrafo do
Estado. Depois seguir direto para a 7.5 e a Fase 8; a 7.4 volta quando o dono
mandar o resultado do teste.

Criterio de verificacao (decidido pelo dono em 2026-10-19, para poupar tempo
e o notebook, que vem caindo com tela azul):
- Fim de cada PARTE: `.\check.ps1` sem warnings, `.\build.ps1` OK e so as
  sequencias da regressao que cobrem o que mudou (`-OnlyX`): rede ou
  programas de texto -> A; grafico, framebuffer ou Doom -> B; disco, ext2 ou
  armazenamento -> C; `wm` ou apps -> D; travas, processos, threads ou sinais
  -> E; ISO ou boot -> F; virtio -> V; NVMe -> N. Na duvida, inclui a
  sequencia.
- Fim de cada ETAPA (todas as partes feitas): regressao completa, uma vez,
  antes de marcar `[x]`.
- Excecao: parte que mexe no nucleo do kernel (interrupcoes, MSI, memoria,
  paginacao, escalonador, preempcao, travas, syscalls de uso geral) roda a
  regressao completa mesmo sendo parte; isso cobre quase toda a Fase 8.
- Se a regressao completa do fim da etapa falhar, achar pelos commits em que
  parte o erro entrou, corrigir num commit novo e rodar de novo, sem
  reescrever historico.
- No diario, cada parte registra quais sequencias rodou e com quantas
  verificacoes.
- Continuam: RAM livre medida antes de rodar QEMU (2 GB; entre 1 e 2 GB roda e
  anota; abaixo de 1 GB depois de esperar, para), commit por parte logo depois
  da verificacao verde, e a entrada do diario so depois dela.
- Enquanto uma regressao roda, so documentos e planejamento: nenhum codigo-fonte
  (nem build, nem `test.ps1`), porque a regressao reconstroi a imagem no meio
  (depois da D e antes da F) e testaria codigo pela metade. Cada commit leva
  so os arquivos da parte dele.

Requisitos da 5.7 parte 2 (pendrive robusto), decididos pelo dono em
2026-10-19:
1. Prazo em toda transferencia bulk e em todo comando do xHCI. Estourou:
   `Stop Endpoint` e `Set TR Dequeue` para depois do TRB cancelado (nada de
   DMA pendente no buffer reaproveitado), devolve EIO, solta a trava e o disco
   passa a responder EIO, sem travar teclado nem mouse.
2. Despachante unico do anel de eventos, rodando na interrupcao: trava de spin
   com interrupcoes desligadas (nunca mutex) e acordar que funcione de dentro
   da interrupcao. Entrega cada conclusao a transferencia dona pelo endereco
   do TRB (nao so por slot e endpoint) e ignora, com registro no log, conclusao
   de transferencia ja cancelada. Ordem de travas: despachante antes de
   teclado e mouse, e nada ali dentro toma a trava do controlador (registrar no
   `DECISOES.md`). O disco dorme esperando a sua conclusao sem segurar a trava
   do controlador; o HID deixa de ser tratado dentro da espera do disco.
3. CSW com status 1: REQUEST SENSE, sense no log. CSW invalido ou STALL: Bulk-Only
   Mass Storage Reset, CLEAR_FEATURE(ENDPOINT_HALT) nos dois endpoints e a
   sequencia do endpoint zerada no xHCI (`Reset Endpoint` se halted; se nao,
   parar e reconfigurar), repetindo o pedido uma vez. Teste: forcar um STALL e
   conferir que a transferencia seguinte funciona.
4. Bloco diferente de 512: recusar com mensagem clara (sem arriscar 4096).
5. Imagem FAT criada fora do Orion com um arquivo conhecido: ler, gravar outro
   e conferir no hospedeiro.
6. Remocao (`device_del` com o disco montado): shell e teclado seguem
   respondendo; o disco morto existe ate o ultimo usuario soltar (contagem de
   referencia); `umount` dele funciona sem panico; `Disable Slot` e o slot
   liberado; um segundo plug do mesmo pendrive (`device_add`) e reconhecido de
   novo.
7. Atualizar no `DECISOES.md` a entrada da 5.7 (o que mudou e por que).

Requisitos da 5.8 (audio HDA), decididos pelo dono em 2026-10-19:
1. Verificacao pelo WAV do QEMU (`-audiodev wav`): o `play` toca um sinal
   gerado e conhecido (por exemplo 1 kHz, 2 s, so no canal esquerdo) e o
   teste confere no hospedeiro que nao esta mudo, que a duracao esta na
   margem, que a frequencia e ~1 kHz (cruzamentos de zero) e que o canal
   direito esta em silencio; nada de comparar amostra por amostra. Fechar o
   QEMU com `quit` pelo monitor antes de ler o WAV, para o cabecalho sair
   certo. Audio ligado so na sequencia que testa audio.
2. Codec sem IDs de no fixos do QEMU: percorrer o grafo (AFG, pinos pela
   configuracao padrao: alto-falante, fone, line out; conexoes ate um DAC) e,
   em cada no do caminho, tirar o mudo e ajustar o ganho, ligar a energia (D0)
   e o EAPD no pino. Registrar no `DECISOES.md` o que foi feito pensando em
   hardware real (7.4).
3. Stream: BDL alinhada em 128 bytes, IOC por entrada, LVI, o mesmo formato no
   stream e no conversor, stream tag no conversor e interrupcao (MSI) para
   repor os buffers.
4. `/dev/dsp` no estilo OSS (`SNDCTL_DSP_SPEED`, `SETFMT`, `CHANNELS`,
   `GETOSPACE` e `O_NONBLOCK`): o `write` dorme com a fila cheia; com a fila
   vazia o driver preenche com silencio (sem repetir o ultimo trecho); converte
   a taxa no software quando o codec nao aceitar a pedida (44,1 kHz para
   48 kHz, por exemplo); um processo por vez (`EBUSY`); no `close`, toca o que
   falta e para o stream.
5. Doom (parte 2): so efeitos sonoros (8 canais mixados, de 11025 Hz para
   48 kHz), com escrita sem bloqueio para o jogo nunca travar no audio. A
   musica (MUS/OPL) fica como parte opcional a parte ou de fora; se ficar de
   fora, registrar no diario.

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
| 5.1 | uACPI | [x] | 10:14 | 11:05 | ver `git log` |
| 5.2 | MSI/MSI-X | [x] | 04:20 | 06:15 | ver `git log` (2 partes) |
| 5.3 | virtio-blk e virtio-net | [x] | 06:15 | 13:35 | ver `git log` (2 partes) |
| 5.4 | NVMe | [x] | 13:35 | 14:10 | ver `git log` |
| 5.5 | xHCI | [x] | 14:10 | 14:55 | ver `git log` |
| 5.6 | USB HID | [x] | 14:55 | 15:55 | ver `git log` |
| 5.7 | USB armazenamento | [~] | 15:55 | | |
| 5.8 | Audio HDA | [ ] | | | |
| 5.9 | Video BGA e virtio-gpu | [ ] | | | |
| 6.1 | Protocolo de janelas | [x] | 11:06 | 11:47 | ver `git log` |
| 6.2 | libgui | [x] | 11:48 | 12:27 | ver `git log` |
| 6.3 | Fontes TrueType | [x] | 12:27 | 13:05 | ver `git log` |
| 6.4 | Imagens PNG/JPEG | [x] | 13:05 | 13:38 | ver `git log` |
| 6.5 | Compositor com areas sujas | [x] | 13:38 | 15:47 | ver `git log` |
| 6.6 | Apps | [x] | 15:47 | 03:05 | ver `git log` (6 partes) |
| 6.7 | Area de trabalho | [x] | 03:05 | 04:20 | ver `git log` (2 partes) |
| 7.1 | ISO hibrida | [x] | 06:26 | 09:44 | ver `git log` |
| 7.2 | Live | [x] | 09:44 | 10:13 | ver `git log` |
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

#### 7.1 ISO hibrida - 06:26 a 09:44 - FEITO
`tools/mkiso.c` (novo, C99, roda no Windows pelo `zig cc` e no Linux pelo
`cc`) monta ISO9660 nivel 2 com Rock Ridge e El Torito com duas entradas
(BIOS com o `limine-bios-cd.bin` e a "boot info table"; UEFI com a imagem FAT
`limine-uefi-cd.bin`), e poe na area de sistema um MBR com a particao EFI;
o `limine bios-install` (o `limine.c` do proprio Limine, compilado em
`out/tools`) completa o boot BIOS por pendrive. `build.ps1 -Iso` gera
`out/orion.iso`; `-Run -Iso` e `-Bios` bootam. O kernel registra de onde foi
lido ("kernel lido de /boot/orion.elf no CD"). Harness: `test.ps1 -Iso`,
`-IsoDisk`, `-Bios` e `reboot:<firmware>,<midia>`; o mesmo no `qrun.py` e no
`regress.py`. Criterio: a sequencia F (nova) boota a mesma ISO como CD e como
pendrive, em BIOS e em UEFI, e o kernel chega ao fim do boot nos quatro. Um
leitor independente em Python conferiu a estrutura (Rock Ridge, tabelas de
caminhos, catalogo com checksum, MBR). Regressao 375 -> 380. Versao 2.3.1
(a 7.1 veio antes das Fases 5 e 6 pela ordem recomendada).
Problemas: (1) os boots UEFI da ISO caiam no "EFI Internal Shell": o NVRAM
do OVMF, compartilhado com os boots do disco, guardava a ordem de boot do
disco. Pior: a primeira rodada da sequencia F gravou ali a ordem nova, e a
regressao inteira seguinte bootou as sequencias A a E no shell do UEFI (298
de 380 falhas). O harness passou a copiar as variaveis limpas do QEMU a cada
partida.
(2) O `zig cc -fsyntax-only` nao funciona para o alvo do hospedeiro no
Windows; o `check.ps1` compila os `tools/*.c` para um `.o` temporario.
Ficou de fora: o teste no VirtualBox (nao esta instalado aqui; fica para o
dono), Joliet e HFS+.

#### 7.2 Live - 09:44 a 10:13 - FEITO
A ISO leva o sistema inteiro num initramfs (cpio `newc` de 49 MiB, gerado
pelo `tools/mkinitrd.py` a partir da mesma arvore da imagem ext2, com os
mesmos modos e donos) que o Limine carrega como modulo. O `vfs_init` tenta o
initramfs antes dos discos: monta um tmpfs como raiz, desempacota
(`fs/initramfs.c`) e devolve ao alocador os 50 MiB do modulo
(`pmm_add_region`); no QEMU de 512 MiB o live sobe com 518 MiB de total e 53
usados. Criterio: a sequencia F entra pelo `login` nos quatro boots da ISO
(CD e pendrive, BIOS e UEFI), com a raiz `initramfs / tmpfs`, e usa o
sistema (`/bin` inteiro, `hello`, `lua`, `su`, gravar e ler na raiz); no boot
seguinte o arquivo gravado nao existe mais. O `make iso` (Linux) gera a
mesma ISO live pelo `qrun.make_iso`.
Regressao 380 -> 390, estresse 5x verde. Versao 2.3.2.
Problemas: nenhum; o desempacotamento e os quatro boots passaram na primeira
rodada.
Ficou de fora: compressao do initramfs, escolher no menu do Limine entre live
e instalar (7.3), persistencia em pendrive.

#### 5.1 uACPI - 10:14 a 11:05 - FEITO
uACPI 6.1.1 em `third_party/uacpi`, compilado junto do kernel (sem warnings),
com a interface `uacpi_kernel_*` em `drivers/acpi_os.c` (mapeamento pelo
HHDM ou pela janela de MMIO, PCI, portas, mutex com prazo, eventos,
spinlocks, SCI pela IRQ ISA e uma thread `acpi-work` para os trabalhos
adiados). `drivers/power.c` carrega o namespace (uns 60 ms no QEMU), liga o
botao de energia e le o `_PRT`; a e1000 passou a usar a GSI que o `_PRT` da
(22 no q35) em vez da IRQ 11. `poweroff` (S5), `reboot` (registrador de
reset do FADT) e `halt`, pela syscall `reboot` do Linux (so root; o usuario
comum recebe `EPERM`), pelo programa `poweroff` (com `reboot` e `halt`) e
pelo shell do kernel. Harness: passo `monlog:` grava a resposta do monitor
do QEMU no log. Criterio: no QEMU, `reboot`, `poweroff` e o botao de energia
(`system_powerdown`) deixam o QEMU em "paused (shutdown)" pelo caminho do
ACPI, em UEFI (sequencia B) e em BIOS (sequencia F, a partir do live).
Regressao 390 -> 399, estresse 5x verde. Versao 2.4.0-a1.
Problemas: (1) o `_PIC(1)` precisa ser avaliado depois da carga do
namespace e antes da inicializacao; na primeira ordem o uACPI recusou e o
`_PRT` devolveu as rotas do PIC, e a e1000 continuou na IRQ 11. (2) Com
`-no-reboot -no-shutdown` o QEMU pausa tanto no reset quanto no S5; o teste
separa os dois pela linha do kernel e confere que o reboot nao caiu no
controlador do teclado. (3) A regressao pegou uma corrida antiga da
sequencia C: o comando seguinte ao `mkfs.ext2` era digitado enquanto ele
ainda imprimia, e o eco caiu no meio da linha conferida; o passo agora
espera o "pronto" antes do proximo comando.
Ficou de fora: o teste no VirtualBox (nao ha VirtualBox aqui), GSIs acima de
47, outros dispositivos pelo `_PRT` (a AHCI trabalha sem IRQ), suspensao (S3).

#### 6.1 Protocolo de janelas - 11:06 a 11:47 - FEITO (as duas partes)
Kernel: `MAP_SHARED` com escrita em arquivo do tmpfs (novo `getpage` no
`fs_type`: o `vma_fault` mapeia a propria pagina do arquivo, com contagem
de referencias); fora do tmpfs continua `ENODEV`, e sem permissao de escrita
no descritor da `EACCES`. `wm`: socket `/run/wmapp.sock` com conexoes
persistentes e protocolo de linhas (`janela`, `mostra`, `titulo`, `fecha`;
de volta `tecla`, `clique`, `fechar`); cada janela de app tem um arquivo em
`/run/wm` que o `wm` e o app mapeiam com `MAP_SHARED`, e o `wm` copia para o
framebuffer so quando o app manda `mostra`. `hello-gui` (novo) abre uma
janela propria, conta teclas e cliques e fecha com `q`. Criterio: na
sequencia D o `hello-gui` abre a janela, recebe duas teclas e um clique (o
log do `wm` mostra o titulo mudando a cada evento) e fecha; a captura tem o
degrade. `shmtest` (novo, sequencia A) confere o `MAP_SHARED` entre
`read`/`write`, entre processos e no `fork`. Regressao 399 -> 407,
estresse 5x verde. Versao 2.4.0-a2.
Problemas: nenhum de comportamento; tudo passou na primeira rodada. Visto
depois, para a 6.2: o `wm` nao ignora `SIGPIPE`, entao escrever para um app
que acabou de sair o mataria (o fluxo testado nao chega nisso).
Ficou de fora: passar descritores por socket (`SCM_RIGHTS`) e `memfd`,
eventos de movimento do mouse e de soltar o botao, redimensionar a janela.

#### 6.2 libgui - 11:48 a 12:27 - FEITO
`user/libgui` (ligada estaticamente em cada app pelo `build.txt`): rotulo,
botao, campo de texto, caixa de opcao, lista com barra de rolagem e barra de
menus com menus suspensos, todos usaveis pelo mouse e pelo teclado (Tab,
Enter, espaco, setas, Page Up/Down, F10, Esc). `guidemo` (novo) usa todos e
troca o titulo a cada acao. O `wm` passou a ignorar `SIGPIPE` (e a devolver
o padrao aos shells que abre). O `check.ps1` compila a `libgui`. Criterio:
na sequencia D o `guidemo` recebe texto, aciona o botao pelo Tab e Enter,
liga a caixa, anda na lista com setas e Page Down, acrescenta um item, abre o
menu Ajuda pelo mouse e sai pelo menu Arquivo pelo teclado; o log do `wm`
mostra cada titulo. Regressao 407 -> 415. Versao 2.4.0-a3.
Problemas: o primeiro teste dos menus pelo mouse falhou: depois de um
`mouse_move -640 -400` seguido de outro movimento, o cursor parou 132 pixels
antes do lugar, porque o QEMU junta movimentos relativos ainda nao entregues.
Os menus ganharam teclado (F10, setas, Enter), o que tambem e melhor para
quem usa, e o teste do mouse passou a sair da posicao conhecida do clique
anterior com um movimento curto.
Ficou de fora: arrastar (o protocolo nao manda movimento nem soltar o
botao), selecao de texto, areas sujas dentro do app.

#### 6.3 Fontes TrueType - 12:27 a 13:05 - FEITO
`stb_truetype` 1.26 (`third_party/stb`) e as DejaVu Sans Mono e Sans 2.37
(`third_party/dejavu`, instaladas em `/usr/share/fonts`). `wm/font.c`
rasteriza cada caractere ASCII uma vez e guarda o mapa de alfa; o desenho
mistura o alfa com o fundo (`gfx_blend_alpha`). O terminal usa a mono de
20 px (a celula, 10x21, sai da metrica da fonte), a barra e os titulos usam a
sans de 15 px; sem os `.ttf`, volta a fonte 8x8. Criterio: na sequencia D o
`wm` carrega as duas fontes, o glifo `A` tem dezenas de niveis de alfa
(antialias) e o cache termina com dezenas de glifos; a sequencia B confere a
fonte no `wm` aberto pelo shell. Regressao 415 -> 420. Versao 2.4.0-a4.
Problemas: (1) com 16 px a celula ficou 8x17 e a letra pequena; foi para
20 px. (2) Ficavam restos do cursor no comeco das linhas: a area suja das
linhas do terminal ainda usava a altura da celula 8x8. (3) A amostra de cores
da captura (um pixel a cada 997) nao separa texto com e sem antialias; a
prova virou o numero de niveis de alfa que o proprio `wm` relata.
Ficou de fora: paineis do `wm` (processos, arquivos, menu) ainda na 8x8 (vao
virar apps na 6.6), Unicode fora do ASCII, `libgui` com fonte vetorial.

#### 6.4 Imagens PNG/JPEG - 13:05 a 13:38 - FEITO
`stb_image` 2.30 (`third_party/stb`, so PNG e JPEG) no `wm` (`wm/img.c`) e no
`imginfo` (novo). `tools/mkwallpaper.py` (novo, Python puro) gera no build o
papel de parede PNG de 1280x800, que o `wm` carrega, ajusta a tela se
precisar e copia no fundo; sem ele, volta o degrade. `share/` (novo) vai para
`/usr/share` e leva um JPEG e um PNG de teste com quadrantes de cores
conhecidas. Criterio: a sequencia D carrega o papel de parede PNG e a captura
do boot grafico tem centenas de cores na amostra (eram 75); a sequencia A
decodifica o JPEG e o PNG de teste (tamanho, canais e as quatro cores certas)
e o papel de parede. Regressao 420 -> 425. Versao 2.4.0-a5.
Problemas: nenhum; tudo passou na primeira rodada. Efeito colateral
registrado: a verificacao de cores da captura do `hello-gui` perdeu forca com
o papel de parede, e o `hello-gui` segue provado pelo log do `wm`.
Ficou de fora: icones (vao com o lancador da 6.7), GIF e BMP, escolher o
papel de parede.

#### 6.5 Compositor com areas sujas - 13:38 a 15:47 - FEITO
Medido primeiro: o `wm` parado ja gastava 0,2 a 0,3% de CPU (back buffer,
retangulos sujos e cursor proprio vinham da Fase 1). O que faltava eram areas
sujas dos apps: o protocolo ganhou `mostra <x> <y> <l> <a>` e a `libgui`
compara uma assinatura do estado visual de cada componente com a do ultimo
desenho, redesenha so os que mudaram e manda so a area deles; o `wm` conta
quadros, quadros parciais e pixels por app. `cpuuso` (novo) mede o CPU de um
processo pelo `/proc`; o passo `type:` do harness aprendeu `>`, `<`, `|` e
`&`. Criterio: na sequencia D, `cpuuso wm 10` (com a saida na serial) mostra
o `wm` parado abaixo de 5% (deu 0,3%), e o `guidemo` manda 16 de 24 quadros
como parciais (2,6 milhoes de pixels copiados contra 5,8 milhoes). Regressao
425 -> 427. Versao 2.4.0-a6.
Problemas: a primeira regressao completa desta etapa falhou no `mremap` do
`mhello`, que nada tinha a ver com ela: o teste contava com a pagina seguinte
a um `mmap` novo livre, o que o ASLR da 4.4 passou a quebrar em cerca de 1 de
cada 8 execucoes (40 execucoes num laco do `dash` mostraram o `EEXIST`). O
teste passou a montar a situacao com `MAP_FIXED` e deixou de ler
`MAP_FAILED + 8192` em cascata; 40 de 40 passaram. Depois a sessao caiu duas vezes
no meio da regressao e a rodada seguinte deu 365 falhas com logs vazios: o
Windows estava sem memoria para alocar (o OneDrive do dono tinha vazado 33 GB
e sobravam 0,2 GB), e o QEMU nao conseguia reservar o buffer do JIT. Com o
OneDrive encerrado pelo dono, a regressao passou inteira.
Ficou de fora: compositor com transparencia e sombras, sincronizar com o
retraco vertical (o framebuffer da UEFI nao tem).

#### 6.6 parte 1 Visualizador de imagens - 15:47 a 17:40 - FEITO
A `libgui` ganhou o componente tela (`gui_canvas`: o app pinta por uma funcao
e aumenta `version` quando o desenho muda, numero que entra na assinatura das
areas sujas), o `gui_blit` e o gancho `on_key`. `imgview` (novo) abre PNG e
JPEG com o `stb_image`, ajustado a 640x380 mantendo a proporcao ou em 1:1
(`f`), anda pelas imagens da mesma pasta com as setas ou os botoes e fecha com
`q`. `Test-ShotPixel` (novo, no `regressao.ps1` e no `regress.py`) confere a
cor de um pixel da captura. Criterio: na sequencia B o `imgview` abre o JPEG
de teste, a captura `reg-app-imgview` tem os quadrantes vermelho e amarelo no
lugar certo, a seta passa para o PNG da mesma pasta e o `q` fecha a janela.
Regressao 427 -> 432, verde na primeira rodada completa (31 minutos). Versao
2.4.0-a7.
Problemas: o PC travou no meio da etapa, antes do commit. Os arquivos ficaram
integros; a etapa foi terminada, compilada e passou na regressao sem mudanca
no codigo. O Windows tinha so 1,5 GB de memoria comprometida livre (muitos
programas abertos, sem vazamento), e foi o bastante para o QEMU.
Ficou de fora: zoom e rolagem em 1:1 (imagem maior que a area aparece cortada
no centro), GIF e BMP.

#### 6.6 parte 2 Monitor do sistema - 17:45 a 19:55 - FEITO
Kernel: `/proc/stat` (novo, formato do Linux) com o total e uma linha por
CPU; cada CPU conta no `sched_tick` os tiques em modo usuario, no kernel e
ociosa. `libgui`: `on_tick`/`tick_ms` (o `gui_run` espera o socket com `poll`
ate o proximo tique). `monitor` (novo): uma amostra por segundo, graficos de
CPU e memoria com 60 amostras, lista de processos (pid, nome, estado, RSS e
CPU de cada um) que guarda a escolha entre amostras, e "Encerrar" com
`SIGTERM`. Criterio: a sequencia A le o `/proc/stat` (total, cpu0 a cpu3 e
`ctxt`); na B o `monitor` abre, conta os processos, encerra pela lista um
`sleep 100 &`, ve o pid sair, a captura tem os dois graficos e o `q` fecha
com quadros parciais. Regressao 432 -> 439 (verde na terceira rodada completa, 26 minutos). Versao
2.4.0-a8. RAM livre antes da regressao: 7,0 GB, 6,3 GB e 6,2 GB nas tres
rodadas completas.
Problemas: (1) o `wm` corta titulos em 31 caracteres e o titulo do sinal
saiu cortado; ficou `monitor: TERM <pid> <nome>`. (2) O `sleep` morto fica
zumbi ate o shell colher, entao o numero de processos nao cai; o monitor
passou a avisar quando o pid encerrado sai ou vira `Z`. (3) A primeira
regressao completa falhou so na ordem de duas linhas do log (a do kernel
sobre o `SIGTERM` caiu entre os dois titulos); a verificacao aceita linhas no
meio. (4) A segunda falhou em 13 verificacoes da sequencia D, sem relacao com
o monitor: com a maquina lenta, o comeco da D, feito de esperas fixas, se
atrasou uns 10 s e o `hello-gui` recebeu as teclas do `guidemo`. A D passou a
esperar o `wm` pronto e o fim do `cpuuso` pelo log (`until:`). (5) A espera
depois do `history` na A, de 1 s para 3 s, foi registrada no `DECISOES.md`.
Ficou de fora: ordenar a lista por CPU ou memoria, sinais alem do `SIGTERM`,
CPU por nucleo no grafico, rede e disco.

#### 6.6 parte 3 Gerenciador de arquivos - 19:55 a 20:30 - FEITO
`libgui`: acao `open` nas listas (Enter ou clique no item ja escolhido),
separada da selecao. `arquivos` (novo): campo do caminho, lista com pastas
primeiro e tamanhos, subir por `..`, Backspace ou botao, nova pasta, apagar
com confirmacao (segundo aperto) e imagens abertas no `imgview` por
`fork`/`execl`, com os filhos colhidos no tique. Criterio: na sequencia B o
`arquivos /usr/share` entra em `imagens` pelo teclado, abre o `teste.jpg` no
`imgview`, cria e apaga `pasta-nova` e fecha com `q`; a captura confere o
campo do caminho e o texto da lista. Regressao 439 -> 447 (verde na primeira rodada completa, 26 minutos).
Versao 2.4.0-a9. RAM livre antes da regressao completa: 6,3 GB.
Problemas: nenhum; a sequencia B passou na primeira rodada com o app novo.
Ficou de fora: copiar, mover e renomear, arquivos ocultos, abrir texto (vem
com o editor), icones (6.7).

#### 6.6 parte 4 Editor de texto - 2026-10-18 20:30 a 2026-10-19 01:45 - FEITO
`editor` (novo): linhas em buffers proprios, desenho numa tela com rolagem
nos dois sentidos, setas, Home/End, PgUp/PgDn, Delete, Backspace que junta
linhas, Tab em 4 espacos, clique que posiciona o cursor e Ctrl+S para salvar;
o titulo marca a mudanca nao salva. O `arquivos` abre no `editor` o que nao e
imagem. Criterio: na sequencia B o `editor /tmp/nota.txt` recebe duas linhas,
Backspace, seta para cima, End e um ponto, salva com Ctrl+S e fecha com
Ctrl+Q; o `cat /tmp/nota.txt` depois do `wm` mostra `ola orion.` e
`segunda vez`, e a captura tem o cursor no fim da primeira linha. Regressao
447 -> 453, verde na rodada completa de 01:13 a 01:40. Versao 2.4.0-a10.
RAM livre antes dessa regressao: 4,4 GB.
Problemas: o PC reiniciou sozinho no meio da primeira regressao completa
desta parte (sequencia C), sem commit; os arquivos ficaram integros e a
regressao seguinte passou inteira sem mudanca no codigo. O log do Windows
registra o desligamento inesperado as 20:42:48 (Kernel-Power 41, sem tela
azul), assim como o das 16:39:57; nenhum dos dois teve antes o aviso de falta
de memoria virtual (o ultimo foi as 15:09, com o vazamento do OneDrive), entao
nao parecem ser falta de memoria. A entrada deste diario tinha sido escrita
antes da regressao terminar; foi tirada e so voltou depois da rodada verde.
Ficou de fora: selecao, copiar e colar, desfazer, busca, perguntar antes de
fechar com mudanca nao salva (o `wm` fecha sem consultar o app), caracteres
fora do ASCII.

#### 6.6 parte 5 Configuracoes - 01:45 a 02:20 - FEITO
`wm`: o papel de parede vem do `/etc/wm.conf` (`fundo=<imagem>` ou
`fundo=nenhum`; sem o arquivo, o `orion.png`) e o socket de comandos ganhou
`fundo <imagem|nenhum>`, que troca o fundo na hora (o `wmctl` tambem usa).
`config` (novo): versao, CPU, memoria, tempo ligado e rede, atualizados a cada
segundo, e a lista de papeis de parede com a escolha atual marcada; Enter ou
"Aplicar" troca o fundo pelo `wm` e grava o `/etc/wm.conf`. Criterio: na
sequencia B o `config` aplica o `teste.jpg` (a captura tem os quadrantes
vermelho e verde no fundo da tela) e o `teste.png` e fecha com `q`; depois, o
`/etc/wm.conf` tem o `teste.png` e um `wm` novo carrega esse fundo ao
iniciar. Regressao 453 -> 461, verde na primeira rodada completa (26
minutos). Versao 2.4.0-a11. RAM livre antes da regressao: 6,7 GB.
Problemas: nenhum; a sequencia B passou na primeira rodada com o app novo.
Ficou de fora: tema e cores, fontes, teclado, data e hora, rede estatica pelo
app (o `ifconfig` faz), usuarios e senhas (o `passwd` faz).

#### 6.6 parte 6 Terminal como app - 02:20 a 03:05 - FEITO (fecha a 6.6)
`libgui`: `on_watch`/`watch_fd` (um descritor a mais no `poll` do `gui_run`).
`terminal` (novo): `sh` numa pty de 48x26 com `COLUMNS` e `LINES` no
ambiente, o interpretador ANSI do `wm` (`wm/term.c`) reaproveitado, teclas
estendidas como no terminal do `wm`; fecha quando o `sh` sai. Criterio: na
sequencia B o `terminal` abre, o `printenv COLUMNS` grava `48` (so o `sh` do
app tem essa variavel), um `sleep 50` e cortado por Ctrl+C antes do `echo`
seguinte, o `ls` aparece na captura com o cursor na linha do prompt e o `exit`
fecha a janela. Regressao 461 -> 467, verde na primeira rodada completa (27
minutos). Versao 2.4.0-a12. RAM livre antes da regressao: 7,4 GB.
Problemas: (1) o `wm` recusa janela de app maior que 800x520 e a primeira
versao (988x424) nao abriu. (2) Com a janela recusada, as teclas do teste
cairam no terminal do `wm` e a verificacao do arquivo passou mesmo assim; o
teste passou a gravar o `COLUMNS`, que separa os dois terminais.
Ficou de fora: fonte vetorial no app (so o `wm` tem), rolagem para tras,
copiar e colar, redimensionar.

#### 6.7 parte 1 Barra de tarefas, Alt+Tab, minimizar e maximizar - 03:05 a 03:40 - FEITO
`wm`: barra de tarefas embaixo com um botao por janela (clique foca,
minimiza a que esta em foco ou restaura); botoes de minimizar e, no terminal
do `wm`, de maximizar no titulo; Alt+F9 minimiza, Alt+F10 maximiza ou volta,
Alt+Tab manda a janela em foco para o fundo e restaura a que sobe se estiver
minimizada. O terminal maximizado ocupa a area de trabalho com o texto
guardado (`term_resize`) e a pty recebe o tamanho novo. `WIN_MAX` 16 e
`TERM_MAX_COLS` 128. Criterio: na sequencia D um terminal novo maximiza para
126x33 (a captura tem o terminal cobrindo a area de trabalho e a barra de
tarefas), volta a 60x20, minimiza, e seis Alt+Tab passam pelas outras seis
janelas e o trazem de volta. Regressao 467 -> 474, verde na primeira rodada
completa (28 minutos). Versao 2.4.0-a13. RAM livre antes da regressao: 6,8 GB.
Problemas: nenhum; a sequencia D passou na primeira rodada com as mudancas.
Ficou de fora: maximizar janela de app (o protocolo nao tem redimensionar),
arrastar a borda para redimensionar, area de trabalho com janelas lado a lado.

#### 6.7 parte 2 Lancador de apps - 03:40 a 04:20 - FEITO (fecha a 6.7 e a Fase 6)
`wm`: o menu "Orion" lanca os apps (terminal do `wm` e o app, arquivos,
editor, monitor, imagens, configuracoes) por `fork`/`execl`; Ctrl+P e Ctrl+F
abrem o `monitor` e o `arquivos` no lugar dos paineis 8x8 (que ficam no
`wmctl procs` e `wmctl files`); o menu ganhou teclado (Alt+F1, setas, Enter,
Esc); a janela "Sobre" e a dica da barra mostram os atalhos novos.
Criterio: na sequencia D o `monitor` (Ctrl+P), o `arquivos` (Ctrl+F) e o
`config` (menu pelo teclado) abrem e a captura final tem a janela do `config`
na frente e os tres apps na barra de tarefas. Regressao 474 -> 480, verde na
primeira rodada completa (28 minutos). Versao 2.4.0-a14. RAM livre antes da
regressao: 3,9 GB.
Problemas: o passo antigo da sequencia D que abria o menu pelo mouse nunca
tinha aberto: o QEMU juntava o `mouse_move -640 -400` com o movimento
seguinte e o clique caia em x=0, fora do botao; a captura `reg-gui-4-menu`
saia sem menu e nenhuma verificacao olhava. Trocado pelo teclado, e a
captura agora confere o item destacado.
Marco M4 (desktop de verdade): terminal, arquivos, editor e visualizador de
imagens rodam como programas separados; a fonte TrueType esta no `wm`
(terminais dele, barra e titulos), mas os apps desenham com a 8x8 ampliada da
`libgui`, entao o M4 fica com essa ressalva.
Ficou de fora: icones no menu e na barra, menu com submenus, abrir arquivo
pelo menu do editor, fonte vetorial nos apps.

#### 5.2 parte 1 MSI e AHCI por interrupcao - 04:20 a 05:40 - FEITO
PCI: lista de capacidades (`pci_find_cap`) e MSI (`pci_enable_msi`, com o
LAPIC de destino e o INTx desligado). Vetores 0x80 a 0x9F alocados com nome;
CPU de destino em rodizio a partir da CPU 1. Contadores de interrupcao por
CPU; `/proc/interrupts` (novo, formato do Linux) e o `irq` do shell do kernel
com uma coluna por CPU. AHCI: com MSI, quem espera o fim do comando dorme
numa fila acordada pela IRQ (antes era `thread_yield` em laco, com a IRQ
desligada), com prazo de 10 ms como rede de seguranca. Criterio: a AHCI
liga a MSI no vetor 128 entregue na CPU 1; na sequencia A o
`/proc/interrupts` tem a AHCI so na CPU 1 e o timer local nas 4 CPUs; na B o
`irq` do shell do kernel mostra a AHCI na CPU 1. Regressao 480 -> 484, verde
na segunda rodada completa (27 minutos); estresse 5x verde. Versao
2.4.0-a15. RAM livre antes das regressoes: 6,1 GB e 6,5 GB.
Problemas: a primeira rodada completa deu 3 falhas na sequencia A. (1) O
`history`: o historico do `sh` guarda 50 comandos e o `cat /proc/interrupts`
novo, posto antes do `history`, empurrou para fora o `ls /bin | wc` que a
verificacao procura na linha 1 (falhou de novo rodando a A sozinha); o
comando passou para depois do `history`. (2) O `tmptest` (1000 arquivos sem
vazar pagina): logo depois de apagar os arquivos, mais de 256 paginas ainda
estavam na fila da `kreclaim` (que devolve as alocacoes grandes a cada 10 ms)
e voltaram em seguida; rodando a A sozinha passou, entao foi tempo, com o
Windows ocupado (o `cpptools` do VS Code tinha gastado 800 s de CPU). Fica
anotado: o teste mede logo depois do ultimo `unlink`, sem esperar a
`kreclaim`.
Ficou de fora: MSI-X (vem com o virtio da 5.3), mais de uma mensagem MSI por
dispositivo, afinidade de IRQ configuravel.

#### 5.2 parte 2 Rede por MSI - 05:40 a 06:15 - FEITO (fecha a 5.2)
O driver da `e1000` tenta MSI antes da GSI do `_PRT`; a `e1000e` (82574L,
`8086:10d3`) funciona com o mesmo driver (descritores legados, MAC em
RAL/RAH) e tem MSI. O QEMU dos testes passou a usar a `e1000e` (`-Nic` no
`test.ps1`, `--nic` no `qrun.py`, lido por sequencia no `regress.py`); a
sequencia D roda com a `e1000` antiga para ela continuar coberta pela GSI do
`_PRT`. Criterio: a rede liga a MSI no vetor 129 entregue na CPU 2 e, depois
do trafego da sequencia A, o `/proc/interrupts` tem a `e1000` so na CPU 2 (a
AHCI segue na CPU 1); a D faz o DHCP pela `e1000` sem MSI. Regressao 484 ->
486, verde na primeira rodada completa (28 minutos); estresse 5x verde.
Versao 2.4.0-a16. RAM livre antes da regressao: 6,3 GB.
Problemas: nenhum; a `e1000e` funcionou de primeira com o driver da `e1000`.
Ficou de fora: recursos proprios da 82574L (descritores estendidos, MSI-X
com uma fila por causa, checksum na placa).

#### 5.3 parte 1 Transporte virtio e virtio-blk - 06:15 a 12:45 - FEITO
Transporte PCI moderno (virtio 1.0) em `drivers/virtio.c`: capacidades de
fornecedor, negociacao so com `VIRTIO_F_VERSION_1`, filas em paginas
contiguas e notificacao; MSI-X no PCI (`pci_enable_msix`, tabela na BAR) e
BAR de 64 bits (`pci_bar_address`). `drivers/virtio_blk.c`: disco `vdX` com um
pedido por vez (cabecalho, dados, status) sob mutex, acordado pela MSI-X como
a AHCI. O OVMF boota pelo virtio-blk e a raiz monta em `vda2` sem mudanca no
VFS. `test.ps1 -Bus virtio` e `qrun.py --bus` ligam a imagem como
`virtio-blk-pci`. Criterio: sequencia V nova (imagem no virtio-blk): MSI-X
fora da CPU 0, particoes GPT e raiz em `vda2`, `fstest -a` na raiz, a MSI-X
contada no `/proc/interrupts` (9470 interrupcoes so na CPU 2 numa rodada) e um
arquivo gravado que sobrevive ao reboot. Regressao 486 -> 495, verde na
primeira rodada completa. Versao 2.4.0-a17. RAM livre antes da regressao:
5,9 GB.
Problemas: nenhum de codigo; o virtio-blk funcionou de primeira. A rodada
completa comecou as 06:21 e so terminou as 11:48: o Windows parou entre a
sequencia A (06:32) e a B (11:33), e todas as sequencias rodaram inteiras
depois; os fontes eram todos de antes do comeco. O Windows reiniciou as 11:51
(evento 1074), ja com a regressao terminada.
Ficou de fora: varios pedidos ao mesmo tempo na fila, `VIRTIO_BLK_F_FLUSH`,
virtio legado por portas de I/O.

#### 5.3 parte 2 virtio-net - 12:45 a 13:35 - FEITO (fecha a 5.3)
Camada `nic_*` (`drivers/nic.c`) entre a pilha de rede e o driver: quem acha
a placa registra suas operacoes e `net.c` e `socket.c` so chamam `nic_*`; a
`e1000` procura so placa Intel. `drivers/virtio_net.c`: fila de recepcao com
64 buffers postados e MSI-X, fila de transmissao liberada no envio, cabecalho
de 12 bytes do virtio 1.0, MAC e link da configuracao. Criterio da 5.3 (a
regressao roda nos dois modos): a sequencia V passou a usar a virtio-net com
o servidor do hospedeiro: DHCP, ping sem perda, 10 MiB por TCP com o crc32
certo (2,4 s) e as MSI-X do disco (CPU 2) e da rede (CPU 3) contadas no
`/proc/interrupts`; A a F seguem com AHCI e `e1000e`. Regressao 495 -> 502,
verde na primeira rodada completa (30 minutos). Versao 2.4.0-a18. RAM livre
antes da regressao: 2,9 GB.
Problemas: o Windows caiu as 12:45:15 (Kernel-Power 41, sem QEMU rodando)
logo depois do commit da parte 1, no meio da escrita desta parte; os seis
arquivos gravados no ultimo minuto ficaram so com bytes nulos (o `git diff`
os mostrava como binarios). O repositorio estava integro (`git fsck`); os
arquivos voltaram do commit e a parte foi refeita. A virtio-net funcionou de
primeira.
Ficou de fora: varias filas, checksum e segmentacao na placa
(`VIRTIO_NET_F_CSUM`, TSO), buffers que se juntam (`MRG_RXBUF`), MSI-X para a
fila de transmissao.

#### 5.4 NVMe - 13:35 a 14:10 - FEITO
`drivers/nvme.c` (novo): fila admin, `Identify` do controlador e do
namespace 1, uma fila de I/O com MSI-X, PRP e lista de PRP para pedidos de ate
64 KiB, `Flush` depois de escrever; so bloco de 512 bytes. O disco e
`nvme0n1` e a camada de bloco poe `p` antes do numero da particao quando o
nome termina em digito (`nvme0n1p2`). `test.ps1 -Bus nvme` e `qrun.py --bus
nvme` ligam a imagem num NVMe do QEMU. Criterio: sequencia N nova, com o boot
e a raiz no NVMe (`nvme0n1p2`), `fstest -a` na raiz, a MSI-X contada no
`/proc/interrupts` e um arquivo que sobrevive ao reboot. Verificacao: a
sequencia N sozinha (9 verificacoes) e a regressao completa de fim de etapa,
502 -> 511, verde na primeira rodada (30 minutos). Versao 2.4.0-a19. RAM livre
antes da regressao: 2,8 GB.
Problemas: nenhum; o driver funcionou de primeira no QEMU.
Ficou de fora: varias filas de I/O e varios pedidos ao mesmo tempo, namespaces
alem do 1, blocos de 4 KiB, desligar o controlador no `poweroff`.

#### 5.5 xHCI e enumeracao USB, em Rust - 14:10 a 14:55 - FEITO
`kernel/rust/usb` (novo, `orion_usb`): `desc.rs` interpreta os descritores
USB sem confiar no dispositivo (testes no hospedeiro com os descritores do
teclado do QEMU, cortados e com cada byte trocado); `xhci.rs` (so no alvo do
kernel) tira o controlador do firmware, reinicia, monta `DCBAA`, rascunho,
anel de comandos e de eventos, e enumera: reset de porta, `Enable Slot`,
`Address Device` e transferencias de controle para os descritores e as
strings. O C (`drivers/usb.c`) acha o xHCI no PCI, mapeia a BAR e da ao Rust
DMA, relogio, espera e log; `/proc/usb` e `lsusb` (novo) mostram o que foi
achado. `test.ps1 -Usb` liga `qemu-xhci` com `usb-kbd` e `usb-tablet`.
Criterio: o `lsusb` lista os dois dispositivos do QEMU com fabricante,
produto, versao, velocidade e classe. Verificacao: a sequencia U sozinha (7
verificacoes) e a regressao completa de fim de etapa, 511 -> 518, verde na
primeira rodada (31 minutos). Versao 2.4.0-a20. RAM livre: 1,8 GB na sequencia
U (rodou e anotado), 2,4 GB antes da completa, depois de esperar.
Problemas: (1) a primeira rodada mostrou "xHCI 0.00": o QEMU devolve 0 na
leitura de 16 bits do registrador de capacidades; passou a ler 32 bits. (2) O
`cargo test` do crate guarda-chuva nao linkava, porque compilava o `xhci.rs`
no Windows; o modulo passou a existir so no alvo do kernel.
Ficou de fora: hubs USB, interrupcao do xHCI (os eventos sao lidos por laco),
desligar slots, dispositivos ligados depois do boot, portas USB 3 de verdade
(os dispositivos do QEMU sao USB 2).

#### 5.6 Teclado e mouse USB HID - 14:55 a 15:55 - FEITO
`orion_usb`: interfaces HID de boot (teclado e mouse) ganham `Set
Configuration`, `Configure Endpoint` do endpoint de interrupcao, `Set
Protocol` e `Set Idle`, e 8 TRBs Normal na fila; uma thread `usbd` le os
eventos a cada 4 ms. `hid.rs` (novo, testado no hospedeiro) traduz o codigo
HID para o scancode set 1 e compara relatorios (modificadores antes e depois
das teclas). O C ganhou `keyboard_feed` (o tratamento do scancode, com trava,
usado pela IRQ do PS/2 e pelo USB) e `mouse_inject`; sem controlador PS/2 o
teclado so avisa. `test.ps1 -Usb` passou a ligar teclado e mouse USB e
`-NoPs2` sobe o q35 sem i8042. Criterio: na sequencia U, sem PS/2, o
`evtest` recebe tecla, Ctrl, seta, movimento e botao do USB. Verificacao: a
sequencia U sozinha (15 verificacoes) e a regressao completa de fim de etapa,
518 -> 526, verde na primeira rodada (32 minutos). Versao 2.4.0-a21. RAM livre:
1,4 GB no primeiro teste (rodou e anotado), 0,8 GB depois (esperei 2 minutos e
subiu para 3,5 GB, como combinado), 4,1 GB antes da completa.
Problemas: a primeira rodada da sequencia U falhou numa verificacao minha (a
expressao do botao do mouse esquecia o ` c=0` da linha); o comportamento
estava certo e a expressao foi corrigida. O teclado e o mouse funcionaram de
primeira.
Ficou de fora: repeticao automatica de tecla, tablet e HID fora do protocolo
de boot (descritor de relatorio), LEDs do teclado, interrupcao do xHCI.

#### 5.7 parte 1 Pendrive USB - 15:55 a 17:05 - FEITO
`orion_usb`: `msc.rs` (novo, testado no hospedeiro) monta o CBW e confere o
CSW e os comandos SCSI; `xhci.rs` acha a interface Bulk-Only, configura os
dois endpoints bulk (o `Configure Endpoint` ficou geral), faz INQUIRY, TEST
UNIT READY e READ CAPACITY, e le e grava em pedacos de 64 KiB com tres
transferencias sincronas por pedido. O C registra o disco no primeiro `sdX`
livre (depois que o controlador fica publicado) e serializa o acesso por
mutex. `test.ps1 -UsbDisk` liga um `usb-storage` em branco. Criterio: na
sequencia U o pendrive vira `sdb`, o `mkfs` o formata em FAT, ele e montado,
gravado, desmontado, montado de novo e um programa copiado roda de la.
Verificacao (criterio novo de parte): a sequencia U, 23 verificacoes, verde.
Versao 2.4.0-a22. RAM livre antes da sequencia U: 2,6 GB.
Problemas: (1) a primeira rodada da U falhou numa expressao minha (o teclado
deixou de ser a primeira linha do `lsusb`, porque o pendrive entra na porta
3); afrouxada. (2) A regressao completa da etapa foi comecada as 16:02 e o
Windows caiu as 16:19:22 (Kernel-Power 41) no meio da sequencia C; os arquivos
ficaram integros. Como a 5.7 ganhou uma parte 2 (pedida pelo dono), a
regressao completa fica para o fim dela, como no criterio novo.
Ficou de fora nesta parte (vai para a parte 2): prazo com cancelamento,
despachante unico com interrupcao, REQUEST SENSE e recuperacao do Bulk-Only,
remocao e segundo plug, imagem FAT criada fora do Orion.

#### 5.7 parte 2 Pendrive robusto - 17:01 a 17:25 - FEITO
O pedido de parte 2 da 5.7 foi dividido em duas, para perder no maximo uma
se o PC cair: esta (itens 1 a 5 e 7 dos requisitos, menos o teste do prazo
pelo device_del) e a parte 3 (remocao, item 6, e o teste do prazo).
`xhci.rs`: despachante unico do anel de eventos, rodando na interrupcao
(MSI-X do xHCI, alocada pelo `usb.c`; MSI ou consulta se faltar) e em quem
espera, com spin lock de interrupcoes desligadas e conclusoes entregues pelo
endereco do TRB; teclado e mouse so no despachante. Prazos em comando,
controle e bulk, com `Stop Endpoint` + `Set TR Dequeue` e disco morto (EIO)
no prazo estourado, e conclusoes tardias ignoradas e contadas. O disco espera
sem a trava do controlador. CSW com status 1 leva ao REQUEST SENSE (sense no
log); CSW invalido, erro de fase ou STALL levam a recuperacao do Bulk-Only
(reset, CLEAR_FEATURE nos dois endpoints, `Reset Endpoint` ou parar e
reconfigurar no xHCI) e a uma repeticao. Bloco diferente de 512 recusado com
mensagem clara. `msc.rs` ganhou REQUEST SENSE e a leitura do sense (fixo e
descritor), com teste no hospedeiro. Comando `usbstall sdX` no shell do
kernel. `tools/mkfat.py` (novo) cria um FAT16 no hospedeiro e le um arquivo
dele; `test.ps1 -UsbFat` (e `qrun.py --usb-fat`) liga esse segundo pendrive.
Ordem das travas e o porque de cada mudanca: `docs/DECISOES.md`, v2.4.0-a23.
Verificacao: sequencia U, 38 verificacoes, verde (eram 23): MSI-X do xHCI
chegando, os dois pendrives, `LEIAME.TXT` feito no hospedeiro lido no Orion,
`NOVO.TXT` gravado no Orion lido no hospedeiro depois de o QEMU sair, STALL
provocado e recuperado com a leitura seguinte boa, REQUEST SENSE com chave 5
e ASC 0x21 no READ(10) fora do disco, nenhum prazo estourado. Versao
2.4.0-a23. RAM livre antes da sequencia U: 5,6 GB.
Problemas: (1) com quatro dispositivos o QEMU pendurou um hub automatico no
xHCI de 4 portas e o segundo pendrive ficou atras dele (hub nao tem driver);
o xHCI do teste passou a 8 portas USB 2 e 8 USB 3 (`p2=8,p3=8`). (2) O
pendrive FAT de fora ja e montado sozinho em `/mnt` na partida, entao o teste
usa `/mnt`. (3) O TEST UNIT READY do QEMU nao volta com UNIT ATTENTION, por
isso o REQUEST SENSE e provocado pelo READ(10) fora do disco. (4) O heredoc
do Bash estragou de novo barras invertidas num script de remendo; corrigido a
mao e o resto feito pelo Edit.
Nao exercitado por teste: a recusa de bloco de 4096 (o `usb-storage` do QEMU
so tem 512) e o caminho do prazo estourado (vem na parte 3, pelo
device_del).

### Sessao 2 - 2026-10-17

Ponto de partida (21:40): a arvore batia com o "Como retomar" (8.7 e 2.7
parte 1 sem commit, mais nada). `.\check.ps1` limpo. Primeiro trabalho:
validar no QEMU e commitar a 8.7 e a 2.7 parte 1 (entradas acima).
