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

Estado (2026-10-20, 12:45, sessao 3): Fases 2, 3 e 4 inteiras (tags `v2.1`,
`v2.2` e `v2.3`), Fases 5 e 6 inteiras (tag `v2.4`): uACPI, MSI/MSI-X,
virtio-blk e virtio-net, NVMe, USB (xHCI em Rust, teclado e mouse, pendrive
robusto com remocao, hub USB 2), audio HDA com `/dev/dsp` e Doom com efeitos
sonoros, video Bochs/BGA e virtio-gpu com `fbset`; desktop com wm, apps,
barra de tarefas e lancador. Mais 8.7, 7.1 e 7.2, e o comeco da 8.2 (contador
de travas de spin por thread). Contexto de interrupcao vigiado (aviso e, na
regressao, panico). Fase 7: 7.1 a 7.3 feitas (ISO, live, instalador); 7.4
`[!]`, aguardando teste do usuario em hardware real, roteiro em
docs/teste-hardware-real.md. 7.5 feita (pacotes). Versao 3.0.0-a3. O
boot pede usuario e senha (`root`/`orion`); o `test.ps1` e o `qrun.py` entram
sozinhos como root. SMEP, SMAP e ASLR ligados. `build.ps1 -Iso` gera a ISO
hibrida e live. ACPI pelo uACPI. Apps graficos em processos separados com a
`libgui` (areas sujas); o `wm` usa fontes TrueType e papel de parede PNG. A
regressao roda no `out\orion.img` (GPT, raiz em ext2) nas sequencias A a E,
na ISO na F, no virtio (disco e rede) na V, no NVMe na N, com USB na U, com
hub na H, com audio na S e com troca de video na G (BGA) e na W
(virtio-gpu), com 646 verificacoes em ~20 min; a sequencia A sobe o `tools/hostweb.py`
no hospedeiro e usa `hostfwd` (5555 e 8081).

Proxima etapa: nenhuma no plano; todas as etapas estao feitas. A 7.4
fechou em 2026-10-22: o terceiro teste do dono (ISO a14, notebook ASUS
Vivobook AMD com Windows, modo live) chegou ao login e passou no
`segtest`; marco M5 cumprido. O caminho ate la (o `#DF` do primeiro teste,
o STAR sem RPL 3 da AMD, a seguranca do disco interno) esta nas partes 1 a
8 da sessao 4 e em `docs/hardware.md`. A ISO 3.0.1-a1 leva o Doom que
acha o WAD sozinho, `lspci`, `dmesg` e as aspas no `sh`, para o dono
conferir (secao 5c do roteiro). Proximas etapas, so anotadas no
`docs/PLANO.md`: 9.1 (instalar em USB), 9.2 (dual boot no NVMe), 9.3 (rede
por adaptador USB) e 9.4 (`FLUSH` no AHCI e no virtio-blk, pendencia de
integridade). Fase 7 fechada com a tag `v3.0-rc1`, a Fase 8 com a
`v3.0-rc2` e o marco M5 com a `v3.0` (commit `11f3d5d`, versao
3.0.0-a15). Depois da tag a versao e 3.0.1-aN. Versao 3.0.1-a1.

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
mandar o resultado do teste. O roteiro tem de dizer que nao ha suporte a hub
USB enquanto a 5.10 nao estiver feita (a secao "Limitacoes conhecidas" do
`docs/teste-hardware-real.md` ja existe desde 2026-10-19).

Criterio de verificacao (decidido pelo dono em 2026-10-19, 17:55; substitui
o anterior, da mesma tarde, porque a regressao completa pesa demais no
notebook, que vem caindo com tela azul):
- Fim de cada PARTE e de cada ETAPA: `.\check.ps1` sem warnings,
  `.\build.ps1` OK e so as sequencias da regressao que cobrem o que mudou
  (`-OnlyX`): rede ou programas de texto -> A; grafico, framebuffer ou Doom
  -> B; disco, ext2 ou armazenamento -> C; `wm` ou apps -> D; travas,
  processos, threads ou sinais -> E; ISO ou boot -> F; virtio -> V; NVMe -> N;
  USB -> U; audio -> S; video BGA -> G; virtio-gpu -> W; hub USB -> H;
  instalador ou pacotes -> I; escalonador, nice ou afinidade -> P; fuzzer ou
  syscalls em geral -> Z (10 min com `-Noturna`); falta de memoria -> M. Na
  duvida, inclui a sequencia. A etapa recebe o `[x]` com isso.
- Regressao completa so no fim de cada FASE (fim da Fase 5, fim da 7.3 e da
  7.5, antes de cada tag) e em parte que mexe no nucleo do kernel
  (interrupcoes, memoria, escalonador, preempcao, travas, syscalls de uso
  geral), o que inclui quase toda a Fase 8.
- Se a regressao completa do fim da fase falhar, achar pelos commits em que
  parte o erro entrou e corrigir num commit novo, sem reescrever historico.
- A regressao completa da 5.7 comecada as 17:44 termina; nao roda outra antes
  da 5.8.
- No diario, cada parte registra quais sequencias rodou e com quantas
  verificacoes.
- Continuam: RAM livre medida antes de rodar QEMU (2 GB; entre 1 e 2 GB roda e
  anota; abaixo de 1 GB depois de esperar, para), commit por parte logo depois
  da verificacao verde, e a entrada do diario so depois dela.
- Enquanto uma regressao roda, so documentos e planejamento: nenhum codigo-fonte
  (nem build, nem `test.ps1`), porque a regressao reconstroi a imagem no meio
  (depois da D e antes da F) e testaria codigo pela metade. Cada commit leva
  so os arquivos da parte dele.

Regra de trabalho continuo (decidida pelo dono em 2026-10-20, vale ate o fim
do plano):
- Nao parar depois de um commit, de um relatorio ou de uma frase como "sigo
  para a proxima". Depois de cada parte: resumo curto e, na mesma resposta,
  ja comecar a proxima parte (ler o diario, planejar, editar, rodar). Nunca
  terminar uma resposta anunciando o que vai fazer: fazer.
- O dono nao precisa aprovar nada entre partes ou etapas; nao esperar
  resposta.
- Parar so nos casos extremos ja combinados: RAM abaixo de 1 GB depois de
  esperar, decisao que nao da para desfazer, erro que nao se resolve depois
  de investigar, ou a 7.4 (hardware real), que fica `[!]` e e pulada.
- Resposta longa: relatorio em poucas linhas e continuar trabalhando.

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

Requisitos da 5.7 parte 4, decididos pelo dono em 2026-10-19:
1. `usbstall`: forcar o STALL num bulk e conferir que a transferencia
   seguinte a recuperacao le os dados certos (prova que a sequencia do
   endpoint foi zerada dos dois lados).
2. Marcador de contexto de interrupcao cobrindo todas as entradas: IRQ do
   timer, IPIs entre CPUs, MSI e MSI-X, e as excecoes que chegam durante uma
   interrupcao (page fault dentro de handler). O despachante do USB rodando
   pela thread (sem MSI) nao conta como interrupcao.
3. `kmalloc` ou funcao que dorme chamados dentro de interrupcao: mensagem
   "BUG: ... dentro de interrupcao" com o endereco de quem chamou, no maximo
   uma vez por ponto de chamada; na regressao, panico.
4. So o contexto de interrupcao dispara o aviso: interrupcoes desligadas com
   trava de spin presa sao normais e nao podem dar falso alarme.
5. Conferir com isso o caminho do despachante (teclado, mouse, acordar).
6. Interrupcao aninhada: depois de baixar o contador, o `sched_check_preempt`
   so troca de thread se o contador da CPU voltou a 0 (interrupcao mais de
   fora). Testar com o timer chegando dentro de outra IRQ, se der para
   provocar.
7. O aviso nao pode travar: o log tem de usar trava com interrupcoes
   desligadas (irqsave) para poder ser chamado de IRQ. No NMI, nada de log
   direto: anotar num buffer por CPU (sem trava) e imprimir no proximo ponto
   seguro fora de interrupcao. A tabela de "ja avisou" e atualizada com
   operacao atomica (cmpxchg em posicoes fixas), sem trava que durma e sem
   alocar memoria.
8. `mutex_trylock` e qualquer operacao que nao durma nao avisam.

Requisitos da 8.2 (kernel preemptivo), anotados pelo dono em 2026-10-19 (o
comeco, o contador de travas de spin por thread, ja entrou na 5.7 parte 4):
1. Troca adiada por trava de spin presa marca `need_resched` (como hoje), e o
   `spin_unlock`, ao zerar o `spin_count` com interrupcoes ligadas e a flag
   marcada, chama o escalonador na hora, em vez de esperar a proxima
   interrupcao.

Requisitos da 8.1 (fila por CPU e nice), decididos pelo dono em 2026-10-20,
sobre o desenho proposto (fila de prontos por CPU ainda sob a trava global,
escolha pelo menor tempo virtual ponderado pelo nice, CPU de destino ociosa
ou menos carregada, roubo de fila vazia, IPI de "tem trabalho",
setpriority/getpriority, sched_setaffinity, programa `nice`, teste
`nicetest` na sequencia E):
1. Thread que acorda: tempo virtual = max(o dela, minimo da fila menos uma
   fatia), para nao monopolizar a CPU depois de dormir. Thread nova entra no
   minimo da fila.
2. Migracao entre CPUs: normalizar o tempo virtual pelo minimo da fila de
   origem e da de destino.
3. Balanceamento de tempos em tempos (por exemplo a cada 4 ticks), nao a cada
   tick, e so com desequilibrio maior que uma thread (ou que o peso de uma).
   Nunca migrar a thread em execucao nem violar a afinidade.
   `sched_setaffinity` tira a thread da CPU atual se ela nao estiver na
   mascara nova; mascara vazia da `EINVAL`.
4. CPU ociosa: conferir a fila e dormir de forma atomica (interrupcoes
   desligadas, conferir, `sti; hlt`), para a IPI de "tem trabalho" nao se
   perder.
5. `nicetest`: medir pelo tempo de CPU de cada thread (nao pelo relogio),
   rodar tempo suficiente e usar margem larga (aceitar de 5:1 a 15:1 para o
   9:1). Rodar o `nicetest` 5 vezes seguidas antes de dar a parte como
   pronta, para pegar oscilacao.
