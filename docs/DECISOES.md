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

## v1.7 (interface grafica: boot direto e aplicativos)

- O `init` le `/etc/autostart`: uma linha por comando, `#` e comentario, e
  `shell <caminho>` troca o programa que ele mantem vivo. Um arquivo de texto
  resolve o caso de uso sem inventar um sistema de servicos.
- O padrao do arquivo continua sendo `shell /bin/sh`. Bootar direto na GUI e
  `.\build.ps1 -Gui` (que grava um autostart com `/bin/dhcp` e `shell /bin/wm`),
  porque as sequencias de teste de texto e de disco falam com o `sh` pela serial
  e ficariam impossiveis se o padrao fosse grafico.
- Dois syscalls proprios em vez de um `/proc`: `sysinfo` (1002) e `proclist`
  (1003). Um `/proc` de verdade exigiria um sistema de arquivos virtual novo so
  para servir dois consumidores.
- As janelas `Processos` e `Arquivos` sao "paineis": um vetor de linhas de texto
  que o `wm` preenche e desenha. Cada tipo de janela nova custa uma funcao de
  atualizacao e nada mais.
- O `Arquivos` usa `readdir`/`stat`, os mesmos do `ls`; clicar num diretorio
  entra nele e `..` sobe. Sem abrir arquivo: abrir exigiria decidir com que
  programa, e nao ha associacao de tipos.
- A barra de topo corta a dica da esquerda para nao colidir com o relogio, o IP
  e a memoria da direita, em vez de deixar os dois se sobreporem.
- O menu abre no botao "Orion 1.0" e fecha em qualquer clique; nao ha submenu.
- `build.ps1` passou a considerar os cabecalhos de dentro de cada programa na
  hora de decidir se recompila. Antes so olhava os de `user/lib`, entao mudar
  `wm/win.h` nao recompilava `wm/win.c` - o binario saia com o valor antigo
  compilado dentro.

## v1.8 (extras)

- As variaveis de ambiente moram na libc, num vetor estatico de 64 entradas de
  256 bytes que o processo adota na primeira escrita (`setenv` copia o `environ`
  original antes de mexer). Sem `malloc` no caminho, e o `environ` continua
  valido depois de `fork`/`exec` porque o `execve` ja levava o ponteiro.
- A expansao de `$NOME` e `${NOME}` acontece na linha inteira, antes de separar
  os tokens. E o suficiente para o que o shell faz e evita ter que carregar o
  estado de citacao pelo parser todo.
- `NOME=valor` sozinho na linha vira uma variavel; com um comando na frente, o
  shell nao herda nada (nao tem `VAR=x cmd`), porque isso exigiria um ambiente
  por comando. Quem precisa usa o programa `env`.
- Se o `PATH` esta definido e o programa nao esta em nenhum dos caminhos, o
  shell tenta o primeiro caminho e deixa o `execv` falhar com a mensagem certa,
  em vez de cair em `/bin` escondido. Sem `PATH` o padrao continua `/bin`.
- O `/etc/autostart` tambem aceita `NOME=valor`, e o `init` passa essas
  variaveis para o shell. E onde `PATH` e `HOME` sao definidos para o sistema.
- **Nao entregue: ISO para BIOS com xorriso.** Nem `xorriso` nem `mkisofs`
  existem nesta maquina, e o `winget` nao tem nenhum dos dois. O Limine ja traz
  `limine-bios-cd.bin` e `limine-bios.sys`, entao falta so a ferramenta que
  monta o ISO9660 com El Torito - escrever uma do zero nao cabia no passo.
- A verificacao do `nslookup` na regressao depende de o hospedeiro ter DNS
  funcionando: o SLIRP do QEMU so encaminha para o resolvedor da maquina. Uma
  vez ela falhou com `SIGALRM` porque o resolvedor nao respondeu em 5 s. Ficaram
  duas verificacoes: uma estritamente local (o `nslookup` usou o servidor que
  veio do DHCP), e a resolucao de verdade, com duas tentativas.

## v1.9 (estabilidade SMP)

- O heap do kernel ganhou uma trava propria (`heap_lock`). Antes ele dependia de
  quem chamava estar com as interrupcoes desligadas, o que nunca foi correto com
  4 CPUs alocando ao mesmo tempo.
- Liberar uma alocacao grande nao desmapeia na hora: o bloco vai para uma lista
  de liberacao adiada, e o `heap_reclaim` desmapeia tudo de uma vez e faz um
  TLB shootdown **sincrono** (IPI `0xF2`, espera todas as CPUs confirmarem)
  antes de devolver as paginas ao PMM. Sem a espera, outra CPU ainda podia ter
  a traducao velha na TLB e escrever numa pagina ja reaproveitada.
- Cada alocacao grande tem uma pagina de guarda nao mapeada logo abaixo. As
  pilhas de kernel das threads sao alocacoes grandes, entao estourar a pilha
  vira `#PF` na guarda, e o dump de excecao diz "ESTOURO DA PILHA DO KERNEL".
- Double fault e NMI rodam em pilhas proprias (IST 1 e 2 do TSS), senao um
  estouro de pilha viraria triple fault e reboot silencioso.
- O spinlock guarda quem o pegou (CPU, thread e o endereco do codigo). Uma
  espera de mais de 10 s (60 s quando a thread pode ceder a CPU) vira panico
  com "deadlock provavel", dizendo a trava, quem segura e onde ela foi pega;
  pegar de novo uma trava que a propria thread segura vira panico na hora.
- Watchdog: a cada segundo cada CPU confere o contador de ticks da vizinha; se
  ele parou por 5 s (interrupcoes desligadas), manda uma NMI para a CPU parada
  imprimir o proprio estado e entra em panico.
- `/dev/null`, `/dev/zero` e `/dev/random` (xorshift semeado pelo TSC) moram na
  tabela fixa do `devfs`, como os outros dispositivos.
- Correcao achada na validacao no Windows: `net_deliver_udp` e
  `net_deliver_icmp` chamavam `wake_up_all_locked` segurando so a `sock_lock`,
  sem a `runq_lock` que a fila de espera exige. O assert novo de `entry_detach`
  pegou isso no segundo `ping` seguido. Passaram a chamar `wake_up_all`.
- O `panic` passou a imprimir a pilha de chamadas da thread atual (a mesma
  varredura do dump de excecao). Nesta maquina nao ha `llvm-addr2line`; o
  `llvm-symbolizer --obj=out\orion.elf <endereco>` faz o mesmo.

## v1.9.1 (copia segura de memoria de usuario)

- Todo acesso do kernel a memoria de usuario passa por tres funcoes:
  `copy_from_user`, `copy_to_user` e `strncpy_from_user` (`mm/uaccess.c`). As
  duas instrucoes que podem faltar (o `rep movsb` e a leitura byte a byte da
  string) moram em `arch/x86_64/uaccess.S` e cada uma tem uma entrada na tabela
  de excecoes (`.ex_table`, um par endereco-que-falta / endereco-de-retomada).
  O handler de `#PF` e de `#GP` em ring 0 procura o RIP na tabela e, se achar,
  desvia para a retomada, que devolve "faltaram N bytes"; a funcao em C vira
  isso em `-EFAULT`. E o mesmo desenho do Linux.
- A ordem no handler importa: primeiro a falta de COW (escrita do kernel numa
  pagina copy-on-write do usuario e legitima e precisa ser resolvida), depois a
  tabela de excecoes, e so entao o panico.
- A faixa ainda e conferida antes (`user_range_ok`): endereco do kernel ou nao
  canonico nunca chega ao `rep movsb`. A tabela cobre o resto (pagina nao
  mapeada, pagina so de leitura, corrida com `munmap`).
- `read` e `write` usam um buffer intermediario no kernel: ate 256 bytes na
  pilha, acima disso paginas contiguas do PMM (ate 64 KiB) acessadas pelo HHDM,
  sem mapear nada. Os drivers (`file_ops->read/write`) passaram a receber so
  ponteiros do kernel, entao nenhum driver precisa saber de memoria de usuario.
  O laco continua enquanto cada pedaco vem cheio; um pedaco curto (pipe, TTY)
  encerra a chamada como antes.
- Se a copia para o usuario falha depois de o driver ja ter consumido os dados
  (um `read` de pipe num buffer invalido), os dados se perdem e a chamada
  devolve `EFAULT`, como no Linux.
- Os ioctls copiam a estrutura inteira para uma variavel local e de volta, em
  vez de escrever campo a campo no ponteiro do usuario.
- `execve` copia o caminho e cada string de `argv`/`envp` para o kernel antes de
  tocar no processo (no maximo 256 argumentos de ate 4 KiB cada; acima disso
  `E2BIG`). O `build_stack` recusa um conjunto que passe de metade da pilha de
  64 KiB, em vez de escrever abaixo dela.
- `rt_sigreturn` volta ao usuario por `iretq`, nao por `sysret`: o `sysret`
  sobrescreve RCX e R11 com RIP e RFLAGS, e o codigo interrompido pelo sinal
  (por uma IRQ, no meio de qualquer conta) perdia esses dois registradores.
  O `syscall_dispatch` devolve `true` quando o quadro inteiro precisa ser
  restaurado, e o `syscall_entry` desvia para o `user_return`.
- O quadro restaurado pelo `rt_sigreturn` e higienizado: RIP e RSP precisam
  estar na metade de usuario (um RIP nao canonico num `iretq` vira `#GP` em ring
  0) e o RFLAGS so aproveita os bits aritmeticos e o DF (nada de IOPL, TF, NT,
  AC), com IF sempre ligado.
- Handler ou restorer de sinal fora da metade de usuario mata o processo com
  SIGSEGV na hora de entregar, em vez de deixar o `sysret` voltar para um
  endereco nao canonico.
- O carregador ELF confere estouro de inteiro nas contas de segmento e de
  tabela de cabecalhos, `p_filesz <= p_memsz` e a entrada na metade de usuario.
- `poll` passou a devolver `EINTR` quando um sinal chega durante a espera.
  Antes ele voltava a dormir e o sinal so era tratado quando algum fd ficasse
  pronto.
- `FMASK` (as flags que a instrucao `syscall` zera na entrada) passou a incluir
  NT e AC, como no Linux. O usuario pode ligar NT com `popfq`; enquanto toda
  syscall voltava por `sysret` isso era inofensivo, mas um `iretq` com NT ligado
  em modo longo da `#GP` em ring 0, e com o GS do usuario ja carregado o sistema
  morria em silencio. Achado por uma revisao do proprio diff antes do commit;
  o `badptr` tem um caso que liga NT dentro de um handler de sinal.
