# Decisoes de arquitetura do Orion

Uma linha por decisao, na ordem em que foram tomadas.

## v0.9 (base)

- A libc de usuario antiga (`user/lib/ulib.c`/`ulib.h`) foi removida: os mesmos
  simbolos (`read`, `write`, `open`, ...) ja existiam em `unistd.c`/`stdio.c` e o
  link de qualquer programa falhava com "duplicate symbol".

## v1.0 (interface grafica)

- `test.ps1` roda o QEMU sem janela (`-display none`) e expoe serial e monitor em
  TCP (4444/4445); e o unico jeito de testar a GUI sem interagir com a janela.
- O mouse PS/2 entrega `dy` ja em orientacao de tela (positivo = para baixo), para
  que o compositor nao precise inverter nada.
- O mouse tenta o modo IntelliMouse (sequencia de sample rate 200/100/80 + `0xF2`);
  se o dispositivo responder id 3, os pacotes passam a ter 4 bytes com a roda.
- Resync do mouse: se o bit 3 do primeiro byte do pacote nao estiver setado, o byte
  e descartado em vez de deslocar o pacote inteiro (contador em `mouse_resync_count`).
- `/dev` nao e um sistema de arquivos montado: `fs/devfs.c` mantem uma tabela fixa
  de dispositivos e o VFS desvia para ela quando o caminho comeca com `/dev`
  (`vfs_lookup`, `vfs_readdir` e `file_open_path`).
- `struct file_ops` ganhou `ioctl` e `mmap`; `SYS_IOCTL` passou a receber tres
  argumentos (fd, request, arg) e `SYS_MMAP` aceita fd >= 0 delegando ao driver.
- Paginas mapeadas que nao pertencem ao PMM (o framebuffer) sao marcadas com
  `PTE_NOFREE` (bit 9, livre para o SO). `vmm_destroy_address_space`, `munmap` e o
  `fork` respeitam esse bit: nunca liberam a pagina e o fork compartilha o mesmo
  quadro fisico em vez de copia-lo.
- `/dev/input` e exclusivo por contagem de aberturas: enquanto houver um fd aberto,
  as teclas vao para a fila de eventos e nao para o TTY.
- `read` em `/dev/input` e bloqueante por padrao; `ioctl(fd, INPUTIOC_SETTIMEOUT, ms)`
  define um timeout para quem precisa de um laco de eventos com relogio (o `wm`).
- A entrada da serial continua sempre indo para o TTY/shell do kernel, mesmo com o
  teclado capturado: e o que permite depurar o compositor sem ver a tela.
- A disciplina de linha virou `struct line_discipline` (`drivers/ldisc.c`), usada
  pelo TTY fisico e pelos slaves de PTY. Trocou o buffer de uma linha so por um
  anel de 1 KiB: da para digitar varias linhas antes do programa ler.
- O eco da disciplina de linha e feito fora do spinlock (callback), para nao
  aninhar a trava do kprintf com a da disciplina.
- `read` no master de um PTY e nao-bloqueante: devolve -EAGAIN quando vazio e 0
  quando o ultimo slave fechou. E o que permite ao compositor ter um unico laco
  de eventos sem threads.
- `SYS_WAIT4` passou a respeitar `WNOHANG`, para o compositor colher os shells
  que morreram sem travar a interface.
- Correcao: `vmm_map_page_in`/`vmm_unmap_page_in` so faziam `invlpg` quando o
  PML4 alterado era o do CR3 atual. Como a metade alta e compartilhada, mexer no
  heap do kernel rodando dentro de um processo deixava traducoes velhas na TLB
  local (o sintoma era `kfree` achando lixo no cabecalho de uma alocacao grande).
- Correcao: `thread_sleep_ms`, `thread_block` e `thread_exit` soltavam o
  `sched_lock` antes de chamar `schedule()`. Nessa janela outra CPU podia acordar
  a thread, enfileira-la e executa-la antes de ela salvar o contexto. Agora a
  trava e mantida ate a troca de contexto (`schedule_locked`).