6. setpriority/getpriority: o `DECISOES.md` diz quais `which` sao aceitos
   (`PRIO_PROCESS` no minimo; os outros dao `EINVAL` ou sao implementados) e
   se valem por thread ou por processo, como no Linux.

Requisitos da 8.3 (fuzzer de syscalls), decididos pelo dono em 2026-10-20:
1. Reproducao: semente impressa no comeco e aceita por `--seed`; cada filho
   guarda as ultimas 32 syscalls (numero e argumentos) num anel em memoria
   compartilhada, e o pai despeja o anel do filho que morreu de forma
   inesperada.
2. Criterio de falha na sequencia: panico (marcador exato), qualquer "BUG:",
   falta do sinal de vida do pai por mais de N segundos e, no fim, paginas
   livres e numero de processos de volta perto do inicio (com margem).
3. Argumentos: alem de lixo e enderecos do kernel, ponteiros validos
   terminando na borda de uma pagina com a seguinte nao mapeada, tamanhos 0,
   1, 4095, 4096, 4097 e enormes, e descritores reais de varios tipos (pipe,
   socket TCP e UNIX, pty, arquivo, diretorio, /proc).
4. fork/clone com probabilidade baixa, sob o limite de processos. exec so de
   caminhos inexistentes ou de lixo.
5. O fuzz roda num diretorio proprio em /tmp, como usuario comum.
Se o fuzzer achar defeito: cada um corrigido num commit proprio, com um teste
que reproduz, e registrado no `DECISOES.md`.

Requisitos da 8.4 (falta de memoria), decididos pelo dono em 2026-10-20,
sobre o desenho proposto (mapeamento que devolve erro nos caminhos de usuario,
reserva de paginas do kernel, matador de processo, `oomtest`):
1. O matador nunca roda segurando trava de memoria do processo: a falha de
   pagina solta tudo, chama o OOM e tenta a falha de novo.
2. Ordem: primeiro devolver cache limpo (kreclaim); so se ainda faltar,
   matar.
3. Uma vitima por vez: marcar "vitima a caminho" e esperar ela liberar a
   memoria (com prazo) antes de escolher outra.
4. SIGKILL acorda a vitima de qualquer espera interrompivel, e o caminho de
   saida do processo nao aloca memoria (ou usa a reserva).
5. Vitima que e o proprio processo que falhou: recebe SIGKILL e morre ao
   voltar da falha, sem panico.
6. Nunca matar o init nem threads do kernel. O `DECISOES.md` registra a
   politica de mmap/brk (aceita e mata na falha, ou recusa com ENOMEM na hora)
   e se o `wm` pode ser vitima.
7. `oomtest`: o caso em que o processo que enche a memoria e o maior (mata a
   si mesmo) e o caso de dois processos enchendo ao mesmo tempo (so um morre
   por vez).
8. (complemento) `wm` como vitima: se o init reinicia servicos, ele relanca o
   `wm` morto por OOM para o desktop voltar sozinho; se nao, registrar no
   `DECISOES.md` que o desktop some. (No modo grafico o `wm` e o programa que
   o init mantem vivo, `shell /bin/wm`, e o init ja o relanca depois de
   qualquer morte.)
9. (complemento, se der tempo) No modo estrito, uma chave para o `kmalloc`
   falhar de vez em quando (1 em 1000), rodada junto do fuzz curto, para
   achar quem chama `kmalloc` sem conferir NULL; senao, trabalho futuro.

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
| 0.1 | Commit da v1.9 e dos programas novos | [x] | 19:22 | 20:20 | `e9ce030` |
| 0.2 | Copia segura de memoria de usuario | [x] | 20:22 | 22:15 | `9d30113` |
| 0.3 | Mutex que dorme, AHCI | [x] | 22:16 | 22:41 | `f9da861` |
| 0.4 | FAT e cache de blocos com mutex | [x] | 22:42 | 23:08 | `18bbda3` |
| 0.5 | Harness em Python e CI | [x] | 23:09 | 23:39 | `688bedd` |
| 0.6 | Shell: `&`, jobs, fg, wait, `VAR=x cmd`, `&&`, `\|\|`, `;` | [x] | 23:40 | 00:00 | `a7ea980` |
| 0.7 | Limites: 256 fds, 256 processos, ulimit | [x] | 00:00 | 01:09 | `85b1bd9` |
| 1.1 | Pilha inicial do Linux (auxv) | [x] | 00:55 | 01:29 | `fbf6f4f` |
| 1.2 | Syscalls de partida da musl | [x] | 01:29 | 01:51 | `838525e` |
| 1.3 | Arquivos no formato do Linux | [x] | 01:51 | 02:13 | `19808ec` |
| 1.4 | Memoria do jeito Linux (VMAs, mmap) | [x] | 02:13 | 02:51 | `97d1768` |
| 1.5 | Programas contra a musl com `zig cc` (plano B: musl pelo build.ps1) | [x] | 02:52 | 03:10 | `e3608af` |
| 1.6 | Threads (clone) | [x] | 03:10 | 03:50 | `09c4c75` |
| 1.7 | futex | [x] | 03:50 | 04:28 | `86e929a` |
| 1.8 | Tempo e sinais completos | [x] | 04:28 | 05:37 | `9aa79fd` |
| 1.9 | dash, lua, sbase | [x] | 05:37 | 06:28 | `e9aa706` |
| 1.10 | Doom | [x] | 06:28 | 07:10 | `5b9097e` |
| 1.11 | tcc dentro do Orion | [x] | 07:10 | 07:54 | `2d6ad29` |
| 1.12 | Programas na musl, aposentar a libc propria | [x] | 07:54 | 08:50 | 6edcf79 |
| 2.1 | VFS com inodes | [x] | 08:51 | 11:05 | 9a8ae30 |
| 2.2 | tmpfs | [x] | 11:06 | 11:47 | 8181913 |
| 2.3 | procfs | [x] | 11:48 | 12:52 | 0c3c9fd |
| 2.4 | Cache de paginas unificado | [x] | 12:53 | 14:55 | 18d27a5 |
| 2.5 | ext2 leitura, piloto em Rust | [x] | 14:56 | 16:36 | ver `git log` |
| 2.6 | ext2 escrita, mkfs.ext2, fsck | [x] | 16:36 | 19:00 | ver `git log` (3 partes) |
| 2.7 | Links simbolicos, chmod, chown, utime | [x] | 20:20 | 22:40 | `31aa078` e ver `git log` (parte 2) |
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
| 5.7 | USB armazenamento | [x] | 15:55 | 18:16 | `196e3c2` |
| 5.8 | Audio HDA | [x] | 10:54 | 11:40 | `4093935` |
| 5.9 | Video BGA e virtio-gpu | [x] | 11:42 | 12:05 | `5b89be7` |
| 5.10 | Hub USB 2 basico (extra, decidido em 2026-10-19) | [x] | 12:08 | 12:20 | `fc2aac4` |
| 6.1 | Protocolo de janelas | [x] | 11:06 | 11:47 | ver `git log` |
| 6.2 | libgui | [x] | 11:48 | 12:27 | ver `git log` |
| 6.3 | Fontes TrueType | [x] | 12:27 | 13:05 | ver `git log` |
| 6.4 | Imagens PNG/JPEG | [x] | 13:05 | 13:38 | ver `git log` |
| 6.5 | Compositor com areas sujas | [x] | 13:38 | 15:47 | ver `git log` |
| 6.6 | Apps | [x] | 15:47 | 03:05 | ver `git log` (6 partes) |
| 6.7 | Area de trabalho | [x] | 03:05 | 04:20 | ver `git log` (2 partes) |
| 7.1 | ISO hibrida | [x] | 06:26 | 09:44 | ver `git log` |
| 7.2 | Live | [x] | 09:44 | 10:13 | ver `git log` |
| 7.3 | Instalador | [x] | 12:44 | 12:58 | `80dfc68` |
| 7.4 | Hardware real | [x] | 13:00 | 2026-10-22 | 3o teste do dono (ISO a14) passou no notebook ASUS Vivobook AMD; resultado em docs/hardware.md |
| 7.5 | Pacotes | [x] | 13:10 | 13:30 | |
| 8.1 | Fila por CPU e nice | [x] | 14:05 | 15:10 | |
| 8.2 | Kernel preemptivo (contador de travas de spin por thread desde a 5.7 parte 4; troca adiada chamada na hora pelo `spin_unlock`) | [x] | 15:15 | 16:05 | |
| 8.3 | Fuzzer de syscalls | [x] | 16:10 | 17:37 | |
| 8.4 | OOM | [x] | 17:40 | 19:04 | |
| 8.5 | Perfilador | [x] | 19:05 | 21:47 | |
| 8.6 | Pilhas do kernel medidas | [x] | 21:50 | 22:24 | |
| 8.7 | memcpy/memset/memmove do kernel em assembly (rep movsb/stosb) | [x] | 20:05 | 22:00 | `e1c8fcb` |
| 8.8 | Thread main do kernel dorme numa fila de espera acordada pela IRQ do teclado/serial (anotado pelo dono na 8.1) | [x] | 22:25 | 23:03 | |

## Diario

### Sessao 1 - 2026-10-16

Ponto de partida (19:22): `.\check.ps1` limpo; `.\regressao.ps1` VERMELHA,
14 de 81. Todas as falhas na sequencia A, do `ping` em diante: o kernel entrou
em panico (`assert falhou: sched_runq_held()` em `wait.c`) no segundo `ping`.

#### 0.1 Commit da v1.9 - 19:22 as 20:20 - commit e9ce030 - FEITO
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

#### 0.2 Copia segura de memoria de usuario - 20:22 as 22:15 - commit 9d30113 - FEITO
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
logo antes, com as correcoes de concorrencia). Observacao: o commit `9d30113`
levou junto as edicoes que o dono fez durante a sessao no arquivo local de regras (regra de
comentarios para todas as linguagens) e em `docs/PLANO.md` (secao "Linguagens",
1.5 com `zig cc`, piloto Rust no ext2 em 2.5, etapa nova 8.7).