- Falta sincrona (`#PF`, `#UD`, `#DE`...) em ring 3 com o sinal correspondente
  ignorado ou bloqueado mata o processo, como o "force signal" do Linux. Antes o
  processo voltava para a mesma instrucao e faltava de novo para sempre, enchendo
  o log.
- A tabela de excecoes so recupera falta de pagina em endereco de usuario
  (`CR2 < USER_ADDR_MAX`). Uma falta no lado do kernel dentro da copia e bug do
  kernel e continua virando panico em vez de um `EFAULT` silencioso.
- `execve` que falha depois de criar o espaco novo devolve o processo exatamente
  como estava (`pages`, `mmap_next`, `entry`, pilha e `brk`).
- O laco do `read` so busca um segundo pedaco em arquivo regular. Em pipe, TTY
  ou dispositivo, um pedaco cheio nao garante que haja mais dados, e uma segunda
  leitura podia bloquear com dados ja entregues.
- `thread_current()` le o `current` da CPU com uma unica instrucao relativa ao
  GS (`movq %gs:current`). Antes eram duas leituras (`percpu_get()` e depois
  `->current`): uma thread preemptada entre as duas e migrada de CPU lia o
  `current` da CPU antiga, ou seja, outra thread. Com interrupcoes ligadas
  dentro das syscalls isso acontecia de verdade no estresse: um filho
  terminava "como" o `sh` (o `sh` aparecia zumbi com a thread bloqueada) e o
  filho de verdade ficava `running` sem thread, travando o `waitpid` do pai.
  Regra: nunca dereferenciar `percpu_get()` com interrupcoes ligadas.
- Fechar a ultima ponta de um pipe ou PTY acorda a outra ponta **segurando** a
  trava do pipe, e so libera a estrutura depois de soltar a trava. Antes o
  `wake_up_all` vinha depois de soltar a trava, e a outra ponta podia fechar e
  liberar a estrutura nesse intervalo (uso apos liberacao).
- Quem termina deixa os filhos orfaos na hora (`orphan_children_locked` em
  `process_terminate`): o filho perde o ponteiro para o pai e, se ja for zumbi,
  passa a ser liberado sozinho. Antes os filhos de um zumbi colhido ficavam com
  `parent` apontando para memoria liberada.
- `execve` monta o espaco de enderecos novo numa variavel local e so publica em
  `p->pml4_phys` junto com o `write_cr3`, com interrupcoes desligadas. Antes uma
  preempcao no meio do `setup_image` fazia a thread voltar com o CR3 do espaco
  ainda em construcao, e se o `exec` falhasse ela retornava ao usuario com o
  CR3 do kernel.
- `poll_table_sleep` nao dorme se houver sinal pendente. Um `signal_send` no
  meio do registro das filas desfazia as entradas sem acordar a thread, que
  depois dormia sem estar em fila nenhuma.

## v1.9.2 (mutex que dorme)

- `struct mutex` (`kernel/mutex.{h,c}`): dono, fila de espera e um spinlock
  interno que so protege o proprio mutex. Quem encontra o mutex ocupado dorme
  na fila (`wait_sleep_timeout`) em vez de girar; quem solta acorda um.
- O mutex guarda estatisticas (aquisicoes, quantas encontraram o mutex ocupado,
  tempo total e maximo de espera) e se registra numa lista global na primeira
  vez que e usado. O comando `mutex` do shell do kernel mostra a tabela.
- Pegar de novo um mutex que a propria thread segura vira panico na hora, com
  o endereco de onde ele foi pego. Esperar mais de 5 s imprime um aviso com o
  dono, a cada 5 s, em vez de travar em silencio.
- Antes do escalonador existir o dono e uma marca especial (`boot`); disputa
  nessa fase e panico, porque nao ha como dormir.
- Disputa com interrupcoes desligadas tambem e panico: dormir com interrupcoes
  desligadas significa que quem chamou segura um spinlock, e a outra thread
  nunca conseguiria avancar. Ate a 0.4 o AHCI e chamado de dentro da trava do
  cache de blocos (spinlock com interrupcoes desligadas), entao o mutex do AHCI
  nunca e disputado; a 0.4 troca essa trava por mutex.
- Cada porta AHCI tem o seu mutex (`ahci0`, `ahci1`...). A espera pelo fim do
  comando cede a CPU (`thread_yield`) quando as interrupcoes estao ligadas, em
  vez de girar com `pause`.

## v1.9.3 (FAT e cache de blocos com mutex)

- A trava do cache de setores (`bcache`) e a de cada FAT montado (`fat-sda1`,
  `fat-sdb`) viraram mutex. As duas eram seguradas durante a E/S de disco: com
  spinlock, uma segunda thread querendo o disco ficava girando (ou cedendo a CPU
  em laco) pelo tempo inteiro de um comando AHCI; agora ela dorme na fila.
- Ordem das travas do disco: `fat-*` -> `bcache` -> `ahci*`. O `bdflush` segue
  a mesma ordem (descarrega a FAT e depois o cache).
- Como o cache agora e mutex, o AHCI e sempre chamado com interrupcoes ligadas
  e a espera pelo fim do comando cede a CPU de verdade.
- A sequencia C da regressao roda duas `fstest -a` ao mesmo tempo (um pipeline
  com a saida da primeira redirecionada para arquivo, enquanto o shell ainda nao
  tem `&`) e confere na tabela do `mutex` que houve disputa e espera dormindo.

## v1.9.4 (harness em Python e CI)

- `tools/qrun.py` faz o que o `test.ps1` faz (QEMU sem janela, serial e
  monitor por TCP, os mesmos passos `cmd:`, `raw:`, `key:`, `keys:`, `mon:`,
  `wait:`, `until:`, `shot:`) e roda em Linux, WSL, GitHub Actions e Windows.
  O firmware UEFI vem do diretorio do QEMU (Windows) ou do pacote `ovmf`.
- `tools/regress.py` nao tem uma copia dos passos: ele le o `regressao.ps1`
  (listas `$stepsX`, linhas `Expect` e `Test-MemDelta`, opcoes da chamada ao
  `test.ps1`) e executa. Duas listas para manter em sincronia acabariam
  divergindo; assim o `regressao.ps1` continua sendo a fonte unica e o dono
  segue usando os mesmos comandos. As regex sao avaliadas sem diferenciar
  maiusculas, como o `-match` do PowerShell.
- No Linux o build e o `make` (o `Makefile` ja existia); o autostart, que o
  `build.ps1` grava, e gravado pelo `qrun.py`.
- Sequencia nova E na regressao: o estresse do plano
  (`stress 3000 16` x4 em pipeline), uma rodada por padrao;
  `.\regressao.ps1 -OnlyE -StressRuns 5` e o estresse 5x das etapas que mexem
  em trava, escalonador, memoria, processos, threads ou sinais.
- `.github/workflows/regressao.yml` roda `make` e `tools/regress.py ABCDE` num
  Ubuntu 24.04 a cada push. Ele so roda quando o dono fizer push; daqui nao da
  para verificar (o intensivo nao faz push).

## v1.9.5 (shell: listas e trabalhos em segundo plano)

- A linha e quebrada primeiro em uma lista de segmentos separados por `;`,
  `&&`, `||` e `&` (nessa ordem de reconhecimento: `&&` antes de `&`, `||`
  antes de `|`); cada segmento e um pipeline. `&` logo depois de `<` ou `>`
  nao separa, para nao confundir com um futuro `2>&1`.
- A expansao de `$NOME` passou a ser por segmento, na hora de rodar, e nao na
  linha inteira de uma vez. E o que faz `false ; echo $?` mostrar 1. `$?` e o
  status do ultimo segmento: codigo de saida, ou 128 + sinal.
- `NOME=valor cmd` guarda as atribuicoes no comando e as aplica no filho logo
  antes do `execv`; o shell nao muda. Para builtins, o shell aplica, roda e
  restaura o valor anterior. So atribuicoes, sem comando, continuam mudando o
  ambiente do shell.
- Trabalhos em segundo plano ficam numa tabela de 16 posicoes (pgid, pids de
  cada estagio, texto). O shell espera os de primeiro plano por pid
  especifico, entao nao colhe os de segundo plano por engano; esses sao
  colhidos com `WNOHANG` antes de cada prompt, que avisa `[n] pronto`.
- Trabalho em segundo plano le de `/dev/null` quando nao tem `<`: sem isso ele
  disputaria o teclado com o shell (ainda nao ha `SIGTTIN`).
- `fg` passa o terminal para o grupo do trabalho e espera; `wait` sem
  argumento espera todos. Nao ha `bg` nem suspensao (Ctrl+Z), porque o kernel
  ainda nao tem `SIGTSTP`/`SIGCONT`.
- Programa novo `false` (o `true` ja existia), para testar `&&` e `||`.

## v1.9.6 (limites)

- A tabela de descritores deixou de ser um vetor fixo de 32 dentro do
  `struct process`: e um vetor alocado que comeca com 32 posicoes e dobra
  (64, 128, 256) quando um `open`/`dup`/`dup2` precisa de um numero maior,
  ate o `RLIMIT_NOFILE` (no maximo 256). Um vetor fixo de 256 deixaria todo
  `struct process` com mais de 2 KiB, o que o tira do slab e faz cada `fork`
  pagar uma alocacao grande (mapear paginas e, na liberacao, um TLB shootdown).
- `getrlimit`, `setrlimit` e `prlimit64` (este so para o proprio processo).
  `RLIMIT_NOFILE` comeca em 256/256, `RLIMIT_NPROC` em 512/512 e
  `RLIMIT_STACK` informa os 64 KiB da pilha. Baixar o limite e livre; subir o
  maximo da `EPERM` (ainda nao ha usuario root). Os limites sao herdados no
  `fork`.
- `RLIMIT_NPROC` e conferido no `fork` contra o numero total de processos
  (ainda ha um usuario so), e o `fork` devolve `EAGAIN` ao passar dele.
- `proclist` aceita ate 256 entradas (`PROCLIST_MAX` da libc tambem).
- `ulimit` no `sh` (`-a`, `-n`, `-u`, `-s`), mudando so o limite corrente.
- O `stress` ganhou um terceiro argumento (milissegundos que cada filho fica
  vivo) e aceita ate 256 filhos ao mesmo tempo; com ele imprime o pico de
  vivos.

## v2.0.0-a1 (pilha inicial do Linux e estado de SSE)

- A fase 1 leva a versao 2.0; enquanto ela nao fecha, o kernel se chama
  `2.0.0-aN` (N = etapa da fase 1) e os commits levam `v2.0:` no titulo.