- O console de texto entra em "modo grafico" enquanto alguem tem `/dev/fb0`
  mapeado: continua atualizando a grade e escrevendo na serial, mas para de
  desenhar no framebuffer. Ao fechar o ultimo fd (ou num panic) ele redesenha
  tudo. Sem isso as mensagens do proprio `wm` apagavam pedacos da tela.
- O compositor mantem um backbuffer proprio (`mmap` anonimo) e so copia para o
  framebuffer os retangulos sujos; a composicao e feita com um retangulo de
  recorte, entao repintar um pedaco custa proporcional ao pedaco.
- O cursor do mouse nao entra no backbuffer: e desenhado direto no framebuffer
  ao final de cada copia. Mover o mouse suja o retangulo antigo e o novo.
- O terminal grafico marca sujeira por linha da grade, entao digitar custa uma
  faixa de 960x18 pixels em vez da janela inteira.
- Sem sinais no kernel: fechar uma janela de terminal fecha o master do PTY, o
  `sh` recebe fim de arquivo na leitura e sai sozinho. Por isso o `/bin/sh` agora
  encerra em EOF em vez de continuar o laco.

## v1.1 (filas de espera e poll)

- `wait_event(wq, lock, cond)` recebe a trava explicitamente (tres argumentos em
  vez de dois): a condicao so pode ser testada sob a mesma trava que o acordador
  segura, e passar essa trava e a forma mais simples de garantir isso.
- `struct wait_entry` fica na pilha de quem espera e a thread guarda a lista das
  suas entradas (`t->waits`), o que permite esperar em varias filas ao mesmo
  tempo - e a base do `poll`.
- Ordem de travas do sistema: trava de dados -> `runq_lock`. Quem acorda alguem
  segura a trava de dados e chama `wake_up`, que pega a `runq_lock`; quem dorme
  faz o mesmo caminho.
- A colheita de threads mortas saiu de dentro da `runq_lock`: `sched_post_switch`
  destaca a zumbi com a trava e so depois libera o espaco de enderecos e avisa o
  pai. Sem isso, `process_thread_reaped` (que pega `proc_lock`) inverteria a
  ordem contra o `waitpid` (que pega `proc_lock` e depois `runq_lock`).
- `waitpid` espera numa unica fila global de saidas (`child_exit_wq`) em vez de
  uma fila por processo: evita ter que garantir a vida do `struct process` do pai
  no momento em que o filho e colhido, e saidas de processo sao raras.
- O master do PTY passou a bloquear por padrao; quem quer o comportamento antigo
  usa `fcntl(fd, F_SETFL, O_NONBLOCK)`. O `wm` e o `ptytest` fazem isso.
- `SYS_POLL` refaz a varredura a cada volta em vez de manter estado entre elas:
  registra nas filas, avalia, e so dorme se nada estiver pronto. Custa uma
  varredura extra por acordada e dispensa qualquer estrutura persistente.

## v1.2 (sinais)

- O trampolim de `sigreturn` vem da libc: `sigaction()` preenche `sa_restorer`
  com o endereco de uma funcao `naked` que so faz `mov $15,%rax; syscall`, e o
  kernel empilha esse endereco como retorno do handler. E o mesmo arranjo do
  Linux/glibc e evita colocar codigo do kernel no espaco do usuario.
- O quadro de sinal vai na pilha do proprio processo, 128 bytes abaixo do `rsp`
  (a red zone; os programas sao compilados com ela ativa), alinhado para o
  handler ver `rsp % 16 == 8` como depois de um `call`.
- `SA_RESTART` e aceito mas ignorado: chamadas bloqueantes interrompidas por
  sinal devolvem `EINTR` e quem chama decide. O `sh` refaz o `waitpid` nesse caso.
- Status de saida no formato do Linux: `(codigo << 8)` para saida normal e o
  numero do sinal nos 7 bits baixos para morte por sinal. `WIFSIGNALED`/`WTERMSIG`
  passam a funcionar e o `sh` imprime `[morto por sinal N]`.