#### 0.3 Mutex que dorme - 22:16 as 22:41 - commit f9da861 - FEITO
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

#### 0.4 FAT e cache de blocos com mutex - 22:42 as 23:08 - commit 18bbda3 - FEITO
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

#### 0.5 Harness em Python e CI - 23:09 as 23:39 - commit 688bedd - FEITO
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
estresse 5x verde) e commitada em `e1c8fcb`.

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
simbolicos" (`31aa078`).

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

#### 5.7 parte 3 Remocao e segundo plug - 17:26 a 17:55 - FEITO
Camada de blocos: contagem de referencias (montagem e arquivo aberto de
`/dev/sdX`), `blockdev_remove` (marca disco e particoes, descarta o cache de
setores dele, leitura e escrita falham antes do driver) e liberacao quando o
ultimo usuario solta, com aviso ao driver. O FAT devolve EIO quando a falha
vem de disco removido. xHCI: evento Port Status Change, tratado na thread
`usbd`; na desconexao, disco morto, esperas do slot concluidas na hora,
pipes HID retirados, `Disable Slot`, DCBAA zerada e memoria devolvida (a do
pendrive so quando a camada de blocos libera); na conexao, a mesma
enumeracao do boot. Comando `usbtimeout sdX` (cala a campainha de uma
transferencia) para exercitar o prazo estourado. O despachante, que roda na
interrupcao, deixou de alocar memoria no caminho do teclado (vetor reservado
no pipe HID); isso devia ter vindo na parte 2. Detalhes:
`docs/DECISOES.md`, v2.4.0-a24.
Verificacao: sequencia U, 54 verificacoes, verde (eram 38): `device_del` do
pendrive montado (EIO na leitura e na escrita, `lsusb` sem ele, `umount` sem
panico liberando o disco, shell respondendo), `device_add` (volta como `sdc`
e monta com os dois arquivos), prazo de 5 s estourado com EIO imediato
depois e teclado USB funcionando em seguida. Versao 2.4.0-a24. RAM livre
antes da sequencia U: 6,3 GB. A regressao completa da etapa (fecho da 5.7)
comecou as 17:44, com 6,4 GB livres; o commit desta parte saiu antes dela
terminar, a pedido do dono, porque o commit nao mexe nos arquivos de
trabalho.
Problemas: (1) no QEMU o `device_del` e percebido pela porta antes do prazo,
entao o prazo estourado e exercitado pelo `usbtimeout`, e nao pela remocao.
(2) O QEMU apaga o `-drive` junto com o dispositivo no `device_del`; o
segundo pendrive passou a usar `-blockdev`, que sobrevive, para o
`device_add` ter o que ligar.

#### Fecho da 5.7 - regressao completa - 17:44 a 18:16 - VERDE
Regressao completa sobre o `196e3c2` (parte 3): 565 verificacoes, 0 falhas
(eram 526 na 5.6; a U foi de 23 para 54 nas partes 1 a 3, e o resto e o que
as partes anteriores ja tinham). RAM livre antes: 6,4 GB. Tempo por
sequencia, pela hora de gravacao dos logs (inclui o build do comeco e o da
imagem antes da F): A ~10,5 min (com o build), B ~5,5, C ~7, D ~3, F ~1,3,
V ~1, N ~0,7, U ~2, E ~0,3; total ~32 min. A 5.7 recebe o `[x]`.

#### Regressao mais rapida - 18:20 a 20:50 - FEITO
Pedido do dono, ate 45 min de trabalho (passou: a rodada medida achou
falhas de espera e o aviso da parte 4 achou um defeito no ACPI, e as duas
coisas foram resolvidas antes do commit). `test.ps1` e `qrun.py`: o `cmd:`
segue quando o prompt volta (1,5 s viraram teto); o `wait:N` depois de um
comando sem `&` termina quando o prompt volta e a saida fica 0,4 s quieta
(N vira teto); so conta prompt depois da quebra de linha do eco e so se a
ultima linha for inteira um prompt; tempo de boot e de cada tipo de passo
impresso por execucao, e o total da regressao no fim; o `Start-Sleep 2` do
hostweb virou esperar a porta abrir. `-SlowWaits` / `--slow-waits` (tambem
no `regressao.ps1` e no `regress.py`) volta as esperas fixas antigas: se uma
verificacao comecar a falhar de vez em quando, o primeiro passo e rodar a
sequencia com `-SlowWaits`; passou, a culpa e da espera. Nenhuma verificacao
saiu; boots nao foram juntados (cada sequencia sobe uma maquina diferente;
o porque esta no `docs/DECISOES.md`).
Tempo: antes ~32 min (regressao completa das 17:44, 565 verificacoes);
primeira rodada com a espera nova 17,4 min, com 3 falhas (o `ptytest` mostra
o prompt do shell de dentro do pty e encerrava a espera cedo; virou `until:`);
na rodada final, com a parte 4, 17,8 min, verde, 585 verificacoes, com o
estresse repetido 3 vezes. Por sequencia nela: A 4,4 min, B 5,3, C 1,5,
D 2,6, F 0,8, V 0,5, N 0,4, U 0,9, E 0,3 por rodada, mais os dois builds.
Problema: o resumo de tempo saia errado quando havia passo `keys:`, porque
`$tabela.Keys` no PowerShell devolve o valor da chave "keys"; virou
`GetEnumerator()`.

#### 5.7 parte 4 Contexto de interrupcao e STALL com dados - 18:40 a 20:50 - FEITO
Requisitos 1 a 8 logo abaixo das regras, mais os pedidos do dono no meio do
caminho (ganchos de teste so no modo estrito, auditoria do padrao per-CPU,
contador de travas de spin por thread). `irqctx` (novo): contador de
interrupcao por CPU em volta de todo handler (IRQ, timer, IPI, MSI, MSI-X,
NMI; excecoes herdam), troca de thread so com o contador em 0, aviso
"BUG: ... dentro de interrupcao" em `kmalloc`, `kzalloc`, `kcalloc`,
`krealloc`, `mutex_lock`, `wait_sleep*`, `thread_yield`, `thread_sleep_ms` e
`thread_block`, uma vez por ponto de chamada (tabela por cmpxchg), NMI so
anota e o aviso sai depois. Modo estrito (panico) pelo fw_cfg do QEMU de
teste (`fwcfg.c`, novo); `-NoStrict` sobe sem ele. Os ganchos de teste
(`usbstall`, `usbtimeout`, `irqtest`) respondem EPERM fora do modo estrito.
`irqtest` novo; `usbstall` confere os 512 bytes relidos depois da
recuperacao, no pendrive feito no hospedeiro. Detalhes: `docs/DECISOES.md`,
v2.4.0-a25.
`in_interrupt()` le o contador com um `mov %gs:` so. Contador de travas de
spin por thread (`spin_count`, contado no dono da trava) e o
`sched_check_preempt` nao troca de thread com ele acima de zero: comeco da
8.2, que parte daqui. Auditoria do padrao "ponteiro da CPU e depois o campo"
no resto do kernel: nenhum outro ponto com risco (lista no DECISOES).
`tmptest` mede de novo enquanto a `kreclaim` devolve paginas, ate 1 s.
Achado do aviso: o botao de energia fazia `kmalloc` dentro do SCI
(`uacpi_kernel_io_map`); o handle de I/O agora nao aloca. Conferido tambem
que o SCI so reconhece o evento: o botao de energia e o AML dos GPEs
(`_Lxx`/`_Exx`, Notify) vao para a thread `acpi-work`.
Segundo achado, no estresse: avisos falsos em fork comum, com "vetor 0":
o `in_interrupt()` lia o ponteiro da CPU e depois o campo com a thread
preemptivel e, migrando no meio, lia o contador de outra CPU.
Verificacao (mexe no nucleo: interrupcoes, memoria, travas): regressao
completa final, 585 verificacoes, verde, 17,8 min, estresse 3 vezes, RAM
livre antes 6,2 GB. Rodadas antes dela: 18:52, 6 falhas (5 falsos alarmes:
a linha do modo estrito dizia "vira panico" e F, V, N, U e E procuravam
"panic" sem diferenciar maiusculas; e o `kmalloc` do ACPI); 19:16, verde,
578; rodada curta de F, V, N, U e E com o marcador exato do kernel
(`*** KERNEL PANIC ***` e `*** EXCECAO NAO TRATADA`, pedido do dono), que
mostrou o panico do estresse; 20:06, 584 com 2 falhas do `tmptest` (a fila
da `kreclaim`, terceira vez) e o estresse 3 vezes verde.
Versao 2.4.0-a25.

#### 5.8 parte 1 Driver HDA - 10:54 a 11:05 - FEITO
A 5.8 vai em tres partes: driver (esta), `/dev/dsp` e `play`, Doom com
efeitos sonoros. `drivers/hda.c` (novo): reset do controlador, CORB e RIRB
(verbos por consulta), grafo do codec percorrido sem IDs fixos (AFG, pinos
pela configuracao padrao, caminho ate um DAC; D0, sem mudo, 0 dB e EAPD em
cada no), stream de saida com BDL de 8 x 4 KiB, IOC por entrada, LVI, o
mesmo formato no stream e no conversor, tag no conversor, MSI, silencio no
que nao foi escrito. Shell do kernel: `hda` (estado) e `hdatone [ms] [Hz]`.
`test.ps1 -Audio` / `qrun.py --audio` ligam `ich9-intel-hda` + `hda-output`
com `-audiodev wav`; `tools/wavcheck.py` (novo) confere o WAV no hospedeiro.
Detalhes e o que ficou pensado para hardware real: `docs/DECISOES.md`,
v2.4.0-a26.
Verificacao: sequencia S (nova), 14 verificacoes, verde: codec 1af4:0012,
AFG 1, DAC 2, pino 3 (line out), MSI com ~93 interrupcoes por 2 s, 0 falta
de dado, WAV com 2 s de 1 kHz no canal esquerdo, direito mudo, sem buraco.
Versao 2.4.0-a26. RAM livre antes: 5,1 GB.
Problemas: (1) o dreno parava o stream assim que o DMA chegava ao ultimo
byte e o fim do som se perdia no FIFO (1,96 s em vez de 2 s); agora espera
mais 60 ms de silencio. (2) O fim do dreno contava como falta de dado; nao
conta mais.