- A pilha inicial segue o Linux: `argc`, `argv`, `NULL`, `envp`, `NULL` e o
  vetor auxiliar (`AT_PHDR`, `AT_PHENT`, `AT_PHNUM`, `AT_PAGESZ`, `AT_BASE`,
  `AT_FLAGS`, `AT_ENTRY`, `AT_UID`/`EUID`/`GID`/`EGID`, `AT_SECURE`,
  `AT_CLKTCK`, `AT_HWCAP`, `AT_PLATFORM`, `AT_RANDOM`, `AT_EXECFN`, `AT_NULL`).
  As strings, o nome da plataforma e os 16 bytes aleatorios ficam no topo;
  `argc` fica alinhado em 16, como o ABI pede.
- `AT_PHDR` aponta para os cabecalhos dentro de um segmento carregado (ou do
  `PT_PHDR`). Os programas da libc propria sao ligados sem os cabecalhos
  carregados; para eles o kernel copia a tabela para a pilha e aponta para la,
  em vez de deixar 0. A musl usa o `AT_PHDR` para achar o `PT_TLS`.
- Os bytes aleatorios saem de `lib/random.c` (xorshift semeado pelo TSC), o
  mesmo gerador do `/dev/random`, que deixou de ter um proprio.
- SSE ligado em todas as CPUs (`CR4.OSFXSR`/`OSXMMEXCPT`, `CR0.EM` desligado).
  A musl compilada pelo Zig usa SSE; o kernel continua compilado sem SSE.
- Cada thread de processo tem uma area de 512 bytes para `fxsave64`. A troca e
  ansiosa: `switch_to` salva a anterior e restaura a proxima sempre. Como o
  kernel nunca toca em SSE, os registradores durante uma syscall continuam
  sendo os do usuario, o que simplifica `fork` (salva os registradores vivos
  no inicio do filho) e `execve` (restaura o estado inicial).
- O estado inicial e capturado uma vez no boot depois de `fninit` e
  `ldmxcsr 0x1f80`, com as areas de ST e XMM zeradas a mao: `fninit` nao
  zera os XMM e o firmware UEFI os usa.
- O quadro de sinal leva o estado de SSE; o `rt_sigreturn` o restaura depois de
  aplicar a mascara do MXCSR (bits reservados no MXCSR fazem o `fxrstor` dar
  `#GP` em ring 0).

## v2.0.0-a2 (syscalls de partida da musl)

- Sinais no formato do Linux: 64 sinais (1..64) e a mascara com o sinal `n` no
  bit `n-1`. Antes o Orion tinha 31 sinais e usava o bit `n`; a musl passa
  mascaras no formato do Linux para `rt_sigprocmask` e `rt_sigaction`. A libc
  propria mudou junto (`sigaddset`, `sigdelset`, `sigismember`).
- A tabela de handlers saiu de dentro do `struct process` para uma alocacao
  propria de exatamente 2 KiB (64 x 32 bytes), que cabe no slab; dentro do
  processo ela o faria passar de 2 KiB e virar alocacao grande em todo `fork`.
- `SIGKILL` e `SIGSTOP` nao podem ser bloqueados nem ter handler. Sem suporte a
  parar processos, `SIGSTOP`, `SIGTSTP`, `SIGTTIN`, `SIGTTOU` e `SIGCONT` sao
  ignorados por padrao, como `SIGCHLD`, `SIGURG` e `SIGWINCH`.
- `rt_sigaction` e `rt_sigprocmask` exigem `sigsetsize` = 8, como o Linux.
- `readv`/`writev` repetem `read`/`write` por pedaco e param no primeiro curto.
- `set_tid_address` guarda o endereco na thread; na saida o kernel escreve 0
  nele. O despertar por `futex` chega na 1.7. `gettid` e o pid enquanto cada
  processo tem uma thread so; `exit_group` e o `exit`.
- `uname`: `Orion`, `orion`, a versao do kernel, `#1 SMP <data>` e `x86_64`.
- `getuid`/`geteuid`/`getgid`/`getegid` devolvem 0 e `sigaltstack` aceita e
  informa `SS_DISABLE`: sao stubs pedidos pelo plano e estao marcados assim em
  `docs/SYSCALLS.md`, que lista o estado de cada syscall.
- `struct termios` do `TCGETS` com o layout do kernel Linux (36 bytes:
  quatro flags, `c_line`, `c_cc[19]`) e valores padrao de verdade (`CS8`,
  `ICRNL`, `OPOST|ONLCR`, `VINTR`=3, `VEOF`=4, `VMIN`=1...). `TCSETSW` e
  `TCSETSF` sao aceitos como `TCSETS`.

## v2.0.0-a3 (arquivos no formato do Linux)

- `struct stat` no layout do Linux x86_64 (144 bytes) em `stat`, `fstat`,
  `lstat` e `newfstatat`. `st_blocks` conta setores de 512; datas sao a hora
  do boot ate o VFS guardar datas por arquivo (2.1); `st_dev` e 1.
- Todo arquivo aberto por caminho guarda o caminho absoluto (`file->path`). E
  o que permite `openat`, `newfstatat`, `mkdirat`, `unlinkat`, `renameat`,
  `faccessat` e `readlinkat` resolverem um caminho relativo a um descritor de
  diretorio, sem um VFS de inodes (que chega na 2.1).
- `getdents64` reaproveita o `readdir` por indice que os sistemas de arquivos
  ja tinham: o `offset` do arquivo de diretorio vira o indice da proxima
  entrada, e `lseek(fd, 0, SEEK_SET)` recomeca a listagem. `d_type` sai do
  `is_dir`; dentro de `/dev`, os discos sao `DT_BLK` e o resto `DT_CHR`.
- A libc propria passou a listar diretorios com `getdents64` (um bloco de
  2 KiB por chamada em vez de uma syscall por entrada) e o `struct dirent`
  perdeu os campos proprios (`d_size`, `d_is_dir`): o `ls` usa `d_type` e
  `stat`. A syscall propria `readdir` (1000) continua existindo.
- Flags por descritor (`FD_CLOEXEC`) num vetor paralelo a tabela de arquivos.
  `O_CLOEXEC` no `open`/`openat`, `F_DUPFD`, `F_DUPFD_CLOEXEC`, `F_GETFD`,
  `F_SETFD`, `dup3` e `pipe2` (`O_CLOEXEC`, `O_NONBLOCK`); o `execve` fecha os
  marcados.
- `open` confere o caso antes de chamar o sistema de arquivos e devolve o erro
  certo: `EEXIST` (`O_CREAT|O_EXCL`), `ENOTDIR` (`O_DIRECTORY` num arquivo),
  `EISDIR` (diretorio aberto para escrita), `ENOENT`, `EROFS`.
- `readlink` devolve `EINVAL` para um caminho que existe (ainda nao ha links
  simbolicos, entao nada existente e link) e `ENOENT` para um que nao existe,
  como o Linux. O plano dizia "devolve ENOENT"; `EINVAL` e o que a musl espera
  de um arquivo comum.
- `access`/`faccessat` so conferem a existencia: ainda nao ha permissoes (4.1).

## v2.0.0-a4 (memoria do jeito Linux)

- Cada processo tem uma lista ordenada de areas (`struct vma`: inicio, fim,
  permissoes, arquivo e deslocamento) em `mm/vma.c`. Ela substitui o
  `mmap_next`, que so crescia. Uma lista simples basta enquanto os processos
  tem dezenas de areas; uma arvore so faria diferenca com milhares.
- Memoria anonima, `brk` e a pilha sao alocados sob demanda: o `mmap` so cria
  a area, e a pagina nasce zerada na primeira falta. A falta e tratada pelo
  mesmo caminho para usuario e kernel (um `copy_to_user` num buffer ainda nao
  tocado tambem cai ali), antes da tabela de excecoes.
- O handler de falta de pagina religa as interrupcoes quando o codigo
  interrompido as tinha ligadas: preencher uma pagina de arquivo le o disco, e
  o disco usa mutex. Com interrupcoes desligadas (quadro de sinal empurrado
  numa IRQ) a falta de arquivo e recusada; a anonima continua funcionando.
- A pilha e uma area de 8 MiB com so os 64 KiB do topo mapeados de saida
  (onde o `execve` escreve argumentos e `auxv`); o resto vem sob demanda.
  `RLIMIT_STACK` continua informando 64 KiB por enquanto.
- Os segmentos do ELF continuam carregados de uma vez, mas ganham areas.
  Dois segmentos que dividem uma pagina viram uma area com a uniao das
  permissoes, e as paginas ja mapeadas sao reescritas com ela.
- `mprotect` divide areas e reescreve as PTEs presentes. Uma pagina
  copy-on-write continua sem escrita no PTE mesmo quando a area volta a ter
  escrita: quem decide se a escrita vale e a area, e a copia acontece na falta.
- `PROT_NONE` numa pagina ja populada nao a libera: o PTE fica sem o bit de
  presente e com um bit de software (`PTE_PROTNONE`, bit 11) guardando o
  endereco, e volta inteiro no proximo `mprotect`. O `fork` e a destruicao do
  espaco de enderecos conhecem esse bit.
- `MAP_SHARED` anonimo e compartilhado de verdade entre pai e filho: o `fork`
  consulta a area e, nesse caso, nao marca copy-on-write.
- `MAP_PRIVATE` de arquivo le a pagina do arquivo na falta (o cache de paginas
  unificado e a etapa 2.4). `MAP_SHARED` de arquivo so e aceito sem escrita (e
  ai e igual ao privado); com `PROT_WRITE` devolve `ENODEV`, porque ainda nao
  ha como escrever de volta no arquivo.
- Dispositivo (`/dev/fb0`) ganhou uma area `NOFREE`; o driver recebe o
  endereco escolhido pelo `mmap` em vez de escolher sozinho.
- `brk` segue o Linux: devolve o break novo, ou o antigo quando nao da. A libc
  propria mudou junto. O heap e uma area que cresce e encolhe.
- Enderecos de `mmap` sem `MAP_FIXED`: a dica, se estiver livre e alinhada;
  senao o primeiro buraco a partir de `0x100000000000`. ASLR fica para a 4.4.
- `madvise(MADV_DONTNEED)` descarta as paginas privadas (a proxima leitura ve
  zero, ou o arquivo de novo); as outras dicas sao aceitas sem efeito.