- O grupo de primeiro plano mora na disciplina de linha (`fg_pgid`), nao no
  processo: e o terminal que sabe para quem mandar `SIGINT`. `TIOCSPGRP` funciona
  igual no TTY fisico e no slave de PTY.
- `alarm()` e conferido por uma thread de kernel (`alarmd`, 100 ms) em vez do
  `sched_tick`: varrer a lista de processos exigiria `proc_lock` dentro da
  `runq_lock`, invertendo a ordem de travas do sistema.
- Correcao: `time_ticks()` passou a ser derivado do TSC. O contador de
  interrupcoes de timer anda ~10x mais devagar que o tempo real neste QEMU
  quando as CPUs ficam ociosas, o que esticava todo `sleep`, `alarm` e timeout
  do sistema na mesma proporcao.
- Correcao: `process_thread_reaped` zerava `pml4_phys` fora do `proc_lock`,
  entao um `waitpid` concorrente podia colher e liberar o mesmo `struct process`
  - dava `kfree` duplo. Agora a limpeza e a decisao de quem libera acontecem
  dentro da trava.

## v1.3 (escrita no FAT e redirecionamentos)

- A FAT inteira ja ficava em RAM desde a v0.9; a escrita mantem esse desenho e so
  marca a faixa de setores suja, descarregada no `sync` e pelo `bdflush` (2 s).
  Escrever a FAT a cada `write()` custava caro demais (era o gargalo do teste de
  3 MiB) e nao trazia garantia real sem journal.
- `struct vfs_node` passou a carregar onde a entrada de diretorio mora
  (`dir_ino` + `dir_offset`), que e o que permite atualizar tamanho e cluster
  depois de escrever sem varrer o diretorio de novo.
- Nome curto 8.3 gerado no estilo do Windows: duas letras, quatro digitos de um
  hash FNV do nome longo e `~N`. Tentar `NOME~1`, `NOME~2`... exigia varrer o
  diretorio inteiro por candidato, o que fazia criar 300 arquivos virar O(n^3).
- O cache de setores fica na camada de blocos (256 entradas, write-back, LRU) e
  nao no FAT: assim o `mkfs` e qualquer leitura crua tambem se beneficiam.
- `bdflush` mora no VFS, nao na camada de blocos, porque precisa descarregar a
  FAT antes dos setores.
- Dispositivos de bloco aparecem em `/dev` (`sda`, `sda1`, `sdb`) com read/write
  por deslocamento, o que permitiu escrever o `mkfs` em espaco de usuario.
- Um segundo sistema de arquivos e montado em `/mnt` (o primeiro FAT que nao for
  o do disco de boot). Um unico ponto de montagem extra cobre o caso de uso sem
  precisar de uma tabela de montagem de verdade.
- O `vvfat` do QEMU (`fat:rw:out/esp`) nao aguenta escrita de verdade: cria
  poucos arquivos e depois aborta o proprio QEMU
  (`check_directory_consistency: assertion failed`) ou falha com
  `Error handling renames (-2)`. Por isso os testes pesados usam `-Disk`, um
  disco cru de 64 MiB formatado pelo `mkfs` dentro do Orion e montado em `/mnt`.

## v1.4 (teclas estendidas e edicao de linha)

- O modo raw e do termios, nao um modo novo: `cfmakeraw` so limpa `ICANON` e
  `ECHO`. Quem quer teclas cruas desliga o cozimento da linha, e a disciplina de
  linha continua sendo a mesma para o TTY e para a PTY.
- O teclado nao ganhou codigos proprios para as teclas estendidas: o driver
  traduz seta, Home, End, Delete, PageUp e PageDown para as sequencias ANSI
  (`ESC [ A`, `ESC [ H`, `ESC [ 3 ~`, ...). Assim o mesmo byte chega pelo teclado
  fisico, pela serial e pela PTY, e um unico parser em espaco de usuario serve
  para os tres.