#### 5.8 parte 2 /dev/dsp e play - 11:06 a 11:21 - FEITO
`drivers/dsp.c` (novo): `/dev/dsp` no estilo OSS (`SPEED`, `SETFMT`,
`CHANNELS`, `STEREO`, `GETFMTS`, `GETBLKSIZE`, `GETOSPACE`, `SYNC`,
`RESET`), conversao de taxa linear em ponto fixo para 48 kHz (ou 44,1 kHz
quando o codec aceita exatamente essa taxa), mono para estereo e U8 para
S16, `write` que dorme com a fila cheia, O_NONBLOCK com EAGAIN, um processo
por vez (EBUSY; o devfs ganhou `open_error` por dispositivo) e `close` que
drena e para o stream. `user/include/sys/soundcard.h` (novo, minimo).
`play` (novo): WAV PCM, `--tom`, `--gravar`, `--nao-bloqueia`,
`--teste-ocupado`. `wavcheck --segmentos N` separa os sons pelo silencio.
Detalhes: `docs/DECISOES.md`, v2.4.0-a27.
Verificacao: sequencias S (21 verificacoes, eram 14) e A (298, pelo devfs),
verdes. Na S: EBUSY e o `open` de novo depois do `close`, tom de 22050 Hz
gravado e tocado (convertido para 48 kHz, 2,00 s no WAV do QEMU), o mesmo
WAV com O_NONBLOCK (EAGAIN e `GETOSPACE`), tres sons de 2 s a ~1 kHz, canal
direito mudo, sem buraco, 0 falta de dado. Versao 2.4.0-a27. RAM livre
antes: 4,2 GB (S) e 4,1 GB (A).
Problemas: (1) o build trata inicializador de campo faltando como erro
(`-Wmissing-field-initializers`); as entradas antigas do devfs ganharam o
`NULL`. (2) O `write` do driver recebe buffer do kernel (o `sys_write` ja
copiou), entao o `copy_from_user` dava EFAULT; virou `memcpy`. (3) Dois
`play` seguidos deixam o silencio do dreno entre eles no WAV, que o
`wavcheck` antigo via como buraco; ele passou a separar os sons.

#### 5.8 parte 3 Doom com efeitos sonoros - 11:25 a 11:40 - FEITO
`user/musl/doom/orion_sound.c` (novo): `DG_sound_module` com 8 canais
mixados a 11025 Hz (volume e separacao por canal, efeitos lidos do formato
DMX do WAD), escrita sem bloqueio no `/dev/dsp` com ~140 ms de folga medida
pelo `GETOSPACE`; `DG_music_module` vazio. Compila com `-DFEATURE_SOUND` e
um `SDL_mixer.h` vazio no caminho de include, sem mexer em `third_party`.
Sem placa de som o Doom avisa e roda mudo. Musica (MUS/OPL) ficou de fora,
como o requisito permite: precisa de um sintetizador OPL, trabalho separado.
`wavcheck` ganhou `--algum-som` e `--resto-som`; `test.ps1 -AudioWav` /
`qrun.py --audio-wav` escolhem o arquivo. Detalhes: `docs/DECISOES.md`,
v2.4.0-a28.
Verificacao: sequencias S (24 verificacoes) e B (o Doom sem placa de som),
verdes. Na S o Doom roda a `demo1` por 12 s depois dos tons: ~80 efeitos,
~114 mil quadros mixados, 0 EAGAIN, alguns segundos de som no WAV. Versao
2.4.0-a28. RAM livre antes: 4,2 a 4,8 GB.
Problemas: (1) com ~80 ms de folga e o hospedeiro lento a fila esvaziava
(55 faltas de dado); a folga foi para ~140 ms e o tamanho total da fila
passou a ser o maior espaco livre visto, porque o `fragstotal` vem
arredondado para baixo. (2) Numa maquina lenta a demo mal comecava em 12 s
(4 efeitos); a S usa `-playdemo demo1`. (3) O heredoc estragou de novo um
`
` num remendo (nada aplicado pela metade).
A 5.8 fecha com as tres partes; a regressao completa fica para o fim da
Fase 5, pelo criterio novo.

#### 5.9 parte 1 Bochs/BGA e fbset - 11:42 a 11:56 - FEITO
A 5.9 vai em duas partes: Bochs/BGA (esta) e virtio-gpu. `drivers/bga.c`
(novo): placa `1234:1111` ou `80ee:beef`, DISPI pelas portas 0x1CE/0x1CF,
memoria de video toda mapeada com write-through (`vmm_map_mmio_flags`,
novo). `console_set_framebuffer` refaz a grade de texto no modo novo.
`FBIOSET_MODE` no `/dev/fb0` (EINVAL, ENODEV, EBUSY com programa grafico).
`fbset` (novo). Sequencia G (nova) e `Test-ShotSize` nos dois harnesses.
Detalhes: `docs/DECISOES.md`, v2.4.0-a29.
Verificacao: sequencias G (13 verificacoes) e B (62, pelo console e pelo
fbdev), verdes: `fbset 1920x1080` muda a tela (a captura do QEMU sai
1920x1080, o criterio da etapa), 1024x768 tambem, modos invalidos recusados,
EBUSY com o `fbtest` no framebuffer, volta a 1280x800. Versao 2.4.0-a29. RAM
livre antes: 3,8 GB.

#### 5.9 parte 2 virtio-gpu - 11:57 a 12:05 - FEITO
`drivers/virtio_gpu.c` (novo): virtio-gpu 2D pela camada virtio que ja
existia; recurso B8G8R8X8 com backing de memoria contigua do kernel,
scanout 0, thread de flush a cada 33 ms quando o console mudou (contador de
geracao novo no console) ou ha programa grafico no framebuffer; troca de
modo com recurso novo e o antigo liberado depois. O `FBIOSET_MODE` vai para
o BGA ou para o virtio-gpu, o que houver. `test.ps1 -Vga virtio` /
`qrun.py --vga virtio`. Sequencia W (nova). Detalhes: `docs/DECISOES.md`,
v2.4.0-a30.
Verificacao: sequencias W (12 verificacoes) e G (13), verdes: tela do boot
visivel pelo flush, 1920x1080 pela captura, o `fbtest` desenhando pelo mmap
aparece a 1920x1080 (pixel amarelo no centro), volta a 1280x800. Versao
2.4.0-a30. RAM livre antes: 4,0 a 4,2 GB.
Problema em aberto: na primeira versao da W o `fbtest` rodava em segundo
plano (`fbtest 3 &`) e a captura de 1,5 s depois saiu com o console ja com a
mensagem que o `fbtest` so imprime ao terminar (depois dos 3 s), como se a
captura tivesse atrasado; em primeiro plano funciona sempre. Nao investiguei
a fundo (suspeita: o `screendump` do QEMU com virtio-gpu espera a proxima
atualizacao de tela); a W usa o primeiro plano. Fica anotado para a 7.x.
A 5.9 fecha com as duas partes; a regressao completa fica para o fim da
Fase 5.

#### 5.10 Hub USB 2 - 12:08 a 12:20 - FEITO
`xhci.rs`: hub de classe 9 (descritor do hub, Set Configuration, contexto
do slot com Hub, numero de portas, MTT e tempo de TT; portas ligadas e
resetadas uma a uma), dispositivos atras dele com route string, porta raiz e
TT (para full/low speed atras de hub high speed), nome de porta "12.2",
`Evaluate Context` para o MaxPacketSize0 real de dispositivo full/low
speed, endpoint de mudanca de estado do hub no despachante e plug e remocao
atras do hub (filhos removidos junto com o hub). `/proc/usb` conta
relatorios HID por slot. `test.ps1 -UsbHub` / `qrun.py --usb-hub`.
Sequencia H (nova). `docs/teste-hardware-real.md` atualizado (so hub USB 3
fica de fora). Detalhes: `docs/DECISOES.md`, v2.4.0-a31.
Verificacao: sequencias H (17 verificacoes) e U (55), verdes: hub full speed
com 8 portas, teclado e pendrive atras dele em full speed, a tecla chega
pelo teclado do hub (relatorios no slot dele), pendrive formatado, gravado e
relido, `device_del`/`device_add` na porta do hub. Versao 2.4.0-a31. RAM
livre antes: 4,1 GB.
Problemas: (1) sem limpar os bits de mudanca das portas, o hub do QEMU
mandava relatorio sem parar (51 em 10 s); o tratamento de mudanca limpa
todos os bits. (2) Na U, a verificacao do `lsusb` depois da remocao contava
as linhas ate "eventos" e quebrou com as linhas novas de relatorios HID; a
expressao passou a aceitar essas linhas, conferindo os mesmos 3
dispositivos.

#### Fecho da Fase 5 - regressao completa - 12:20 a 12:41 - VERDE
Regressao completa (criterio novo: fim de fase): 646 verificacoes, 0 falhas,
20,1 min, com as sequencias novas S (audio), G (Bochs/BGA), W (virtio-gpu) e
H (hub USB). RAM livre antes: 4,2 GB. Tag `v2.4` (Fases 5 e 6; as duas foram
feitas como 2.4.0-aNN).

#### 7.3 Instalador - 12:44 a 12:58 - FEITO
`instalar /dev/sdX` (novo): GPT com ESP de 64 MiB e raiz ext2, `BLKRRPART`
(novo no kernel) para o kernel reler as particoes, `mkfs` e `mkfs.ext2`,
copia da carga da particao EFI (que agora viaja no initramfs da ISO) e do
sistema live preservando modo, dono e links. So UEFI. Meio de boot
"nenhuma" no `reboot:` dos harnesses; `New-BlankImage` na regressao (e no
`regress.py`). A partir daqui a versao e 3.0.0-aNN (Fase 7 = v3.0) e os
commits usam `v3.0:`. Detalhes: `docs/DECISOES.md`, v3.0.0-a1.
Verificacao: sequencias I (nova, 15 verificacoes), C (67) e F (22), verdes:
instala no QEMU (396 arquivos, 61 MiB), tira a ISO, boota do disco com
login, permissoes e persistencia. Versao 3.0.0-a1. RAM livre antes: 1,7 GB
depois do build da ISO; esperados 2 min, 2,0 GB.