- Depois de uma revisao do diff, antes do commit:
  - No `fork`, toda pagina privada vira copy-on-write, inclusive as que ja
    estavam so de leitura. Antes uma pagina so de leitura era compartilhada
    sem o bit, e um `mprotect(PROT_WRITE)` depois deixava um processo escrever
    na pagina do outro.
  - `MAP_SHARED` anonimo e populado inteiro no `mmap`. Sob demanda, uma pagina
    tocada so depois do `fork` nascia separada em cada processo; com as
    paginas criadas antes, o `fork` sempre as compartilha. Um objeto de
    memoria compartilhada de verdade fica para quando houver `shm`.
  - Area de dispositivo nao gera pagina sob demanda: o driver ja mapeou tudo o
    que existe, e o resto da area e `SIGSEGV`.
  - `mprotect` preserva os bits de cache da PTE (`PWT`/`PCD`), que o
    framebuffer usa.
  - `MADV_DONTNEED` nao descarta segmentos do executavel (`VMA_IMAGE`): eles
    nao tem arquivo por tras e voltariam zerados.
  - `brk` trabalha sobre a faixa entre o break antigo e o novo (cresce so se
    ela estiver livre, encolhe com `munmap`), em vez de redimensionar a
    "primeira area de heap", que podia ter sido cortada por um `munmap`.
  - `mmap` nunca mapeia a ultima pagina da metade de usuario: um `sysret` com
    RIP em `0x7ffffffff000+` voltaria para um endereco nao canonico.
  - O handler de excecao le o CR2 uma vez so, no comeco: com as interrupcoes
    religadas, outra falta (de outra thread) pode trocar o CR2 no meio.

## v2.0.0-a5 (programas contra a musl com o Zig)

- A musl chega pelo `zig cc -target x86_64-linux-musl -static`, como o plano
  manda: o Zig traz a musl e os cabecalhos do Linux prontos, entao nao ha
  configure, `alltypes.h` gerado nem WSL. Versao fixa 0.16.0 (abril de 2026),
  baixada do site oficial pelo `tools/zig.ps1` com o SHA-256 conferido, em
  `third_party/zig` (fora do git: sao ~100 MB). O `setup.ps1` chama o mesmo
  script, e o `build.ps1` o chama sozinho se o Zig faltar.
- Programas da musl moram em `user/musl/<nome>/` e viram `/bin/<nome>`. Um
  `build.txt` opcional lista fontes de fora (`src`), opcoes (`flag`) e
  dependencias para a recompilacao incremental (`dep`); e o que vai trazer o
  `dash` e o `lua` na 1.9 sem um Makefile por programa.
- O Zig gera `ET_EXEC` estatico em `0x1000000` com os cabecalhos dentro do
  primeiro segmento; o carregador do Orion aceitou sem mudanca. `-s` tira a
  informacao de depuracao (o `hello` cai de 1,3 MiB para 18 KiB).
- O `check.ps1` confere os programas da musl com o proprio `zig cc` (que nao
  aceita `-fsyntax-only`: compila para um objeto temporario).
- No Linux e no CI, `tools/musl_build.py` faz o mesmo que o `build.ps1` (o
  `Makefile` continua so com o kernel e a libc propria); o workflow baixa o
  Zig 0.16.0 para Linux com o SHA-256 conferido.
- `sched_getaffinity` virou syscall de verdade: a musl a usa para contar CPUs.
- A sequencia A ganhou uma verificacao negativa: nenhuma syscall desconhecida
  durante a sequencia inteira.

## v2.0.0-a6 (threads: clone, exit_group, sinais por thread)

- `clone(CLONE_THREAD)` cria uma thread no mesmo processo. Exige `CLONE_VM` e
  `CLONE_SIGHAND`, como o Linux; o tid sai do mesmo contador dos pids e fica
  em `thread->utid` (`gettid` e `set_tid_address` devolvem ele). Tratados:
  `CLONE_SETTLS`, `CLONE_PARENT_SETTID`, `CLONE_CHILD_SETTID` e
  `CLONE_CHILD_CLEARTID`. A thread nasce parada, recebe tid, TLS e mascara e
  so entao entra na lista do processo e na fila, para nunca rodar meio pronta.
- `clone` sem `CLONE_THREAD` e um fork que aceita pilha nova (e o que o
  `posix_spawn` da musl usa); `CLONE_VM` sem thread vira copia com COW, o que
  basta para quem faz `exec` logo em seguida. `vfork` e `fork`.
- Mascara de sinais por thread; pendentes por processo e por thread. O envio
  ao processo escolhe uma thread que nao bloqueia o sinal (de preferencia a
  atual) e a acorda. O sigframe guarda a mascara da thread.
- `exit` encerra so a thread; a ultima a sair encerra o processo com o status
  dela. `exit_group` (e a morte por sinal) marca o grupo, liga `kill_me` nas
  outras threads e as acorda; cada uma sai sozinha ao passar pelo
  `signal_check` na volta ao usuario, e as esperas interrompiveis voltam com
  `EINTR` porque `kill_me` conta como sinal pendente. So a ultima thread fecha
  os descritores, solta as VMAs e vira zumbi; o espaco de enderecos e liberado
  quando o ceifador recolhe essa thread (`last_of_process`).
- `execve` com varias threads mata as outras, espera elas sairem e a que
  ficou assume o pid como tid.
- `fork` de um processo com threads copia so a thread que chamou. A copia do
  espaco de enderecos segura o `mm_lock` do pai, e depois faz o shootdown de
  TLB: as paginas do pai viraram COW so-leitura, e outra CPU rodando outra
  thread do pai ainda podia ter a traducao gravavel no TLB.
- `mm_lock` (mutex por processo, fora do registro de travas porque ha um por
  processo) protege a lista de VMAs e as tabelas de paginas: mmap, munmap,
  mprotect, madvise, brk e a falta de pagina. Com interrupcoes desligadas a
  falta nao pode dormir, entao so tenta a trava e, se nao conseguir, a falta
  falha e a tabela de excecoes devolve `EFAULT` (codigo do kernel que copia
  de ou para o usuario segurando um spinlock fica sujeito a isso). Mudancas que tiram ou rebaixam
  traducoes (munmap, mprotect, DONTNEED, brk encolhendo, COW) fazem shootdown
  quando o processo tem mais de uma thread viva.
- A tabela de descritores e protegida pelo `tlock` do processo. Cada syscall
  "prende" os arquivos que usa (`process_fd_pin`: uma referencia a mais,
  guardada na thread) e solta todos no fim da syscall; assim um `close` em
  outra thread nao libera o arquivo no meio de um `read`. Sao ate 40 por
  syscall, acima do maximo de 32 do `poll`.
- Espera interrompivel (`wait_sleep_intr`): depois de entrar na fila e com a
  trava da fila de prontos segurada, confere se ha sinal pendente e, se ha,
  nao dorme. O envio de sinal marca o pendente antes de pegar essa trava para
  acordar, entao o sinal que chega entre a verificacao do chamador e o sono
  nao se perde mais. Pipe, terminal, pty e `waitpid` usam ela. `pause` virou
  syscall de verdade.
- Futex ainda nao existe (etapa 1.7): a musl cai num laco que tenta a syscall
  de novo, o que funciona mas gasta CPU. A verificacao "nenhuma syscall
  desconhecida" da sequencia A tolera a 202 ate la.
- Depois de uma revisao do diff, antes do commit:
  - O processo conta as structs de thread vivas (`thread_refs`), e o espaco
    de enderecos so e destruido quando a ultima delas e liberada pelo
    escalonador, ja fora da CPU. Antes valia a ultima thread a *sair*: uma
    thread que saiu antes, mas ainda nao tinha trocado de contexto (ou foi
    preemptada no caminho), podia voltar a rodar com o processo ja colhido.
  - `execve` le o executavel antes de matar as outras threads (um `execvp`
    que testa varios caminhos nao derruba as threads a cada falha) e espera
    as structs serem liberadas, nao so as threads sairem. Durante a espera o
    processo fica marcado (`execing`): um `clone` nesse meio nasce marcado
    para sair, um segundo `execve` desiste com `EAGAIN` e sai junto com as
    outras, e um `exit_group` de outra thread vence o `execve`.
  - Todo acesso a tabela de descritores passa pelo `tlock`: `dup3`, a copia
    no `fork` e `F_GETFD`/`F_SETFD` faltavam.
  - O `signal_check` liga as interrupcoes quando ha sinal a entregar (ele so
    roda na volta ao usuario, sem trava nenhuma segurada). Assim a falta de
    pagina ao empurrar o quadro na pilha de uma thread nova pode esperar o
    `mm_lock` em vez de matar o processo com `SIGSEGV`.
  - Sinais de falta (`SIGSEGV`, `SIGFPE`, `SIGILL`) e o `SIGPIPE` vao para a
    thread que os causou, nao para o processo.
  - Duas threads nao entregam mais o mesmo sinal: quem pega o sinal e quem
    efetivamente apagou o bit de pendente.
  - Uma thread que passa a bloquear um sinal ja pendente no processo acorda
    outra que o aceite.
  - `MAP_FIXED` que falha faz o shootdown mesmo assim: o mapeamento antigo ja
    foi desfeito.
  - Escrita no lado escravo do pty e interrompivel; antes uma thread presa
    ali segurava `exit_group` e `execve` para sempre.

## v2.0.0-a7 (futex)

- Uma trava global (`futex_lock`) e 64 baldes de esperadores. Cada esperador
  e uma struct na pilha de quem espera, com fila de espera propria; acordar e
  tirar a struct do balde, marcar `woken` e acordar a fila dela. Com uma
  trava so, o `REQUEUE` e so trocar a chave e mudar de balde. Uma trava por
  balde so compensa com muitas CPUs disputando enderecos diferentes.
- Chave: com `FUTEX_PRIVATE_FLAG`, ou numa area privada, e (processo,
  endereco virtual), que e o que as threads de um processo dividem. Numa area
  `MAP_SHARED` sem a flag, e o endereco fisico da palavra, o que faz um mutex
  `PTHREAD_PROCESS_SHARED` funcionar entre pai e filho. Area privada depois do
  `fork` continua com a chave por processo: o copy-on-write separa as paginas,
  entao dois processos nunca esperam na mesma palavra privada.
- `FUTEX_WAIT` le a palavra uma vez antes de pegar a trava (para a falta de
  pagina acontecer com interrupcoes ligadas) e de novo com a trava, que e a
  leitura que vale: quem muda a palavra e chama `FUTEX_WAKE` pega a mesma
  trava, entao ou o esperador ve o valor novo, ou ja esta no balde quando o
  wake procura.