- A edicao de linha e do `sh`, nao do kernel. A disciplina de linha cozida
  continua simples (backspace e mais nada); historico, cursor e redesenho ficam
  no `edit.c` do `sh`, que e onde da para evoluir sem mexer no kernel.
- O redesenho da linha e sempre `\r`, `ESC [ K` e a linha inteira de novo, em vez
  de calcular o menor conjunto de movimentos. E O(largura) por tecla, o que nao
  se nota em 80 colunas, e elimina uma classe inteira de bugs de cursor.
- O historico guarda 50 linhas, ignora repetidas seguidas e mora so na memoria:
  gravar em disco exigiria um `$HOME` que ainda nao existe.
- O terminal grafico do `wm` ganhou um parser CSI de verdade (cursor, apagamento,
  16 cores, video reverso) porque o `edit` e o historico do `sh` mandam as mesmas
  sequencias para os dois terminais. Sem isso o editor so funcionaria no TTY.
- Cada celula do terminal grafico guarda caractere, cor de frente e cor de fundo
  (3 bytes). Custa 3x a memoria de antes por celula (~23 KiB numa janela de
  80x24) e evita ter que reprocessar as sequencias para redesenhar uma linha.
- O `edit` sai com `Ctrl+X`, nao `Ctrl+Q`: o `wm` reserva `Ctrl+Q` para fechar a
  janela focada, entao dentro do terminal grafico o `Ctrl+Q` nunca chegaria ao
  editor. `Ctrl+X` funciona igual nos dois terminais.
- O `edit` carrega o arquivo inteiro em memoria (512 linhas de 256 colunas, 128
  KiB estaticos) e regrava tudo ao salvar. E um editor de notas, nao de fontes;
  edicao incremental exigiria muito mais do que o passo pede.
- O `test.ps1` ganhou o passo `keys:<hex>` (varios bytes de uma vez) porque uma
  seta e tres bytes e mandar um `key:` por byte deixava 0,6 s entre eles.
- O `edit` so redesenha a tela inteira quando as linhas se deslocam (abrir,
  rolar, quebrar ou juntar linha); nas outras teclas reescreve so a linha do
  cursor e as duas de status. Redesenhar as 24 linhas por tecla dava ~2 KiB por
  tecla, o que numa serial de 115200 baud atrasava o editor em varios segundos.

## v1.5 (fork copy-on-write)

- A contagem de referencias mora no PMM, num vetor de `uint16_t` por pagina
  fisica (~230 KiB para 458 MiB de RAM), e nao numa estrutura por regiao de
  memoria. O kernel ainda nao tem VMA; o vetor plano resolve o problema inteiro
  com uma indirecao e sem alocacao dinamica no caminho da falta.
- O valor guardado e o numero de referencias **extras**: 0 significa dono unico.
  Assim `pmm_alloc_page` nao precisa inicializar nada e o caminho de liberacao
  das paginas que nunca foram compartilhadas continua identico ao de antes.
- `pmm_free_page` decrementa a referencia em vez de liberar quando ela e maior
  que zero. Todo mundo que ja liberava paginas (destruir espaco de enderecos,
  `munmap`, `exec`) passou a funcionar com COW sem mudar uma linha.
- O bit 10 da PTE marca copy-on-write. Na `fork`, a pagina gravavel perde o
  `PTE_WRITABLE` e ganha o `PTE_COW` **nos dois** espacos de enderecos; pagina
  so de leitura e compartilhada sem marcar COW, para que uma escrita nela
  continue virando SIGSEGV.
- Depois de remarcar as PTEs do pai a `fork` recarrega o CR3 inteiro em vez de
  fazer `invlpg` pagina a pagina: sao centenas de paginas, e um processo tem uma
  thread so, entao nao ha outra CPU com esse espaco de enderecos carregado.