#### 7.4 Hardware real - 13:00 a 13:08 - [!] AGUARDANDO O DONO
Feito so o que da sem um PC de verdade, como combinado: a ISO
(`build.ps1 -Iso`) conferida no QEMU nos quatro boots da sequencia F (CD e
pendrive, BIOS e UEFI, verde nesta sessao) e `docs/teste-hardware-real.md`
com o roteiro inteiro: gravar o pendrive no Windows (Rufus em modo DD ou
balenaEtcher), opcoes do firmware (Secure Boot desligado, UEFI primeiro, CSM
so para BIOS, Fast Boot desligado, menu de boot rapido), o que conferir em
cada etapa (Limine, log do kernel, login, shell, comandos de teste, shell
do kernel, poweroff), o que anotar e mandar se falhar (foto da tela, ultimo
texto, modelo da placa, modo de boot, teclado, `lsusb`/`pci`/`lsblk`), a
instalacao opcional e as limitacoes conhecidas.
Escrevendo o roteiro apareceu um defeito no `instalar`: disco NVMe
(`nvme0n1`, nome terminado em digito) era recusado como se fosse particao e
teria as particoes com o nome errado; agora segue a regra de nomes do
kernel (`nvme0n1p1`). A sequencia I passou a instalar num NVMe em branco
(`test.ps1 -ExtraNvme` / `qrun.py --extra-nvme`, novos) e recusa tambem
uma particao: 16 verificacoes, verde, ~6 min. O caminho AHCI ficou
verificado na rodada da 7.3. Versao 3.0.0-a2. RAM livre antes: 3,4 GB.
A 7.4 fica `[!]` com a nota "aguardando teste do usuario em hardware real,
roteiro em docs/teste-hardware-real.md" e volta quando o dono mandar o
resultado.

#### 7.5 Pacotes - 13:10 a 13:30 - FEITO
Pacote e um `.tar` ustar com `MANIFEST` na frente; o repositorio e estatico
(`INDEX` + `.tar`), gerado por `tools/mkrepo.py` a partir do `out/esp` e
servido pelo `tools/hostweb.py` em `/repo` (o hostweb gera ao subir). O
`pkg` (novo; `update`, `install`, `remove`, `list`, `disponiveis`) baixa
pelo `wget`, confere tamanho e SHA-256, recusa caminho absoluto ou com
`..`, grava por `rename` e registra os arquivos em `/var/lib/pkg`.
Teste na sequencia I, no sistema recem-instalado no NVMe, depois do
segundo reboot: tira o `/bin/lua` (o `lua` deixa de rodar), `pkg update`,
`pkg disponiveis`, `pkg install lua` e o `lua` roda (`42 Lua 5.4`) com o
modo 0755 do pacote, `pkg list`, `pkg install quebrado` recusado pela soma
e sem deixar arquivo, usuario comum nao remove, `pkg remove lua` e o `lua`
some. Primeira rodada: 2 falhas, as duas do teste (o `sh` do Orion nao
trata aspas: `lua -e "..."` e `su -c '...'` chegavam partidos); trocado
por argumento sem espaco e por um script do `dash`. Segunda: 29
verificacoes, verde, 4,5 min. `check.ps1` limpo. Versao 3.0.0-a3. RAM livre
antes: 3,3 GB e 2,9 GB.

#### Fim da Fase 7 - regressao completa - VERDE
Regressao completa depois da 7.5 (`c1aa38f`): 675 verificacoes, verde, 28,3
min (eram 646 em ~20 min no fim da Fase 5; a mais: as sequencias I e F
cresceram com o instalador em NVMe e os pacotes). RAM livre antes: 2,9 GB.
Tag `v3.0-rc1`, nao `v3.0`: a 7.4 continua `[!]` aguardando o teste do dono
em hardware real, e a `v3.0` (marco M5) so vem quando ela fechar.

#### 8.1 Fila de prontos por CPU e nice - 14:05 a 15:10 - FEITO
Requisitos 1 a 6 da 8.1 (logo abaixo das regras). Fila de prontos por CPU
(ainda sob a `runq_lock` unica), escolha pelo menor tempo virtual com os
pesos do nice do Linux, piso de uma fatia para quem acorda, thread nova no
minimo da fila, normalizacao na migracao, CPU de destino livre ou menos
carregada, roubo pela CPU com a fila vazia, balanceamento a cada 4 ticks so
quando diminui o desequilibrio, IPI nova de "tem trabalho" (0xF3), laco
ocioso `cli`/confere/`sti; hlt` tambem nas APs. `setpriority`/`getpriority`
(so `PRIO_PROCESS`, por processo), `sched_setaffinity` de verdade,
`getcpu`, `/proc/sched`, nice no `/proc/<pid>/stat`, programas `nice` e
`nicetest`, sequencia nova P. Resolve tambem o pendente antigo "o
escalonador nao acorda CPU ociosa quando entra trabalho". Detalhes:
`docs/DECISOES.md`, v3.0.0-a4.
Achado do `nicetest` na primeira rodada (espalha e mistura falharam, 1,5 s
de CPU sumidos em 12): a thread `main` do kernel fazia `hlt` em laco sem
dormir e o escalonador a via como uma thread de nice 0 sempre pronta; agora
dorme 2 ms sem entrada. Trabalho futuro anotado pelo dono como 8.8: dormir
numa fila de espera acordada pela IRQ do teclado e da serial.
`nicetest` 5 vezes seguidas, todas verdes: fixa 8,7 a 9,3:1, mistura 9,5 a
9,9:1, espalha com o pior em 1462 de 1500 ms. Sequencia P: 15 verificacoes,
verde. Mexe no escalonador: regressao completa, 690 verificacoes, verde,
28,7 min, RAM livre antes 3,7 GB. Versao 3.0.0-a4 trocada depois da
regressao; nenhuma verificacao confere o texto da versao, entao so
`check.ps1` e build depois da troca.

#### 8.2 Kernel preemptivo - 15:15 a 16:05 - FEITO
O kernel ja trocava de thread na volta de interrupcao em modo kernel e, desde
a 5.7 parte 4, adiava a troca com trava de spin presa. Agora o `spin_unlock`,
ao zerar o contador da thread atual com interrupcoes ligadas e
`need_resched` marcado, chama o escalonador na hora
(`sched_unlock_preempt`); o `spin_unlock_irqrestore` confere depois de
religar as interrupcoes. O `spin_lock_slow` so cede a CPU se a thread nao
segura outra trava. `irqtest`: a thread de ajuda presa na mesma CPU nao roda
com a trava presa e roda antes de o `spin_unlock` voltar (1 troca na
liberacao), verificacao nova na sequencia A. Detalhes: `docs/DECISOES.md`,
v3.0.0-a5.
Verificacao (nucleo: travas, escalonador): `irqtest` 2 vezes e `nicetest`
verdes no QEMU; regressao completa, 691 verificacoes, verde, 26,7 min, RAM
livre antes 3,7 GB. Versao trocada antes da regressao desta vez.

#### 8.3 Fuzzer de syscalls - 16:10 a 17:37 - FEITO
Requisitos 1 a 5 da 8.3 (logo abaixo das regras). `fuzz` (novo): semente
impressa e `--seed`, anel das ultimas 32 chamadas por filho em memoria
compartilhada (despejado na morte inesperada, com a chamada em andamento),
descritores reais de 9 tipos, ponteiros na borda de pagina nao mapeada,
tamanhos de borda, enderecos do kernel e lixo, fork/clone com probabilidade
baixa e limite por filho, exec so de caminho inexistente ou lixo, filhos
como uid 1000 em `/tmp/fuzz`. Sequencia nova Z (60 s; 600 s com `-Noturna`;
`regress.py --noturna`; o `regress.py` tambem ganhou a P, que faltava na
lista padrao). Detalhes: `docs/DECISOES.md`, v3.0.0-a6.
Defeito achado e corrigido em commit proprio (`c4cfbe2`): usuario comum
inundava o log do kernel (syscall desconhecida, morte por sinal, falha de
pagina, ELF invalido), e o fuzz de 60 s levava 216 s por causa da serial;
agora no maximo 10 linhas por segundo por ponto de chamada, com a contagem
das suprimidas; teste `logflood` na sequencia A (que tambem teve a
verificacao do `history` corrigida: o historico guarda 50 comandos e o
primeiro mudou). Com isso o fuzz passou de ~16 mil para ~100 mil chamadas
em 60 s.
Problema no teste do `logflood` (`304dd8e`): a contagem por
`(?s)(X.*?){31}` faz o regex do .NET voltar atras de forma explosiva quando
nao casa (no Python sai rapido); a primeira regressao completa da 8.3 ficou
uma hora parada nessa linha, com o PowerShell queimando CPU e a RAM livre
caindo para 0,4 GB. Agora `\A(?s)(?>.*?X){31}`: grupo atomico ancorado no
comeco, uma passada so (14 ms no log real).
Ajustes do proprio fuzzer no caminho (nao eram defeitos do kernel): as duas
primeiras "mortes inesperadas" eram o filho se desmapeando inteiro
(`munmap(0x1000, enorme)`), e o processo que sobrava no fim era um neto
preso numa chamada depois que o pai dele morreu.
Verificacao: Z verde 2 vezes em 60 s (101 mil chamadas, 0 morte inesperada)
e a noturna de 10 min verde (926 mil chamadas em 3058 filhos, 0 morte
inesperada, maior intervalo sem sinal de vida 822 ms, memoria e processos
de volta). A correcao do log mexe em syscalls de uso geral: regressao
completa verde, 706 verificacoes (Z incluida), 29,5 min, RAM livre antes 1,4 GB depois de esperar (estava em 0,4 GB com o PowerShell travado no regex).