- O sono e o `wait_sleep_intr` da 1.6, com o tempo que falta ate o prazo:
  sinal devolve `EINTR`, prazo vencido `ETIMEDOUT`, acordar sem ser por wake
  nem sinal (um `kick`, por exemplo) volta a dormir. Timeout zero devolve
  `ETIMEDOUT` sem dormir; o resto e arredondado para cima em milissegundos.
- `clear_child_tid` agora acorda um esperador depois de zerar a palavra, que
  e o que o `pthread_join` da musl espera; sem isso ele girava.
- Sem `FUTEX_WAIT_BITSET`, `WAKE_OP` e as operacoes PI: a musl usa so
  `WAIT`, `WAKE` e `REQUEUE` (PI so com `PTHREAD_PRIO_INHERIT`, que devolve
  erro). `ENOSYS` para o resto.
- O comando `mutex` do shell do kernel mostra quantas threads estao no futex;
  a regressao confere que sobra zero depois dos testes.
- Depois de uma revisao do futex, antes do commit:
  - Prazo enorme (como `{INT64_MAX, ...}`, que a musl manda quando o
    `abstime` esta muito longe) passou a ser "sem prazo" em vez de estourar a
    conta e vencer na hora. Timeout zero tem uma flag propria, e o prazo leva
    um tick de folga para nunca vencer antes do pedido.
  - Fila por chave em ordem de chegada (FIFO): com a pilha (LIFO) de antes,
    quem voltava a esperar logo era sempre o primeiro acordado, e o mais
    antigo podia nunca sair.
  - A leitura da palavra com a trava segurada, se falhar (a pagina sumiu
    entre as duas leituras), solta a trava, traz a pagina de novo e tenta outra
    vez, em vez de devolver `EFAULT`.
  - Retornos como os do Linux: `REQUEUE` devolve acordados mais movidos,
    `WAKE` com contagem zero acorda um, endereco fora da metade de usuario da
    `EFAULT` e desalinhado `EINVAL`.
  - Os testes do `mfutex` nao dependem mais de `usleep` para saber se a thread
    ja esta esperando: contam os esperadores com um `CMP_REQUEUE` da palavra
    para ela mesma, que nao acorda ninguem.
  - Fica para a 1.8: sem `SA_RESTART` de verdade, toda espera interrompida
    devolve `EINTR`.

### Segunda revisao da 1.6 (corrigida junto com a 1.7)

- Paginas de usuario so voltam ao alocador depois do shootdown de TLB. Com
  mais de uma thread, `munmap`, `MADV_DONTNEED`, `brk` encolhendo e
  `MAP_FIXED` guardam as paginas num lote do processo (`free_batch`, 32
  entradas, protegido pelo `mm_lock`) e o `process_mm_tlb_sync` as libera
  depois do shootdown sincrono. Antes, outra CPU com a traducao velha no TLB
  podia escrever numa pagina que ja era de outro processo. A copia do
  copy-on-write faz o mesmo com a referencia da pagina antiga.
- A thread nova entra na lista do processo e na fila de prontas na mesma
  secao critica. Antes, entre as duas coisas, um `exit_group` ou um sinal
  podia acordar a thread, ela saia e era liberada, e o criador mexia nela
  depois. O `fork` guarda o pid antes de soltar o filho.
- `SIGCHLD` so e enviado quando o filho ja pode ser colhido (no
  `process_thread_reaped`). Um handler com `waitpid(WNOHANG)` recebia o
  sinal cedo demais, nao achava o filho e nao recebia outro sinal.
- Sinal para o processo nao acorda mais uma thread que o bloqueia: fica
  pendente ate alguem desbloquear. Quando uma thread sai, ou bloqueia um
  sinal pendente (pelo `sigprocmask`, pela mascara do handler ou pelo
  `sigreturn`), outra thread que o aceite e acordada. A troca de mascara e o
  calculo do que ficou bloqueado acontecem sob o `tlock`.
- `fork` copia `brk`, pilha e entrada junto com as VMAs, sob o `mm_lock`, e o
  `cwd` junto com a tabela de descritores, sob o `tlock`; `chdir`, `getcwd` e
  a resolucao de caminhos relativos leem e escrevem o `cwd` sob o `tlock`.
- `pipe2` e `openpty` que falham depois de instalar os descritores so fecham
  o numero se ele ainda for o arquivo que instalaram (outra thread podia ter
  fechado e reaberto o mesmo numero).
- `fork` que falha ao criar a thread solta as referencias dos arquivos.
- Leitura de `/dev/input` e interrompivel.
- Depois de um NMI o kernel nao entrega sinais (o NMI roda numa pilha de
  emergencia). Fica pendente: um NMI que chegue na janela entre a entrada da
  syscall e o `swapgs` ainda roda com o GS do usuario; so acontece com NMI de
  fora (o monitor do QEMU) e e assunto da etapa de robustez.
- Fica para a 1.8: `nanosleep` acordado por sinal ainda devolve 0 sem o tempo
  restante.

## v2.0.0-a8 (tempo e sinais completos)

- Reinicio de syscall como no Linux. A entrada da syscall guarda o numero
  original no campo `err_code` do quadro (antes ia zero). As esperas
  bloqueantes devolvem codigos internos (`ERESTARTSYS`, `ERESTARTNOHAND`,
  `ERESTARTNOINTR`, 512 a 514) e o `signal_check` decide na volta ao usuario:
  com handler e `SA_RESTART` (ou `ERESTARTNOINTR`), volta o `rip` 2 bytes, para
  cima da instrucao `syscall`, e repoe o `rax`; com handler sem `SA_RESTART`,
  vira `EINTR`; sem handler nenhum (o sinal foi para outra thread ou era
  ignorado), reinicia. O `sigreturn` marca o quadro restaurado como "nao
  reiniciar", para um `rax` de usuario que por acaso valha -512 nao disparar
  nada. So existe a porta da instrucao `syscall` (nao ha `int 0x80` para o
  usuario), entao o vetor 0x80 no quadro identifica uma syscall.
- Quem reinicia: leitura e escrita de pipe, terminal e pty, `/dev/input`,
  `recvfrom`, `wait4` e `futex` sem prazo. `pause` e `rt_sigsuspend` usam
  `ERESTARTNOHAND` (sempre `EINTR` depois de um handler). `poll`,
  `nanosleep`, `rt_sigtimedwait` e esperas com prazo devolvem `EINTR` direto:
  reiniciar com os argumentos originais recomecaria o prazo do zero (o Linux
  usa um `restart_block` para isso, que nao vale o custo aqui).
- `rt_sigsuspend` guarda a mascara antiga na thread (`saved_mask`); o quadro
  do handler leva essa mascara, entao o `sigreturn` a devolve. Se nenhum
  handler rodar, o `signal_check` a devolve antes de reiniciar.
- `rt_sigtimedwait` desbloqueia o conjunto esperado durante a espera (e o que
  faz o envio escolher esta thread e acorda-la) e devolve a mascara antes de
  voltar. Sinais comuns nao tem fila: um segundo envio do mesmo sinal pendente
  se junta ao primeiro (e guarda a origem do primeiro), como no Linux.
- `tkill` e `tgkill` acham a thread pelo tid em todos os processos; a musl
  usa o `tkill` no `raise` e no `pthread_kill`. Sinal para uma thread que o
  bloqueia fica pendente nela sem acorda-la.
- `SA_SIGINFO`, `SA_NODEFER` e `SA_RESETHAND` valem (o formato do quadro esta
  no fim desta secao).
- Tempo de CPU medido pelo TSC na troca de contexto (`run_us` por thread),
  nao por ticks: no QEMU TCG o timer do LAPIC chega bem abaixo dos 1000 Hz
  nominais (medido: uns 70 por segundo na CPU 0), e contar ticks dava 19 ms
  para um laco de 300 ms. Os ticks so dividem o total entre usuario e sistema
  (a fracao de ticks que pegou a thread em modo usuario), como o Linux faz.
  Threads que saem somam no processo; `wait4` soma o filho (e os filhos dele)
  no pai.
- Relogios: os de parede e os monotonicos vem do TSC em microssegundos; os
  `COARSE` cortam em milissegundos; `BOOTTIME` e `MONOTONIC_RAW` sao o
  `MONOTONIC` (nao ha suspensao nem ajuste de frequencia); `TAI` e o
  `REALTIME` (o Linux tambem usa deslocamento zero sem NTP).
- `nanosleep` e `clock_nanosleep` dormem por fila de espera ate o prazo em
  microssegundos, acordando no tick seguinte; um sinal devolve `EINTR` com o
  tempo restante. Prazos acima de 10^9 segundos viram "para sempre".
- `setitimer`/`getitimer` com `ITIMER_REAL` e intervalo, em cima do mesmo
  `alarm_at` do `alarm`; a thread do alarme passou a olhar a cada 5 ms (antes
  100 ms). `ITIMER_VIRTUAL` e `ITIMER_PROF` dao `EINVAL`.
- `times` conta em ticks de 100 Hz, que e o `CLK_TCK` fixo da musl.
- O `sigtest` foi reescrito contra a musl (64 verificacoes, cada grupo num
  processo filho com prazo, para um kernel quebrado virar FALHA e nao travar
  a regressao). O da libc propria saiu.