- A falta de COW e tratada em `handle_exception`, antes do caminho que manda
  SIGSEGV, e aceita falta de **ring 0 tambem** (`err & 3`), nao so de ring 3: com
  `CR0.WP` ligado, uma syscall que escreve no buffer do usuario (um `read`, um
  `getcwd`) tambem leva falta de escrita numa pagina COW, e la o CPL e 0.
- Se a pagina tem uma referencia so, a falta apenas devolve o `PTE_WRITABLE` e
  nao copia nada. E o caso comum depois que o outro lado do fork morre ou faz
  `exec`.
- O PMM ganhou um spinlock de verdade. Antes so desabilitava interrupcoes, o que
  nunca foi correto com 4 CPUs; o COW multiplica as alocacoes concorrentes
  (uma por falta de escrita) e tornaria a corrida facil de encontrar.
- O `mem` do shell do kernel mostra paginas compartilhadas, faltas de COW e
  copias feitas. Sem isso nao da para distinguir "o fork ficou barato" de
  "o fork parou de funcionar".

## v1.6 (rede)

- Placa escolhida: Intel e1000 (82540EM), que e o padrao do QEMU e tem uma
  interface simples de aneis de descritores. O driver procura por classe PCI
  (02.00) e nao por um par vendor/device fixo.
- O MAC sai de `RAL`/`RAH` depois do reset, nao da EEPROM: o proprio hardware
  recarrega esses registradores da EEPROM, entao ler o resultado e mais simples
  e vale para qualquer variante.
- O driver usa interrupcao **e** um laco de recepcao com espera de 20 ms. A
  interrupcao e o caminho normal; o tempo limite e uma rede de seguranca para o
  caso de a IRQ do PCI nao chegar, e nao custa nada quando ela chega.
- A pilha e uma interface so (`eth0`), sem tabela de rotas: destino fora da
  mascara vai para o gateway. Uma tabela de rotas de verdade nao acrescentaria
  nada num sistema com uma placa.
- O cache ARP tem 16 entradas com validade de 2 minutos. `net_resolve` **dorme**
  esperando a resposta em vez de devolver "tente de novo": quem envia esta sempre
  em contexto de processo (uma syscall ou o cliente DHCP), entao dormir e
  legitimo e evita ter que enfileirar pacotes pendentes.
- O ICMP echo request e respondido dentro do kernel; os outros tipos de ICMP vao
  para os sockets raw. Assim o Orion responde a ping mesmo sem nenhum programa
  aberto.
- Socket raw entrega so o corpo do ICMP, sem o cabecalho IP. E diferente do
  Linux, mas o `ping` fica mais simples e nada mais usa raw hoje.
- O cliente DHCP mora no kernel, nao num programa. Ele precisa mandar de
  0.0.0.0 para 255.255.255.255 antes de a interface ter endereco, um caso
  especial que teria que vazar para a API de sockets; e a configuracao da
  interface e do kernel de qualquer jeito. O `dhcp` e o `ifconfig dhcp` so pedem
  ao kernel que rode o cliente.
- A configuracao da interface e lida e escrita por ioctl **no proprio socket**
  (`SIOCGIFCONF_ORION` e amigos), como no Unix, em vez de um `/dev/net` novo.
- Sem TCP nesta etapa (e sem `wget`): o passo pedia TCP como opcional e a
  maquina de estados com retransmissao e janela nao cabia no tempo da etapa.
  UDP, ICMP e DNS cobrem o que os programas pedidos precisam.
- O `test.ps1` grava todo o trafego em `out\net.pcap` (`filter-dump` do QEMU),
  que e o que permite conferir um pacote byte a byte quando algo nao responde.
- O console do kernel ganhou o mesmo parser CSI do terminal grafico (cursor,
  apagamento, 16 cores, video reverso). Sem ele o `edit` e o historico do `sh`
  despejavam as sequencias como texto na tela fisica, o que alem de feio fazia o
  framebuffer rolar varias vezes por tecla - e rolar 1280x800 e um memmove de
  4 MiB, o que deixava o editor segundos atras do teclado.