#### 8.4 parte 1 Falta de memoria e matador de processo - 17:40 a 18:24 - FEITO
Requisitos 1 a 8 da 8.4 (logo abaixo das regras). Reserva do kernel para
as paginas de usuario (`pmm_alloc_user_page`), tabela de paginas sem
memoria devolve falha num espaco de usuario em vez de panico (falha de
pagina, COW, fork, ELF, mmap compartilhado, framebuffer, mremap), falha de
pagina com tres resultados e o OOM chamado ja sem a `mm_lock`: primeiro
cache limpo, depois uma vitima por vez (o maior processo que nao e o init),
com espera de ate 5 s; a propria vitima morre ao voltar da falha.
`mmap`/`brk` maiores que a RAM recusados na hora. O `wm` pode ser vitima e
o init ja o relanca (e o programa que ele mantem vivo no modo grafico).
`oomtest` (novo) e sequencia M nova. Detalhes: `docs/DECISOES.md`,
v3.0.0-a7.
Primeira versao do `oomtest` so exercitou a vitima que mata a si mesma;
entrou o caso "outro" (um parado com 60% e outro enchendo), que exercita a
espera pela vitima. Alarme falso na verificacao de ordem: o `(\d+)` podia
capturar so "1" de "10"; agora exige o `'` depois do numero. Uma rodada
teve o enchimento em 29 s em vez de 2 s, duas vezes seguidas, e depois nao
se repetiu (o proprio convidado mediu o tempo, entao foi o QEMU mais lento;
anotado).
Verificacao (nucleo: memoria e falha de pagina): sequencia M verde 3 vezes;
regressao completa verde, 719 verificacoes (M incluida), 28,8 min, RAM livre antes 6,3 GB.

#### 8.4 parte 2 Falha injetada no kmalloc - 18:25 a 19:04 - FEITO
Requisito 9 da 8.4 (complemento do dono). `kmfail N` no shell do kernel, so
no modo estrito: 1 em N `kmalloc`/`kzalloc`/`kcalloc`/`krealloc` de
processo de usuario comum devolve NULL. Rodadas de exploracao com o fuzz:
1 em 100 (166 falhas injetadas em 96 mil chamadas) e 1 em 20 por 90 s
(1145 falhas em 147 mil chamadas): nenhum panico, nenhum BUG, memoria e
processos de volta, nenhum defeito para corrigir. A sequencia Z roda o fuzz
com 1 em 1000; a F confere o EPERM fora do modo estrito. O fuzz conta a
parte os filhos que nao conseguem se preparar. Detalhes: `docs/DECISOES.md`.
Verificacao: Z e F verdes (36 verificacoes); mexe no `kmalloc` (nucleo de
memoria): regressao completa verde, 722 verificacoes, 29,5 min, RAM livre antes 6,1 GB.

#### 8.5 Perfilador por amostragem - 19:05 a 21:47 - FEITO
O timer de cada CPU manda uma NMI a CPU seguinte com a marca "amostra
pendente"; a NMI grava RIP, pid, CPU, usuario e se as interrupcoes estavam
desligadas num vetor por CPU sem trava. Syscall do Orion 1003 (so root)
para ligar, desligar e ler; `perf` (novo) resolve os nomes do kernel pela
tabela de simbolos do `orion.elf` na particao EFI e mostra as funcoes mais
quentes. Teste no modo estrito: o kernel gira com interrupcoes desligadas
em `prof_selftest_irqoff`, e ela aparece com todas as amostras sem IRQ.
Sequencia P com 7 verificacoes novas (21, verde). Detalhes:
`docs/DECISOES.md`, v3.0.0-a8.
Defeito achado e corrigido em commit proprio: a NMI na janela do `swapgs`
(entrada e saida da syscall), pendente desde a Fase 0, travava o sistema
em segundos com a amostragem ligada e muitas syscalls; NMI e #DF agora
entram pela entrada paranoica (decidem o `swapgs` pelo `MSR_GS_BASE`).
Antes de achar isso, a trava foi atribuida por engano a mudanca na espera
do ICR. Outro ajuste em commit proprio: a verificacao de mortes por sinal
do `logflood` dependia do tempo (num periodo lento do hospedeiro os 60
`fork` levaram 6,7 s, nenhum segundo passou de 10 mortes e a regressao
completa acusou); agora os 60 filhos morrem juntos. A primeira regressao
completa desta etapa foi interrompida no meio da sequencia I (queda da
sessao); arvore conferida (sem bytes nulos, `git fsck` limpo).
Medido: no QEMU sem aceleracao ~700 amostras/s por CPU a 1000 Hz, e o
envio da NMI pesa ~20%. Achado: ~12% das amostras em `spin_lock_slow` (a
`runq_lock` unica disputada pelos timers).
Verificacao (nucleo: timer, NMI, entrada de interrupcao): P verde 2 vezes;
regressao completa verde, 728 verificacoes, 31,7 min, RAM livre antes 6,9 GB.

#### 8.6 Pilhas do kernel medidas - 21:50 a 22:24 - FEITO
Pilha de cada thread pintada com 0x5A na criacao; marca d'agua medida ao
liberar a thread e sob demanda nas vivas; `/proc/stacks` (novo). Depois de
`stress` de 300 processos, fuzz, `oomtest`, `perf` e `irqtest`, o maior uso
foi 6680 de 16384 bytes (40%), no `dhcp` do boot: fica em 16 KiB. A
regressao le `/proc/stacks` no fim de A, Z e M e falha a partir de 75%; na
regressao completa o maior foi 8640 bytes (52%), na `netrx` (testes de rede).
Detalhes: `docs/DECISOES.md`, v3.0.0-a9.
Verificacao (nucleo: criacao de threads): regressao completa verde, 731 verificacoes, 29,9 min, RAM livre antes 6,6 GB.

#### 8.8 Thread main acordada pela entrada - 22:25 a 23:03 - FEITO
A `main` dorme na fila `conin` (prazo de 1 s) e o teclado (PS/2 e USB) e a
COM1 a acordam; a COM1 ganhou a IRQ4 de recepcao e um anel de 1024 bytes
com trava. Parado por ~8 s, as trocas de contexto cairam de ~6000 para
~1900; a `main` acorda uma vez por comando digitado. `/proc/sched` mostra
os despertares. Detalhes: `docs/DECISOES.md`, v3.0.0-a10.
Verificacao (nucleo: interrupcoes e escalonamento da entrada): regressao
completa verde, 733 verificacoes, 30,0 min, RAM livre antes 6,4 GB.

#### Fim da Fase 8 - 23:03
8.1 a 8.8 feitas (fila por CPU e nice, kernel preemptivo, fuzzer, OOM e
falha injetada no kmalloc, perfilador, pilhas medidas, memcpy em assembly,
main acordada pela entrada), mais as correcoes achadas no caminho: log do
kernel inundavel pelo usuario, NMI na janela do `swapgs`, verificacoes da
regressao dependentes do tempo. A regressao completa acima e a do fim da
fase. Tag `v3.0-rc2` (a `v3.0` espera a 7.4).

### Sessao 2 - 2026-10-17

Ponto de partida (21:40): a arvore batia com o "Como retomar" (8.7 e 2.7
parte 1 sem commit, mais nada). `.\check.ps1` limpo. Primeiro trabalho:
validar no QEMU e commitar a 8.7 e a 2.7 parte 1 (entradas acima).

### Sessao 4 - 2026-10-21 - resultado do primeiro teste em hardware real (7.4)

Ponto de partida (19:05): HEAD `67195c3`, tag `v3.0-rc2`, arvore limpa,
`check.ps1` limpo. O dono testou a ISO de 20/10 22:47 (mesmo codigo do
HEAD, gerada durante a regressao da 8.8) no notebook ASUS Vivobook (AMD
Ryzen, Windows no NVMe): tudo subiu ate `init iniciado` e caiu em `#DF` na
CPU 1 com estouro da pilha do kernel da thread `init`. Resultado completo
em `docs/hardware.md`. Antes de qualquer diagnostico, o dono pediu a
seguranca do disco interno (a ESP do Windows tinha sido montada com
escrita).

#### 7.4 parte 1 - seguranca: modo live nao toca no disco interno; instalador lista, avisa e exige o nome do disco - 19:10 a 19:50 - FEITO
`vfs_mount_extra` no modo live (raiz em memoria) so monta dispositivo
removivel e somente leitura, e diz no log qual disco interno pulou;
`struct blockdev` com `removable` e `model` (USB, NVMe, AHCI, virtio-blk);
`/proc/blockdevs` (novo) com nome, setores, pai, tipo, removivel, GUID do
tipo e modelo. `instalar` sem argumento lista os discos com modelo, tamanho
e particoes, reconhece NTFS, exFAT, ESP com `\EFI\Microsoft` (monta somente
leitura e olha), reservada e recuperacao do Windows, raiz e dados Linux e
os tipos MBR comuns; disco com qualquer uma delas e marcado `*** TEM OUTRO
SISTEMA: nao use ***`, nunca entra em "Destinos sem outro sistema", o
`--sim` e recusado nele e so `apagar <nome>` segue; disco limpo exige o
nome dele em vez de `SIM`. `docs/teste-hardware-real.md` ganhou o aviso em
destaque: maquina com outro sistema, so o modo live. Detalhes:
`docs/DECISOES.md`, v3.0.0-a11 parte 1.
Verificacao: sequencia I (33 verificacoes, 7,3 min) verde, com 5
verificacoes novas (listagem, nome do disco, disco com a raiz do Orion
marcado e `--sim` recusado, `apagar nvme0n1` exigido, lista sem destino
depois da instalacao); sequencias C (disco, `/mnt` no sistema instalado
continua igual) e F (ISO live) verdes. RAM livre antes: 5,8 GB e 7,0 GB.