- Depois de uma revisao do diff, antes do commit (15 problemas, todos
  corrigidos):
  - A entrega de sinal lia o endereco do handler duas vezes, sem trava: a
    primeira para validar, a segunda depois de copiar o quadro para a pilha.
    Outra thread trocando o handler no meio fazia o `sysret` voltar para um
    endereco nao canonico, o que no Intel gera #GP em ring 0 com a pilha do
    usuario. Agora a tabela de handlers so e lida e escrita sob o `tlock`, a
    entrega usa uma copia validada, e toda volta ao usuario confere que `rip`
    e `rsp` sao enderecos de usuario.
  - O quadro de sinal passou a ter o formato do `rt_sigframe` do Linux
    (`ucontext` com `mcontext` na ordem dos `REG_*`, `siginfo` e a area de
    FPU), e o handler `SA_SIGINFO` recebe `siginfo` e `ucontext`. O
    `sigreturn` restaura os registradores e a mascara do `ucontext`, entao um
    handler que muda o `rip` ali muda o lugar da volta: e o que o
    `pthread_cancel` da musl faz para cancelar uma thread presa numa syscall.
  - Cada sinal pendente guarda de onde veio (`sig_src`): `kill` com o pid de
    quem mandou, `tkill`/`tgkill` com `SI_TKILL`, `SIGCHLD` com pid e status do
    filho, falta de pagina com `SEGV_MAPERR`/`SEGV_ACCERR` e o endereco.
  - Sinal ignorado por padrao (`SIGCHLD`, `SIGWINCH`, `SIGURG`) so e
    descartado no envio se alguma thread o deixa desbloqueado; bloqueado, fica
    pendente para `sigtimedwait` e `sigpending`. Quem esta num `sigtimedwait`
    conta como bloqueando o conjunto esperado (`sig_waiting`, o `real_blocked`
    do Linux).
  - Uma volta ao usuario entrega todos os sinais deliverables, empilhando um
    quadro por handler (antes era um so, e o `sigsuspend` perdia o segundo).
  - "Restart block" como o do Linux para esperas com prazo: `nanosleep`,
    `clock_nanosleep` relativo e `poll` com prazo devolvem
    `ERESTART_RESTARTBLOCK` e guardam o prazo absoluto na thread; sem handler,
    a syscall vira `restart_syscall` (219) e continua ate o mesmo prazo; com
    handler, vira `EINTR`. `poll` sem prazo e `clock_nanosleep` absoluto usam
    `ERESTARTNOHAND`. Antes um sinal pego por outra thread dava `EINTR` sem
    handler nenhum ter rodado.
  - O tempo de CPU nao anda mais para tras: a divisao entre usuario e sistema
    so cresce (como o `cputime_adjust` do Linux), e a leitura e feita sob a
    trava da fila de prontas.
  - `ITIMER_REAL` com intervalo rearma a partir do disparo anterior, nao do
    momento em que a thread do alarme olhou; essa thread agora dorme ate o
    proximo alarme em vez de acordar a cada 5 ms.
  - `wait4` com `WNOHANG` sem filho pronto nao zera mais o `rusage`; o tempo
    que a ultima thread gasta desmontando o processo conta para o pai;
    `sigaction` para ignorar limpa tambem o pendente de cada thread; o
    `sleep` da libc propria devolve os segundos que faltaram.
- Uma segunda revisao, so das correcoes, achou mais 8 problemas, e a
  regressao achou um panic; todos corrigidos:
  - O `kill` vindo do kernel (Ctrl+C no terminal manda `SIGINT` ao grupo)
    lia o pid de um processo atual que nao existe numa thread do kernel.
  - O "restart block" e apagado no `sigreturn` e quando um handler roda
    (como o Linux faz): sem isso, um `restart_syscall` salvo antes de um
    handler podia retomar com o prazo e os argumentos de uma espera feita
    dentro do handler.
  - A area de FPU do quadro fica acima dele, alinhada em 64 bytes (como no
    Linux), e o `sigreturn` a le pelo ponteiro `fpstate` do `ucontext`; o
    handler comeca com o FPU limpo e com DF, TF e RF zerados.
  - A divisao do tempo de CPU nunca subtrai alem do total (leituras de TSC de
    CPUs diferentes podiam dar um total menor que o anterior).
  - `SEGV_ACCERR` quando o endereco esta numa area mapeada (mesmo sem pagina
    ainda), `SEGV_MAPERR` so fora de qualquer area.

## v2.0.0-a9 (dash, lua e sbase)

- As fontes vem como estao (`third_party/dash` 0.5.13.5, `third_party/lua`
  5.4.9, `third_party/sbase` no commit `c546c3a`), com versao e SHA-256 em
  `third_party/LICENSES.md`. Nenhuma linha delas mudou.
- O `build.txt` dos programas da musl ganhou quatro diretivas: `gen
  <script>` (roda um gerador em Python antes de compilar, com a pasta
  `out/gen/<programa>` e o caminho do Zig), `gensrc <padrao>` (fontes que o
  gerador escreveu), `@gen`/`@root` dentro de `flag` (que agora aceita varias
  opcoes por linha) e `link <nomes>` (copia o binario tambem com esses nomes
  em `/bin`, se nenhum programa ja usa o nome). O `tools/musl_build.py` faz o
  mesmo no Linux e no CI.
- O `dash` precisa do que o `configure` e o `make` dele geram. O
  `tools/dash_gen.py` escreve o `config.h` (para a musl estatica: `SMALL`,
  sem libedit; `HAVE_WAIT3`, sem o qual o `jobs.c` nao compila; os nomes
  `*64` como sinonimos, porque a musl 1.2.4+ so os expoe com
  `_LARGEFILE64_SOURCE`), refaz em Python o que os scripts `mktokens` e
  `mkbuiltins` fazem (o `builtins.def` passa pelo pre-processador do Zig, para
  `JOBS` e `SMALL` valerem), escreve o `signames.c` com os sinais da musl
  (o `mksignames` leria os sinais do Windows), e compila e roda no proprio
  host os geradores em C do dash (`mknodes`, `mksyntax`, `mkinit`), que nao
  dependem do sistema. Um carimbo em `out/gen/dash` evita refazer tudo a cada
  build.
- O `sbase` vira um multicall (`sbase-box`), como o `make sbase-box` faria:
  o `tools/sbase_box.py` faz o que o script `mkbox` faz (renomeia o `main` de
  cada utilitario e gera a tabela). Sao 97 utilitarios em 476 KB; FAT nao tem
  links, entao os 29 mais uteis que nao colidem com programas da libc propria
  sao copias do binario em `/bin` (`basename`, `cut`, `expr`, `find`, `od`,
  `printf`, `seq`, `test`, `tr`, `xargs`...). Os que colidem (`cat`, `ls`,
  `sort`...) ficam acessiveis por `sbase-box <nome>` ate a 1.12 aposentar os
  proprios.
- `/bin/sh` continua sendo o shell proprio (historico, edicao de linha e
  jobs, que o dash sem libedit nao tem), mas ganhou `-c`: e o que o `system()`
  e o `popen()` da musl chamam (o `os.execute` do lua). No modo `-c` ele nao
  mexe em grupo de processos nem no terminal. O shell proprio nao trata aspas
  simples, entao a regressao testa o dash entrando nele (`dash`, os comandos,
  `exit 7`).
- O `rev` do sbase nesse commit esta quebrado no proprio upstream: o commit
  `3de61ef` ("rev, tail: replace hardcoded code by UTF8_POINT macro") inverteu
  a condicao, e o `rev` devolve a linha sem inverter. Como `third_party` fica
  como veio, o `rev` nao vai para `/bin` (continua em `sbase-box rev`); vale
  avisar o upstream ou subir o commit quando houver correcao.
- `faccessat2` (439) virou syscall de verdade: a musl a tenta antes do
  `faccessat` em `access`/`test -x`.
- Observado e deixado para o escalonador (fase 2/8): uma CPU ociosa so acorda
  no proximo tick quando entra trabalho na fila (nao ha IPI de "acorde"). No
  QEMU TCG isso atrasa a primeira execucao de um filho em uns 15 ms, e um
  `sleep 30 & kill $!` digitado na mesma linha do dash interativo perde a
  corrida (o filho ainda ignora `SIGTERM`, herdado do shell interativo, e o
  sinal ignorado e descartado, como no Linux). No Linux o filho quase sempre
  ganha porque comeca na hora em outra CPU.
- Depois de uma revisao, antes do commit:
  - O carimbo do `dash_gen.py` e um SHA-256 do conteudo de todas as
    entradas (fontes, cabecalhos e o proprio script), nao das datas: um
    tarball novo vem com datas antigas e passaria despercebido. Com o carimbo
    invalido, os geradores em C sao recompilados e todas as saidas conferidas.
  - O `sbase_box.py` apaga fontes geradas que nao pertencem mais a lista.
  - O `musl_build.py` sempre recopia os nomes de `link` (antes so se faltassem,
    e ficavam com o binario velho).
  - O Python e procurado por `tools/python.ps1`: `py -3`, depois `python3` e
    `python`, ignorando o atalho da Microsoft Store e conferindo a versao; o
    `setup.ps1` instala e confere o Python 3.
  - `sh -c`: comando longo demais da erro em vez de ser cortado; quebras de
    linha separam comandos; em modo `-c` nao imprime `[1] pid` nem "saiu com"
    e os comandos em segundo plano ignoram `SIGINT`/`SIGQUIT`, como o POSIX
    pede sem controle de jobs.
  - `faccessat2` com `AT_EMPTY_PATH` e `AT_FDCWD` confere o diretorio atual em
    vez de dar `EBADF`.

## v2.0.0-a10 (Doom)

- Doom pelo `doomgeneric` (commit `e257a09`, GPL-2.0), com o IWAD do
  Freedoom 0.13.0 (`freedoom1.wad`, BSD-3-Clause). O motor entra como veio:
  sao as 80 fontes da lista `SRC_DOOM` do Makefile dele, sem as de outras
  plataformas e sem som (o `doomfeatures.h` deixa `FEATURE_SOUND` desligado).
  Os oito tipos de aviso que o motor gera com `-Wall -Wextra` sao desligados
  por nome no `build.txt` (e nao com `-w`, que esconderia avisos do nosso
  arquivo).
- O que e nosso e so a plataforma (`user/musl/doom/doomgeneric_orion.c`): as
  seis funcoes `DG_*`. O quadro de 640x400 (o jogo de 320x200 ampliado 2x pelo
  proprio doomgeneric) e ampliado de novo pela escala inteira que cabe no
  framebuffer (2x em 1280x800) e centralizado, convertendo cada pixel para as
  mascaras de cor que o `FBIOGET_INFO` informa. As teclas vem de
  `/dev/input`, lido com `poll` sem esperar (o driver nao tem `O_NONBLOCK`),
  e seguem o mapeamento da plataforma SDL do proprio doomgeneric: Ctrl atira,
  espaco usa, Shift corre, Alt faz strafe. A ABI dos dispositivos do Orion
  fica num cabecalho proprio (`orion_dev.h`), ja que a musl nao tem os
  cabecalhos da libc propria.
- `-orionquit <segundos>` (tirado do argv antes do doomgeneric ver) encerra o
  jogo depois desse tempo e imprime quantos quadros desenhou. E o que a
  regressao usa: roda a demo do titulo por 30 s, tira a captura no meio e
  confere que ela tem a cena do jogo (dezenas de cores numa amostra, coisa
  que uma tela preta ou o console nao tem). `-nogui` e obrigatorio: sem ele o
  `I_Error` tenta rodar o `zenity` pelo shell.
- O IWAD tem 28 MB e fica fora do git, como o Zig: `tools/freedoom.ps1` baixa
  o zip oficial uma vez, confere o SHA-256 do zip e do `freedoom1.wad`, e o
  `build.ps1` o copia para `/doom/freedoom1.wad` quando o `doom` existe.
- No QEMU TCG com 4 CPUs a demo roda a uns 38 quadros por segundo, no limite
  dos 35 Hz do jogo.
- `check.ps1` passou a entender varias opcoes por linha `flag` e os
  marcadores `@gen`/`@root`, como o `build.ps1`.
- Depois de uma revisao, antes do commit:
  - O driver de teclado nao mandava Shift, Ctrl, Alt e Caps para
    `/dev/input` (so atualizava os modificadores), e jogava fora Ctrl e Alt da
    direita. No Doom isso deixava sem atirar, correr e strafe. Agora todas as
    teclas viram evento, e o Ctrl/Alt da direita tambem contam como
    modificador para o terminal.
  - Teclas estendidas chegam como o scancode simples com a flag
    `INPUT_MOD_EXTENDED` (a seta para cima e 0x48 + flag), nao como 0x148; a
    plataforma juntava errado e as setas nao funcionavam. As setas do teclado
    numerico tambem valem.
  - A tecla do Doom vem de uma tabela fixa por scancode, sem Shift: com o
    ASCII do evento, apertar `-` e soltar com Shift apertado mandava `_` na
    soltura e a tecla ficava presa. Scroll Lock e a pausa (a tecla Pause manda
    uma sequencia E1 que o driver ainda nao trata).
  - Mascaras de cor com mais de 8 bits por canal sao tratadas.
  - Linux/CI: `tools/freedoom.py` baixa e confere o WAD como o
    `freedoom.ps1`; o caminho das capturas no `qrun.py` aceita a barra do
    Windows que vem do `regressao.ps1`. Sem rede, o build avisa e segue sem
    IWAD em vez de falhar inteiro.
  - `__pycache__` saiu do git.

## v2.0.0-a11 (tcc dentro do Orion)

- TinyCC do ramo `mob` (commit `e23dabe`, 0.9.28rc), nao a 0.9.27 de 2017:
  a libc do Zig usa relocacao TLS (`R_X86_64_TPOFF32`) que a 0.9.27 nao liga,
  e o `mob` ja traz a correcao de ligacao estatica com a musl. O `mob` e um
  ramo de escrita publica, por isso o commit e o SHA-256 do snapshot ficam
  fixos em `third_party/LICENSES.md`.
- O tcc compila como um arquivo so (`tcc.c` inclui o resto). O `config.h`
  sai de `tools/tcc_gen.py`: alvo x86_64, `CONFIG_TCC_STATIC` (sem `dlopen`),
  `-static` como padrao (nao ha carregador dinamico no Orion) e
  `CONFIG_TCC_PREDEFS 0`, que faz o tcc ler o `tccdefs.h` do disco em vez de
  embuti-lo (evita gerar o `tccdefs_.h` com o `c2str`).
- O que o tcc precisa dentro do sistema (3,3 MB) e montado pelo mesmo
  gerador em `out/gen/tcc/sysroot` e copiado para o ESP pela diretiva nova
  `install` do `build.txt`: `/lib/tcc/include` (cabecalhos do tcc),
  `/lib/tcc/libtcc1.a` (compilada com o Zig, `-fno-pic`), `/usr/include` (os
  cabecalhos da musl que o Zig traz: `generic-musl` e por cima
  `x86_64-linux-musl`) e `/usr/lib` (`crt1.o`, `libc.a` e bibliotecas
  vazias para `-lm`, `-lpthread`...).
- A `libc.a` do Zig 0.16 nao basta sozinha: `memcpy`, `strlen`, as funcoes de
  matematica e outras foram para a `libzigc` e a `compiler_rt` do Zig. O
  gerador liga um programa com `--verbose-link`, acha as tres no cache e junta
  os objetos das duas ultimas na `libc.a` que vai para o Orion. O Zig tambem
  nao gera `crti.o`/`crtn.o`; a musl so os usa para `_init`/`_fini` fracos,
  entao vao objetos vazios.
- `mremap` (25) virou syscall de verdade, porque o `realloc` da musl o usa
  para blocos grandes (o tcc mostrou o aviso). Memoria anonima privada:
  encolher desfaz o fim; crescer usa o espaco logo depois se estiver livre;
  com `MREMAP_MAYMOVE` cria a area nova e muda as entradas de pagina de lugar
  (as paginas nao sao copiadas nem liberadas), com shootdown de TLB no fim.
  `MREMAP_FIXED` e areas de arquivo ou compartilhadas ficam para depois.
- O executavel que o tcc gera tem segmentos que nao comecam em fronteira de
  pagina e nao tem `PT_PHDR`; o carregador do Orion ja tratava os dois casos
  (o `AT_PHDR` tem o fallback da 1.1) e o binario roda sem mudanca.
- A regressao entra no dash, compila `/lib/tcc/hello.c` e um programa escrito
  na hora com `printf`, roda os dois e confere a saida e o codigo de saida.
- Depois de uma revisao, antes do commit:
  - `mremap` divide a area antes de mover qualquer pagina, entao a remocao
    da area velha no fim nao precisa alocar e nao falha no meio; os erros do
    `vma_unmap` sao devolvidos (antes um encolhimento sem memoria dizia que
    tinha dado certo); o shootdown so acontece quando alguma pagina saiu do
    lugar.
  - `tcc_gen.py` acha os arquivos do Zig na saida do `--verbose-link` mesmo
    com espacos no caminho, pega o diretorio de bibliotecas pelo `zig env` e
    falha com mensagem clara se os cabecalhos da musl nao vierem.
  - `install` espelha a origem: guarda a lista do que copiou e apaga do ESP o
    que saiu da origem.
  - Fica pendente: um `mremap` que precise de tabelas de pagina novas sem
    memoria livre ainda entra em panico, como a falta de pagina (etapa 8.4); e
    um `mremap` de faixa que atravessa duas areas vizinhas iguais da `EFAULT`
    (o Orion nao funde areas vizinhas; o Linux funde).

## v2.0.0-a12 (programas proprios na musl, libc propria aposentada)

- Os 53 programas de `user/progs` foram para `user/musl` e compilam contra a
  musl; `user/lib` (a libc propria, 1313 linhas) e o `user/linker.ld` sairam.
  A conversao foi feita por seis subagentes em paralelo, um lote de programas
  cada (shell, wm, testes de syscall, testes de processo e dispositivo, rede e
  sistema, utilitarios), com a regra de manter cada texto que a regressao
  confere; a integracao, a regressao e a revisao foram feitas depois.
- O que e do Orion e nao existe no Linux fica num cabecalho so,
  `user/include/orion.h`, que todo programa da musl enxerga: framebuffer e
  `/dev/input`, `sysinfo_orion`/`proclist_orion` (syscalls proprias 1002 e
  1003), `orion_openpty` (o `openpty` da musl abriria `/dev/ptmx`, que o Orion
  nao tem), os ioctls de rede e `orion_inet_aton`/`orion_inet_ntoa` (IP em
  ordem de host, como a libc propria fazia).
- As mensagens de erro continuam em portugues: `orion_strerror` e
  `orion_perror` tem a tabela da libc propria (a musl so tem ingles). O `sh`
  e o `kill` guardam a tabela de nomes de sinais em portugues.
- Diferencas da musl tratadas na conversao: o `sh` e o `wm` deixam o stdout
  sem buffer, como a libc propria (a ordem entre `printf` e `write` direto no
  terminal importa no editor de linha); os outros seguem o padrao da musl
  (linha no terminal, bloco em pipe e arquivo), entao, redirecionados para o
  mesmo arquivo, saida e erro podem aparecer em outra ordem. `fflush` antes de
  `fork`. O `edit` refaz o modo cru da libc propria (so `ICANON` e `ECHO`),
  porque o `cfmakeraw` da musl tambem desligaria o Ctrl+C. Testes que
  conferem o kernel (`badptr`, `sctest`, `filetest`, `memtest`, `fputest`)
  chamam as syscalls cruas, para a musl nao validar nem emular antes.
- Depois de uma revisao, antes do commit: dois testes do `filetest`
  (`open` com `O_CLOEXEC` e `F_DUPFD_CLOEXEC`) passaram a usar syscall crua,
  porque a musl emula os dois sozinha e o teste passaria com o kernel
  quebrado; o `forktest` ganhou um teste do heap do `brk` atraves do `fork`
  (o `malloc` da musl usa `mmap`, e o teste de heap antigo deixou de exercitar
  o `brk`); mudar o `orion.h` recompila os programas que o usam.

## v2.1.0-a1 (VFS com vnodes e montagens)

- O VFS antigo resolvia o caminho inteiro a cada chamada e devolvia uma copia
  do no (`struct vfs_node`) que cada `struct file` guardava. Dois descritores
  do mesmo arquivo tinham tamanhos diferentes depois de uma escrita, `/mnt` e
  `/dev` eram prefixos fixos no codigo e `..` era resolvido como texto. Agora
  existe um `struct vnode` por arquivo vivo, compartilhado por todos que o
  usam e contado por referencia (`vnode_get`/`vnode_put`), num cache de 256
  baldes indexado por (montagem, numero do inode). Um vnode sai do cache e e
  liberado quando a ultima referencia cai; nao ha lista de vnodes sem uso
  (isso fica para o cache de paginas da 2.4, que vai precisar segurar paginas
  de arquivos fechados).
- Travas: `vfs_lock` (spinlock) protege o cache, as contagens, o ponteiro
  `covered_by` e a contagem de vnodes por montagem; `mount_lock` (mutex)
  serializa `mount`, `umount` e `sync`; cada montagem tem `ns_lock` (mutex)
  que serializa criar, apagar e renomear naquela montagem, de modo que a
  checagem "o nome existe?" e a mudanca sao uma coisa so. A ordem e
  `mount_lock` -> `ns_lock` -> trava do sistema de arquivos -> `vfs_lock`, e
  `p->tlock` -> `vfs_lock`. Consultas de nome nao pegam `ns_lock` (o FAT
  serializa as proprias). `vnode_put` pode liberar os clusters de um arquivo
  apagado e por isso nunca e chamado com a trava do FAT nem com spinlock.
- Montagens: `struct mount` tem a raiz, o vnode coberto e a lista global. O
  vnode coberto ganha `covered_by`; o caminho e resolvido componente a
  componente e, depois de cada passo, desce para a raiz da montagem que
  cobre o vnode. `..` na raiz de uma montagem sobe para o vnode coberto e so
  entao pergunta ao sistema de arquivos; `..` na raiz global fica nela. O
  `umount` so acontece se a raiz da montagem tiver exatamente as referencias
  da propria montagem e de quem pediu, e se nao houver outro vnode vivo da
  montagem (arquivo aberto, cwd, area de `mmap` ou outra montagem dentro);
  senao `EBUSY`. Um diretorio aceita uma montagem so (`EBUSY` na segunda),
  e um disco nao monta duas vezes, nem a particao junto com o disco inteiro.