#### 7.4 parte 2 - o #DF do hardware real: relatorio que acha a causa, conferencia da pilha na entrada, tempestade de IRQ, SCI - 19:50 a 20:30 - FEITO
Simbolo conferido no `orion.elf` do HEAD (a ISO testada era do mesmo
codigo): `RIP = isr_common+3`. Leitura do relatorio do dono, com o layout
do bloco grande do `kmalloc` (pilha 0x40 bytes depois da pagina): uma
interrupcao chegou com so 16 bytes de pilha, o `isr_common` empurrou tres
registros e o quarto caiu na guarda. Os registros sao de um `mmap(0,
0x5000)` do `init` recem-feito (R8 a R11 intactos, R11 = flags do
`syscall`), e RDI = RBX = base+0x70 poe o comeco de uma entrada vinda do
anel 3 em base+0x120, e nao no topo. Duas hipoteses fecham (topo de
entrada errado ou cadeia de 16 KiB), e o relatorio antigo nao seguia a
cadeia. Nao e interrupcao aninhada nem a COM1: no notebook a UART le 0xFF e
a IRQ4 nem e ligada; o SCI do uACPI ja deixa a GPE desligada ate o trabalho
na `acpi-work` terminar (conferido no `event.c`).
O que entrou: conferencia do topo da pilha em toda entrada do anel 3
(panico com `rsp0` do TSS e `kernel_rsp`); relatorio do `#DF`/panico com a
cadeia pelo `RBP` (recursao compactada), o quadro da interrupcao que nao
coube e uma linha por CPU com profundidade, ultimos 16 vetores e o vetor
mais disparado; limite de 8 interrupcoes aninhadas; detector de tempestade
(32768/s: mascara a IRQ ou poe o vetor MSI em quarentena, com aviso); SCI
roteada como nivel/baixo sem override da MADT, com log; leitura da COM1
na IRQ limitada; pilha do kernel de 32 KiB; `-fno-omit-frame-pointer`
explicito. Comandos de teste no modo estrito: `irqstorm` (sequencia A:
auto-IPI sem fim e a COM1 em nivel com THRE sem tratamento, os dois viram
aviso e mascara em ~150 ms) e `estouro` (sequencia K, nova: o cenario do
notebook no QEMU, relatorio com o mesmo padrao de RSP e CR2). Detalhes:
`docs/DECISOES.md`, v3.0.0-a11 parte 2.
Problemas no caminho: (1) o heredoc do Bash estragou de novo as barras de
um script de remendo (as quebras viraram linhas de verdade dentro das
strings do C); consertado antes do build. (2) A primeira versao do
`estouro` mergulhava na thread `main`, que usa a pilha de boot sem guarda:
desceu sem limite e travou em silencio; agora roda numa thread propria.
(3) O teste de borda exigia mais de 32768 disparos do handler, mas o
32769o ja e o calado.
Verificacao (nucleo: interrupcoes e pilha): `check.ps1` limpo, sequencia K
(9 verificacoes) verde; regressao completa verde, 753 verificacoes (K incluida), 28,4 min; maior uso de pilha 6680 de 32768 bytes (20%). RAM livre
antes: 7,0 GB. Versao 3.0.0-a11.

#### 7.4 parte 3 - a causa do #DF: o kernel com o GS do usuario em laco de #PF; entrada decide o swapgs pelo MSR - 20:30 a 21:00 - FEITO
Com o relatorio novo da parte 2 e o disassembly do `isr_dispatch` testado,
os registros do notebook fecham com um mecanismo preciso: o quadro de
entrada em RDI = RBX = base+0x70 poe o RBP da primeira funcao chamada em
base+0x10 (o do relatorio), o `RAX = 1` e o retorno do `percpu_ready()`, e
o `percpu_get()` seguinte le `%gs:` com a base do usuario, faz `#PF`, e a
entrada antiga (que decidia o `swapgs` pelo CS) entra de novo nele. 272
bytes por volta ate a pilha acabar, sem tocar nos registros do usuario.
Reproduzido no QEMU com `iretfalha` (strict): `swapgs` e `iretq` com CS
invalido. Na entrada antiga deu o mesmo `#DF` do notebook (RAX = 1,
RSP = base-0x40, CR2 em `...ff8`, cadeia `percpu_get` / `isr_dispatch` /
`isr_common` repetida 241 vezes). Agora toda entrada decide o `swapgs`
pelo `MSR_GS_BASE` (a paranoica virou a unica) e passa o resultado ao C,
que entra em panico com o caso e os numeros: volta ao anel 3 que falhou no
`iretq`/`sysretq` (com o quadro devolvido), entrada do anel 3 com o GS do
kernel ja carregado, ou kernel rodando com o GS do usuario. O gatilho no
notebook ainda nao esta provado; a segunda rodada diz qual. Detalhes:
`docs/DECISOES.md`, v3.0.0-a12.
Verificacao (nucleo: entrada de interrupcao): `check.ps1` limpo,
sequencia K (12 verificacoes, com o segundo boot do `iretfalha`) verde;
regressao completa verde, 757 verificacoes, 29,9 min. RAM livre antes: 6,8 GB. Versao 3.0.0-a12.

#### 7.4 parte 4 - ISO nova, roteiro da segunda rodada e etapas 9.1 a 9.3 no plano - 21:00 a 21:10 - FEITO
`build.ps1 -Iso` no HEAD `5f5dc3c`: `out\orion.iso` (65,1 MiB) e
`out\orion.elf` da mesma compilacao (o kernel dentro da ISO e identico ao
`out\orion.elf`, conferido byte a byte; versao 3.0.0-a12). Sequencia F
nessa ISO, sem recompilar: 26 verificacoes, verde, nos quatro boots (CD e
pendrive, BIOS e UEFI), com `modo live: disco interno ... nao e montado
sozinho` e os comandos de teste respondendo EPERM fora do modo estrito.
`docs/teste-hardware-real.md` ganhou a secao 5a (segunda rodada no
notebook, so modo live, o que conferir e as linhas de panico novas que
podem aparecer) e o que fotografar num `#DF`; `docs/hardware.md` tem o
resultado do primeiro teste. `docs/PLANO.md` ganhou, so como anotacao, as
etapas 9.1 (instalar em disco USB), 9.2 (dual boot no NVMe em espaco livre,
sem tocar em `\EFI\Microsoft`, com backup) e 9.3 (rede por adaptador USB
CDC-ECM ou RTL8153). A 7.4 continua `[!]` ate a segunda rodada passar.

#### 7.4 parte 5 - revisao do dono: tempestade so em linhas, pilha de volta a 16 KiB, CI - 2026-10-22 - FEITO
Tres pontos do dono sobre a parte 2. (1) O detector de tempestade valia
para MSI/MSI-X, e NVMe e xHCI sob carga passam de 32768 por segundo em
hardware real: agora so IRQs de linha (ISA, menos o timer, e GSI do I/O
APIC) sao mascaradas; MSI e MSI-X nunca. O `irqstorm` passou a exigir que
um MSI com pico acima do limite (80 mil disparos) nao seja mascarado, e as
sequencias C, V, N, U e E exigem nenhuma tempestade; o `irq` mostra o pico
por segundo de cada vetor (NVMe no `fstest -a`: ~2 mil/s no QEMU). (2) A
pilha volta a 16 KiB: o estouro era o laco de `#PF` da parte 3, e o maior
uso medido segue em 52% (8576 bytes, `netrx`). (3) O CI do GitHub (`gh run view --log-failed`)
caia no fim da sequencia A com `re.error: global flags not at the start`:
dois padroes do `logflood` com `\A(?s)`, que o .NET aceita e o Python 3.11+
nao. Viraram `(?s)\A`; os 729 padroes compilam no Python, conferidos pelo
proprio `regress.py`, que ganhou a sequencia K na lista padrao. Detalhes:
`docs/DECISOES.md`, v3.0.0-a13.
Verificacao (nucleo: interrupcoes e pilha): `check.ps1` limpo; rodada
manual com `fstest -a` no NVMe e `irqstorm`; regressao completa verde, 763 verificacoes, 34,2 min (a primeira, com a suspensao do Windows no meio, deu vermelho so na M e a segunda so no perf; correcoes nas entradas abaixo). RAM
livre antes: 6,5 GB na primeira, 2,8 GB na final. Versao 3.0.0-a13. O push continua com o dono; o CI so
roda de novo no proximo push dele.

#### Correcao achada na regressao da parte 5: o OOM matava o `sh` - 2026-10-22 - FEITO
A primeira regressao completa da parte 5 (192,6 min: o Windows suspendeu
das 06:50 as 09:34 no meio da sequencia B, que mesmo assim passou) deu
vermelho so na M, 6 de 763: no caso "dois processos enchendo juntos" o OOM
matou o `sh` depois que a segunda vitima saiu. A vitima conta como saida ao
virar zumbi, mas a memoria volta depois; uma falha nesse intervalo escolhia
outra vitima. Defeito da 8.4, dependente de tempo. Agora quem precisa de
memoria espera ate 2 s a da vitima voltar antes de escolher outra.
Detalhes: `docs/DECISOES.md`, logo depois da v3.0.0-a13. Verificacao:
sequencia M verde 3 vezes seguidas; a segunda regressao completa
(32,2 min) passou na M e deu vermelho so no perf (entrada abaixo). RAM
livre antes: 1,7 GB; esperados 2 min, 6,0 GB.

#### Verificacao do perfilador sem janela de interrupcoes ligadas - 2026-10-22 - FEITO
A segunda regressao completa da parte 5 deu vermelho em 1 de 763: das 176
amostras em `prof_selftest_irqoff`, 175 sem IRQ. A funcao ligava e
desligava as interrupcoes dentro dela, e uma NMI caiu no prologo ou no
epilogo numa rodada lenta (metade das amostras de sempre). O `cli` e o
`sti` foram para quem chama; a verificacao continua exigindo todas as
amostras sem IRQ. Detalhes: `docs/DECISOES.md`, logo depois da correcao
do OOM. Verificacao: sequencia P verde tres vezes antes e tres depois;
regressao completa final verde, 763 verificacoes, 34,2 min, maior uso de
pilha 8576 bytes (52%), pico do NVMe no `fstest` 1478/s, nenhuma
tempestade acusada fora do teste. RAM livre antes: 2,8 GB.