- `mount`/`umount2` sao as syscalls 165/166 do Linux; os tipos sao `vfat` e
  `devfs`. A raiz e montada no boot como antes (a primeira particao FAT), o
  devfs vai para `/dev` e o segundo disco, se tiver FAT, para `/mnt` (o
  comando `mount` do shell do kernel continua fazendo isso). `/bin/mount` e
  `/bin/umount` sao programas novos; `mount` sem argumentos le
  `/proc/mounts`, que chega na 2.3. `build.ps1` e o `Makefile` criam `dev` e
  `mnt` na raiz da ESP para servirem de ponto de montagem; se faltar, o
  kernel cria no boot.
- O processo guarda o vnode do diretorio atual alem do texto. Caminho
  relativo comeca nesse vnode, e o `openat` com descritor comeca no vnode do
  descritor, entao renomear o diretorio atual ou apaga-lo nao confunde mais a
  resolucao (criar arquivo num cwd apagado da `ENOENT`, como no Linux). O
  texto do `getcwd` continua sendo o caminho normalizado do `chdir`; sem links
  simbolicos (2.7) os dois sempre concordam. `fchdir` (81) entrou junto.
- FAT: o numero do inode de um diretorio e o primeiro cluster (raiz = 1) e o
  de um arquivo e a posicao da entrada no diretorio pai, porque arquivo vazio
  nao tem cluster e o primeiro cluster muda num `truncate` para zero seguido
  de escrita. O `rename` move a entrada e troca a chave do vnode no cache. O
  `unlink` tira a entrada e marca o vnode como apagado; os clusters so voltam
  ao FAT quando o ultimo descritor fecha, entao ler e escrever num arquivo
  apagado e ainda aberto funciona e criar outro com o mesmo nome nao mistura
  os dois. `rename` agora atravessa diretorios (corrigindo o `..` do
  diretorio movido e recusando mover um diretorio para dentro dele mesmo) e
  substitui o destino existente; no FAT isso e apagar o destino e depois
  renomear, sem ser atomico. Escrever depois do fim e `ftruncate` para cima
  preenchem o intervalo com zeros (antes sobrava lixo do fim do ultimo
  cluster). Todas as operacoes do FAT pegam a trava do FAT do comeco ao fim;
  antes o `create` consultava o nome e escrevia a entrada em dois passos.
- Corrigido na mudanca: escrever num arquivo vazio a partir de um deslocamento
  maior que um cluster gravava o cluster errado como o primeiro da cadeia.
- `pread64`/`pwrite64` (17/18) e `truncate` (76) entraram; o `vfstest` usa os
  dois primeiros e eles faltavam.
- `st_dev` passou a ser o numero da montagem (raiz 1, `/dev` 2...) e `st_ino`
  e o numero do vnode, o mesmo que o `getdents64` devolve.
- Teste: `vfstest` (montagens e `..`, devfs montado num diretorio,
  `EBUSY`, vnode compartilhado, arquivo apagado e aberto, `rename` em todas as
  formas, cwd apagado, buracos e `truncate`, erros de caminho). Na sequencia
  A roda com `-n` (sem escrever na raiz: o vvfat do QEMU as vezes deixa no
  hospedeiro o que o Orion apagou); na C roda inteiro em `/mnt`, seguido de
  `umount` ocupado, `mount -r` com `EROFS` e remontagem.
- Revisao adversarial antes do commit (4 revisores, cada achado conferido por
  outro), corrigido:
  - `..` a partir de um diretorio apagado lia a entrada `..` dos clusters
    ainda vivos do apagado e recriava um vnode para o pai, cujos clusters ja
    podiam ter sido liberados: criar arquivo ali escrevia em cluster livre.
    Agora `..` num diretorio apagado da `ENOENT`.
  - `rename` por cima de um destino apagava o destino antes de saber se o
    nome novo era valido e se havia espaco; uma falha perdia o arquivo. O
    `rename` do sistema de arquivos recebe o destino e faz tudo sob a trava
    do FAT: grava a entrada nova, so entao tira a do destino e a da origem.
  - `O_APPEND` lia o tamanho fora da trava do FAT, e dois processos
    escrevendo no fim do mesmo arquivo se sobrescreviam. A escrita recebe a
    flag e o deslocamento e decide o fim do arquivo dentro da trava.
  - O `mutex` de cada montagem entrava no registro global de mutexes e nao
    saia no `umount`: o registro apontava para memoria liberada (a regressao
    viu a tabela do comando `mutex` corrompida).
  - Volumes FAT forjados: setor diferente do dispositivo, tabela FAT menor
    que o numero de clusters ou volume maior que o disco agora sao recusados
    (antes liam e escreviam fora do buffer da FAT no heap).
  - `rename` so de maiusculas e minusculas devolvia sucesso sem fazer nada
    (a busca do FAT nao diferencia).
  - A posicao do `getdents64` era a contagem de entradas vivas; apagar
    durante a leitura do diretorio pulava entradas (`rm -r` em diretorio
    grande deixava arquivos). Agora a posicao e um cookie do sistema de
    arquivos (no FAT, o deslocamento da proxima entrada), devolvido em `d_off`,
    e o `getdents64` enche o buffer numa passada so.
  - Depois de renomear o diretorio atual, o texto do `getcwd` ficava velho. O
    `getcwd` confere se o texto ainda leva ao vnode do cwd e, se nao, refaz o
    caminho subindo por `..` e procurando o nome pelo inode (`vfs_getpath`);
    cwd apagado da `ENOENT`, como no Linux.
  - Menores: preencher um buraco grande andava a cadeia de clusters desde o
    comeco para cada cluster; diretorios FAT limitados a 65536 entradas (as
    chaves de arquivo dependem disso); caminho relativo longo demais era
    truncado em vez de dar `ENAMETOOLONG`; `errno` de `O_CREAT` em
    diretorio, `unlink(".")`, `rename` com `..`, `access(W_OK)` em diretorio
    de montagem so leitura e `execve("")`.
- Segunda rodada de revisao (sobre as correcoes da primeira), corrigido:
  a checagem de "diretorio apagado" no `..` era feita antes de pegar a trava
  do FAT e ainda deixava a corrida aberta entre duas CPUs (agora e conferida
  dentro da trava, para qualquer nome); o `getcwd` que refazia o texto podia
  desfazer um `chdir` feito ao mesmo tempo por outra thread (agora so troca o
  texto se o vnode do cwd for o mesmo); as protecoes contra laco do
  `rename` e do `getcwd` aceitavam o resultado quando o limite estourava;
  uma falha de disco no meio do `rename` por cima ainda podia perder o
  destino (a ordem virou: entrada nova, tira a origem, tira o destino, e a
  origem volta se a ultima etapa falhar); `fchdir` para um diretorio ja
  apagado falhava (o Linux aceita; o `getcwd` e que da `ENOENT`); `read` e
  `write` em arquivo regular ignoravam o modo de abertura (agora `EBADF`);
  `O_APPEND` era atomico so por pedaco interno de 64 KiB (agora uma trava por
  vnode segura o `write`/`writev` inteiro); o `st_ino` de arquivo FAT mudava
  no `rename`. O vnode passou a ter `key` (a chave do cache, que no FAT e a
  posicao da entrada) separada de `ino` (o que o `stat` mostra, fixo enquanto
  o vnode vive); um vnode renomeado ou apagado e aberto entra na lista de
  "deslocados" da montagem, e um vnode novo cuja chave coincida com o `ino`
  de um deslocado recebe um numero novo. O `d_ino` do `getdents64` usa o
  `ino` do vnode vivo; sem vnode vivo, e a chave (pode divergir do `stat` so
  nesse caso raro, de nome novo no lugar de um arquivo renomeado e aberto).
  O `mkfs` do Orion podia gerar uma FAT um setor menor que o necessario, que a
  checagem nova do `fat_mount` recusaria.
- Observado e deixado para o escalonador: o `mutex` nao e justo (quem solta
  pode pegar de novo antes de o acordado rodar), e sem IPI para acordar CPU
  ociosa o acordado demora; numa sequencia de escritas pequenas o `bdflush`
  chegou a esperar 10 s pela trava do FAT (so atrasa o descarregamento).
  Passar a trava direto para quem espera resolveria a fome, mas cada troca
  passaria a custar a latencia de acordar a outra CPU; fica para quando
  houver IPI de acordar.
- Tambem observado: na partida do estresse (64 filhos fazendo `exec` de
  `/bin/true` ao mesmo tempo) processos esperam mais de 5 s pela trava do FAT
  da raiz, porque cada `exec` le o ELF inteiro com a trava pega e o TCG do
  QEMU e lento. O padrao de trava e o mesmo de antes da 2.1; o cache de
  paginas da 2.4 tira a leitura de dentro da trava.
- Terceira rodada (sobre as correcoes da segunda), corrigido: o `ino`
  alternativo de um vnode cuja chave coincide com a de um deslocado vinha de
  um contador e mudava a cada `stat`; agora e uma funcao da chave (mistura de
  bits com o bit 63 ligado), a mesma que o `getdents64` usa quando nao ha vnode
  vivo; o vnode sai da lista de deslocados antes de liberar os clusters (senao
  um `mkdir` na janela pegava o cluster e um `ino` que o `readdir` nao
  mostrava); `chdir` e `execve` relativos nao falham mais so porque o texto do
  cwd nao coube; no `rename`, a origem e salva inteira (entrada curta e as de
  nome longo) e volta inteira se uma etapa falhar, e o `..` do diretorio
  movido e gravado antes e desfeito na falha; `dir_add_entry` nao descarrega
  mais a FAT (uma falha ali dizia "nao gravou" com a entrada ja no disco).
- Quarta rodada: a volta atras do `rename` podia gravar lixo da pilha no `..`
  de um diretorio movido quando a falha vinha antes de o `..` ser trocado
  (agora so desfaz o que foi feito); e `rename` por cima de um arquivo numa
  raiz FAT12/16 cheia dava `ENOSPC`, porque a entrada nova era criada antes
  de sair a antiga. Agora, sem espaco, o destino (ou a propria origem, no
  `rename` so de maiusculas) sai primeiro, com as entradas salvas para voltar
  se algo falhar. A rodada nao achou outros problemas novos; a lista de
  deslocados percorrida com a trava global foi considerada aceitavel (so
  cresce com arquivos apagados ou renomeados e ainda abertos).