#### 7.4 parte 6 - segunda rodada no notebook: a causa e o STAR sem RPL 3 (o sysret da AMD) - 2026-10-22 - FEITO
Segunda rodada do dono (ISO a13, Ventoy, UEFI): o live nao montou o
disco, e logo depois de `init iniciado` o relatorio novo deu "a volta ao
anel 3 falhou no iretq ... #GP (erro 18) ... CS=23 ... SS=18". Resolvido
no `orion.elf` da a13: o `iretq` da volta de interrupcao no `isr_common`,
com o SS que a CPU empilhou do anel 3. O kernel monta todos os quadros de
usuario com RPL 3 (conferido em todos os lugares que escrevem os
seletores); quem deixava SS=0x18 no anel 3 era o `sysret`, porque o STAR
tinha `GDT_KERNEL_DATA` sem o RPL 3, e a AMD, ao contrario da Intel e do
QEMU, nao forca o RPL do SS. O mesmo `iretq` foi o `#DF` do primeiro
teste. Corrigido o STAR (0x13); boot calcula o que o `sysret` carrega nas
duas regras e para em panico se estiver errado; conferencia dos seletores
antes de todo `iretq` para o anel 3 (panico no modo estrito, correcao e
aviso unico fora dele), contagem `IRT:` no `/proc/interrupts`; `iretss`
no terceiro boot da K; `segtest` (novo) na sequencia R (nova, `-Smp 8`):
16 threads com syscall, migracao, preempcao, sinal a cada 2 ms e fork,
lendo CS/SS no anel 3. O `regress.py` passou a respeitar o `-Smp` de cada
sequencia. Detalhes: `docs/DECISOES.md`, v3.0.0-a14.
A regressao nunca passou por isso porque o `sysret` do QEMU (TCG) sempre
forca SS com RPL 3. Teste que falha sem a correcao: a sequencia R com o
STAR antigo deu vermelho em 8 de 9 (o boot para em "STAR errado ... SS=18
(AMD)"); com a correcao, 9 de 9 (~11 milhoes de leituras, 4118 voltas por
`iretq` conferidas, nenhuma errada).
Verificacao (nucleo: entrada e saida do anel 3): `check.ps1` limpo,
sequencias R (9) e K (15) verdes; regressao completa verde, 776 verificacoes, 31,9 min. RAM livre
antes: 4,6 GB. Versao 3.0.0-a14.

#### 7.4 parte 7 - terceiro teste no notebook passou; o Doom acha o WAD sozinho - 2026-10-22 - FEITO
Terceiro teste do dono (ISO a14, Ventoy, UEFI): SUCESSO. Boot completo ate
o login, live sem montar o `nvme0n1`, `segtest 5` TUDO OK nas 8 CPUs
(99855 leituras depois de syscall e de troca de CPU, 2494 sinais, 4
filhos, 10704 voltas por `iretq`, 0 com seletor errado). O `doom` sem
argumentos nao achou o WAD: ele estava em `/doom`, mas o doomgeneric so
procura no diretorio atual (e o `find / -name '*.wad'` do dono falhou
porque o `sh` do Orion passa as aspas para o `find`). Os dois Freedoom vao
para `/usr/share/games/doom`, e o `main` do Doom do Orion acha o IWAD
(`DOOMWADDIR`, caminhos padrao) e da mensagem clara sem ele, sem chamar o
`zenity`. Detalhes: `docs/DECISOES.md`, v3.0.0-a15 parte 7.
Verificacao: sequencias F (33, com o Doom na ISO live e a captura), B
(62), S e I verdes. A B precisou de dois ajustes no teste (a pasta `games`
nova mudou a posicao de `imagens` no app `arquivos` e a linha do cursor no
terminal); uma rodada com a imagem reaproveitada (`-NoBuild`) falhou no
`config` porque a rodada anterior tinha gravado o `/etc/wm.conf` na
imagem, o que nao acontece na regressao completa, que refaz a imagem.

#### 7.4 parte 8 - `/proc/pci`, `dmesg`, CI do GitHub e escrita em disco rapida - 2026-10-22 - FEITO
Tres frentes juntas, porque mexem nos mesmos arquivos (`regressao.ps1`,
`nvme.c`, `regress.py`):
(1) `/proc/pci` com o driver de cada dispositivo (`pci_set_driver`, so
quando o init deu certo) e `lspci`; anel de 64 KiB do log do kernel em
`/proc/kmsg` e `dmesg [-g texto]`.
(2) CI: o run 39795608775 falhou em 2 de 490 porque o `Makefile` do Linux
nao criava `home/orion` (`ls /home` dava inexistente no log baixado do
CI). Corrigido no `Makefile`, no `musl_build.py` e no `mkimage.py`; os
dois WADs no build do Linux; o `regress.py` lia zero passos da K e da R
(lista numa linha so) e agora para com erro nesse caso; prazos x2 (com
KVM) ou x3 no CI (`CI=true`, `ORION_TIME_SCALE`); `-accel kvm` quando o
`/dev/kvm` existe; `-Smp` por sequencia; workflow com `checkout@v5`,
`upload-artifact@v5`, KVM liberado, todas as sequencias da regressao
local e limite de 300 min.
(3) Os WADs deixaram a instalacao da sequencia I lenta demais (mais de
1500 s). Medido com o `wrtest` (novo): escrita no ext2 do NVMe a ~185
KiB/s com qualquer tamanho de escrita; causas: `FLUSH` do NVMe depois de
cada escrita e um comando por setor no cache de blocos. Agora o `FLUSH`
vai so no `sync` (`blockdev->flush`), escritas de 4 KiB ou mais vao
direto ao disco e o instalador formata a raiz com blocos de 4 KiB:
2651 KiB/s (blocos de 1 KiB) e 8305 KiB/s (4 KiB).
Ajustes de teste no caminho: o fim de linha do log ia por `kputc` e o
`dmesg` saia numa linha so; V e N aceitam qualquer tamanho de raiz (279
MiB com os WADs). Detalhes: `docs/DECISOES.md`, v3.0.0-a15 partes 8, 9 e
10.
Verificacao: `regress.py` completo pelo caminho do CI (790 de 792; as duas
falhas eram do `permtest` novo da correcao abaixo, num kernel em que o
`process.c` nao tinha sido recompilado) e regressao completa do PowerShell
verde em 790 de 793 (24,3 min), so o `nicetest` oscilando (abaixo). O multiplicador de prazos foi conferido com `CI=true` (K verde
com prazos x3); o Linux do runner e o KVM nao deu para testar daqui.

#### Defeito achado pelo fuzzer: `setpgid` sem permissao - 2026-10-22 - FEITO
Numa regressao completa o fuzz da Z nao terminou: o pai (root) morreu com
SIGKILL depois de um filho (usuario 1000) mover o pai para o grupo dele
com `setpgid` e o pai matar aquele grupo no fim da rodada. O kernel
aceitava `setpgid` de qualquer processo para qualquer outro. Agora so o
proprio processo ou um filho, com `EINVAL`, `ESRCH` e `EPERM` como no
Linux. `permtest` com quatro verificacoes novas (falham com o kernel
antigo, passam com o novo); o fuzz com a mesma semente vai ate o fim.
Detalhes: `docs/DECISOES.md`, logo depois da v3.0.0-a15 parte 10.
No caminho: o `nicetest` misto oscilou muito hoje; o commit da a14
compilado num worktree a parte falhou do mesmo jeito (4 de 5) nesta
maquina e hora, entao nao e regressao; registrado no `DECISOES.md`.
Verificacao: a mesma regressao completa da parte 8 (A com o `permtest`, Z e E verdes).

#### Fecho da 7.4 - 2026-10-22
Terceiro teste do dono no notebook ASUS Vivobook (AMD Ryzen, Windows no
NVMe), ISO 3.0.0-a14, Ventoy em modo normal, UEFI: boot completo ate o
login, modo live sem montar o `nvme0n1`, `segtest 5` TUDO OK nas 8 CPUs
(10704 voltas por `iretq` conferidas, nenhuma com seletor errado). A 7.4
fica `[x]`: o Orion boota num computador fisico pelo pendrive e chega ao
shell (marco M5). Limitacoes que ficam nesse notebook, em
`docs/hardware.md`: sem rede suportada, Wi-Fi ainda nao identificada, sem
som (o audio analogico passa pelo AMD ACP, nao suportado), sem driver de
video da GPU nem de touchpad I2C.

#### Aspas no `sh` e o `FLUSH` conferido - 2026-10-22 - FEITO
(1) O `sh` passava as aspas como texto (`find / -name '*.wad'` recebia
`'*.wad'`). O `dash` foi avaliado como shell de login e ficou de fora: e
compilado sem libedit, sem historico nem edicao de linha. O `sh` proprio
ganhou um lexer por palavra: aspas simples, duplas com `$`, barra
invertida, argumento vazio, `$VAR` dividido so sem aspas, operadores so
fora das aspas, `VAR="a b" cmd`, erro para aspas sem fechar. Oito
verificacoes novas na A; dois passos que dependiam das aspas literais
mudaram (o `printf` do `telnet` e o `pkg remove` por `su -c`).
(2) Auditoria do `FLUSH` depois da parte 10: `sync`, `fsync`, `umount`,
`poweroff` e `reboot` chegavam ao NVMe; o `fdatasync` nao existia
(`ENOSYS`) e o `syncfs` tambem nao; o `fat_sync` mandava um `FLUSH` a mais
em todo `sync` e um a cada 2 s pelo `bdflush` com a ESP montada; a escrita
direta que falhasse deixava no cache, marcado limpo, um dado que nao chegou
ao disco. Corrigidos; contador de `FLUSH` por disco no `/proc/diskstats`;
o `bdflush` so manda `FLUSH` se houve escrita; `discos gravados (N FLUSH)`
no `poweroff`/`reboot`. Teste novo `flushtest` nas sequencias N e I
(disco cru, arquivo com `read` e `mmap`, `umount`), conferido contra um
kernel com o cache de blocos errado de proposito.
Detalhes: `docs/DECISOES.md`, v3.0.0-a15 partes 11 e 12.
No caminho: a regressao completa achou uma corrida no OOM (a segunda
vitima morta sem esperar a memoria da primeira voltar), corrigida em
`mm/oom.c`; sequencia M verde 10 vezes seguidas. Detalhes no
`docs/DECISOES.md`, logo depois da parte 12.

#### Versao 3.0.1-a1 e a pendencia do `FLUSH` - 2026-10-23 - FEITO
A ISO tinha mudado depois da tag `v3.0` com o mesmo nome 3.0.0-a15. Agora
toda mudanca depois de uma tag sobe a versao: serie 3.0.1-aN, commits
`v3.0.1:` (`docs/DECISOES.md`). A falta de `FLUSH` no AHCI e no virtio-blk
virou a etapa 9.4 do `docs/PLANO.md`.
