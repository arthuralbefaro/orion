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

## v2.1.0-a2 (tmpfs)

- `tmpfs` e o terceiro tipo da VFS: arvore de nos na memoria do kernel, os
  dados de cada arquivo num vetor de paginas fisicas (uma por 4 KiB, zero =
  buraco que le zeros). Montado no boot em `/tmp` e `/run`, e pelo `mount`
  (`mount -t tmpfs tmpfs /x`, com `size=` em bytes, `k`, `m`, `g` ou `%` da
  RAM; o padrao e metade da RAM, como no Linux). Passar do limite da
  `ENOSPC`.
- Um no vive enquanto estiver ligado a um diretorio ou enquanto algum vnode
  apontar para ele (contador `vrefs`, mexido so com o mutex da montagem). Assim
  arquivo apagado e ainda aberto continua com as paginas, e elas voltam ao
  alocador no ultimo `close`, nao no `unlink`. O vnode e liberado quando a
  ultima referencia cai e o no fica: abrir de novo cria outro vnode para o
  mesmo no.
- A posicao do `getdents64` no tmpfs e um numero de sequencia dado a cada
  entrada quando ela entra no diretorio (cresce sempre), entao apagar ou
  renomear durante a leitura nao pula nem repete entradas antigas.
- O criterio da etapa ("1000 arquivos criados e apagados sem vazar pagina") e
  o `tmptest`: depois de uma rodada de aquecimento, uma segunda rodada de 1000
  arquivos de 3000 bytes tem que devolver exatamente as paginas livres
  (tolerancia de 4 paginas para o heap do kernel). Na pratica volta igual.
- `vfstest /tmp` roda a mesma bateria da VFS no tmpfs (a parte de raiz FAT
  cheia se desliga sozinha quando o diretorio nao enche).
- Revisao adversarial, corrigido: o vetor de paginas de cada arquivo tinha o
  tamanho do arquivo e ficava fora do limite `size=` (um `ftruncate` grande
  prendia centenas de MiB do heap do kernel); virou uma arvore radix de ate 3
  niveis de 512 entradas, de altura variavel (altura 0: o proprio no aponta
  para a unica pagina de dados; cresce pondo a raiz antiga na entrada 0 de
  uma tabela nova), com cada pagina de tabela contada no limite. Arquivo
  esparso de 256 GiB custa so as tabelas do que foi escrito; o maior
  arquivo e 512 GiB. O `umount` liberava a arvore com recursao do tamanho da
  profundidade dos diretorios (estourava a pilha de 16 KiB com algumas
  centenas de niveis); agora e iterativo. `rename` no mesmo diretorio dava
  posicao nova a entrada, e quem renomeava enquanto lia o diretorio nunca
  chegava ao fim. Entrou `nr_inodes=` (padrao: metade das paginas da RAM,
  como no Linux), `size=0` e ilimitado, opcao desconhecida da `EINVAL`, e as
  opcoes do `mount` so sao copiadas para o tmpfs (ate 256 bytes).
- Segunda rodada no tmpfs: os dois tmpfs do boot (cada um com metade da RAM)
  somados podiam pegar toda a memoria fisica, e o kernel entra em panico
  quando falta pagina para tabela de paginas (pendencia da 8.4). Agora o
  tmpfs recusa pagina nova (`ENOSPC`) quando a memoria livre cai abaixo de
  1/16 da RAM (no minimo 2048 paginas), qualquer que seja o `size=`. E uma
  escrita que falhava no meio deixava tabelas da radix alem do fim do
  arquivo, contadas no limite ate o `unlink`; escrita curta e `truncate`
  agora liberam o que passa do fim.

## v2.1.0-a3 (procfs)

- `proc` e o quarto tipo da VFS, montado so leitura em `/proc` no boot. Nada
  fica guardado: o vnode carrega na chave o pid, o tipo de entrada e o numero
  do descritor, e o conteudo de cada arquivo e gerado inteiro na hora do
  `open` (num buffer que vive ate o `close`). Assim quem le em varios
  pedacos ve um retrato so, e `lseek` funciona. Tamanho no `stat` e 0, como
  no Linux.
- Entradas: `/proc/<pid>/{status,cmdline,maps,stat,fd/}`, `self` (um
  diretorio que aponta para o processo que pergunta; vira link simbolico na
  2.7), `meminfo`, `cpuinfo`, `uptime`, `loadavg`, `mounts` e `version`.
  `fd/N` le o caminho do arquivo aberto (o caminho guardado no `open`;
  `/dev/tty`, `pipe:` etc. para o resto), ate os links simbolicos.
  `meminfo` tem os campos do Linux e tres do Orion (`KernelHeap`,
  `CowShared`, `CowFaults`/`CowCopies`).
- Ler outro processo exigiu duas mudancas no nucleo: o processo ganhou um
  contador de pinos (`process_get`/`process_put`), e a liberacao do processo,
  se houver pino, fica para o ultimo `put` (quem chega a zero primeiro, o
  `free` ou o `put`, faz a liberacao, com troca atomica de uma flag). E a
  lista de VMAs passou a mudar sempre com `mm_lock`: o `exec` (que troca o
  espaco de enderecamento) e a saida do processo nao pegavam a trava porque
  so havia uma thread; agora o `/proc/<pid>/maps` de outro processo le a
  lista com a mesma trava.
- O processo guarda a linha de comando (ate 4 KiB, argumentos separados por
  NUL, trocada sob `p->tlock` no `exec`) e o caminho do executavel (o nome
  das areas da imagem no `maps`).
- As syscalls 1002 (`sysinfo`) e 1003 (`proclist`) sairam. O `orion.h` tem
  `orion_sysinfo`, `orion_proclist` e `orion_free_pages`, que leem do
  `/proc`; `ps`, `top`, `free`, `uptime`, a janela de processos do `wm`, o
  `memtest`, o `limtest` e o `tmptest` usam essas funcoes. Os testes de
  vazamento medem so `MemFree` (contar os processos lista `/proc`, e o buffer
  grande do `getdents64` tem liberacao adiada no heap, o que da ruido de
  algumas paginas).
- Teste: `proctest` (status, cmdline, maps acompanhando `mmap`/`mprotect`/
  `munmap`, fd, zumbi, pid sumindo depois do `wait`, `maps` aberto antes de o
  processo morrer, `EROFS`); o criterio da etapa e `cat /proc/self/maps` na
  regressao.
- Revisao adversarial, corrigido: na saida do processo os descritores eram
  soltos antes de a entrada da tabela ser zerada e sem `p->tlock`, e o
  `/proc/<pid>/fd/N` de um processo saindo lia um `struct file` ja liberado
  (agora a saida e a falha do `fork` usam o mesmo `process_fd_close`: tira da
  tabela com a trava, solta depois); um filho ja colhido mas ainda preso pelo
  `/proc` guardava o ponteiro para o pai, que podia ja ter sido liberado (o
  `unregister` zera o ponteiro); o caminho do executavel e trocado junto com
  as VMAs, sob `mm_lock`. O gancho `open` dos sistemas de arquivos passou a
  devolver o erro (`int open(v, flags, &file)`): abrir um arquivo do `/proc`
  de um processo que acabou de sair da `ENOENT` em vez de `ENODEV`. E o
  `orion_proclist` devolve a lista do mais novo para o mais velho, como a
  syscall antiga, para o `ps`, o `top` e o `wm` nao mudarem de ordem.
- Segunda rodada: com mais de 256 processos o `orion_proclist` ficava com os
  mais velhos (le todo o `/proc` e guarda os mais novos agora); e um nome de
  executavel com quebra de linha forjava campos no `/proc/<pid>/status`
  (o `exec` troca controles e parenteses do nome por `?`).

## v2.1.0-a4 (cache de paginas unificado)

- Cada vnode tem um cache de paginas (`struct pcache`: arvore radix de ate 4
  niveis de 512 entradas com paginas fisicas, protegida por um spinlock do
  proprio vnode). Sistema de arquivos que oferece `readpage` (por enquanto o
  FAT) passa a ser lido so pelo cache: `read`, o carregamento do ELF no `exec`
  e o `mmap` de arquivo usam as mesmas paginas. tmpfs, devfs e proc nao usam
  (o tmpfs ja guarda as paginas dele; os outros nao tem dado em disco).
- Referencias: o cache tem a referencia base de cada pagina; quem le copia com
  uma referencia extra e solta depois; o `mmap` mapeia a propria pagina do
  cache com uma referencia extra, sempre com `PTE_COW` e sem escrita (mesmo
  em area so leitura, para um `mprotect` posterior nao liberar escrita na
  pagina do cache). Escrever num `MAP_PRIVATE` copia a pagina como no `fork`.
  Tirar a pagina do cache (truncate, despejo) so solta a referencia base: um
  processo que ainda a mapeia continua com ela.
- Escrita e write-through: o FAT grava no disco como antes e depois o VFS
  copia o trecho para as paginas que estiverem no cache; `truncate` tira as
  paginas alem do fim e zera o resto da ultima. Um contador de geracao por
  vnode impede que uma leitura do disco que comecou antes de uma escrita
  ponha uma pagina velha no cache (ela tenta de novo).
- Para a segunda leitura nao ir ao disco o cache precisa sobreviver ao
  `close`: vnode com paginas no cache, quando a ultima referencia cai, vai
  para uma lista LRU de inativos em vez de ser liberado (continua no cache de
  vnodes; achar de novo o reativa). Despejo pelo fim da LRU quando ha mais de
  512 inativos ou quando o cache passa de 1/4 da RAM (acima disso a leitura
  usa uma pagina que nao fica no cache). O `umount` despeja os inativos da
  montagem antes de conferir se esta ocupada. `MemAvailable` soma o cache, e
  `Cached` no `/proc/meminfo` mostra o tamanho.
- A abertura de `/bin/doom` ainda ia ao disco: diretorio FAT que nao e a raiz
  fixa era lido em clusters inteiros direto do disco (o cache de blocos so
  guarda leitura de setor unico). Clusters de diretorio agora passam pelo
  cache de blocos setor a setor (`blockdev_read_cached`).
- `/proc/diskstats` (formato do Linux, so leituras e escritas e setores por
  disco) mostra quantas vezes cada disco foi lido de verdade; o `pctest` le o
  arquivo duas vezes e confere que o contador nao mexeu na segunda, que o
  `mmap` ve o mesmo conteudo sem ir ao disco, e (com `-w` num diretorio
  gravavel) que `pwrite` aparece no `read` e num `mmap` ja feito e que
  `ftruncate` fica coerente.
- Revisao adversarial, corrigido: o contador de referencias de pagina do pmm
  e de 16 bits, e 65536 `mmap` da mesma pagina de um arquivo estouravam o
  contador e derrubavam o kernel (agora, perto do limite, o cache entrega uma
  copia particular). A atualizacao do cache depois de escrever e de truncar
  acontecia fora da trava do FAT: duas escritas podiam deixar o cache numa
  ordem e o disco em outra, quem lia via o tamanho novo antes dos bytes, e um
  erro no meio pulava a atualizacao. Agora o FAT atualiza o cache dentro da
  propria trava, no lugar exato em que muda dados e tamanho, e publica o
  tamanho depois dos dados. Uma leitura do disco que vinha curta (erro no
  meio da pagina) entrava no cache com zeros; agora da `EIO`. Faltava
  devolver memoria sob pressao: quando o alocador de paginas falha numa falta
  de pagina ou no proprio cache, os vnodes inativos mais antigos sao
  despejados e a alocacao tenta de novo; `MemAvailable` conta so o que e
  reaproveitavel. E o `umount` podia dar `EBUSY` a toa enquanto outra CPU
  despejava um vnode da montagem (agora espera o despejo terminar).
- O comando `mem` do shell do kernel mostra as paginas usadas sem o cache de
  paginas (e uma linha a parte com o tamanho do cache): a regressao confere
  que a memoria volta ao mesmo ponto no fim de cada sequencia, e o cache, que
  agora guarda os arquivos lidos, fazia essa conta parecer vazamento.
- Segunda rodada: limitar as referencias so no cache nao bastava, porque o
  `fork` de um processo com milhares de mapeamentos da mesma pagina ainda
  estourava o contador de 16 bits. O contador do pmm passou a ser de 32 bits
  (os comentarios do `pmm.c` sairam junto, pela regra do repositorio), e a
  copia particular perto do limite deixou de ser necessaria. O `readpage` do
  FAT media o tamanho e lia em duas posses da trava (um `truncate` no meio
  virava `EIO` falso); agora e uma so. A falta de COW tambem despeja cache
  inativo e tenta de novo, e o `fork` despeja cache quando a memoria livre
  cai abaixo de 1/16 da RAM.
- Terceira rodada: toda escrita invalidava o preenchimento em andamento de
  qualquer pagina do arquivo (o contador de geracao era do arquivo inteiro), e
  um arquivo que alguem esta acrescentando era relido ate 9 vezes por pagina
  sem nunca entrar no cache. O contador saiu: o `readpage` do FAT poe a pagina
  no cache (`pcache_install`) ainda com a trava do FAT, a mesma sob a qual as
  escritas atualizam o cache, entao preenchimento e escrita nunca se cruzam.
  E o tmpfs, ao chegar na reserva de memoria, primeiro despeja cache inativo
  antes de dar `ENOSPC` (senao o cache do FAT, contado como disponivel,
  roubava espaco do `/tmp`).
- Quarta rodada: quando arquivos abertos sozinhos ja enchiam o orcamento do
  cache, cada falta no cache esvaziava a LRU inteira (a condicao de parada
  contava paginas que a LRU nao pode devolver). O despejo agora so tira o
  suficiente para cobrir o excesso, e so roda se ha algo reaproveitavel.

## v2.1.0-a5 (ext2 so leitura, piloto em Rust)

- O leitor de ext2 e o primeiro codigo em Rust do kernel: `kernel/rust/ext2`,
  uma `staticlib` `no_std` compilada pelo `cargo` para `x86_64-unknown-none`
  (sem SSE, sem red zone) com `-C code-model=kernel -C relocation-model=static`
  e `-D warnings`, `panic = "abort"`. O `build.ps1` e o `Makefile` chamam o
  `cargo` e ligam o `.a` junto com os objetos de C; o `check.ps1` roda o
  `cargo clippy` com `-D warnings` e procura comentario nos `.rs`. A unica
  regra do clippy desligada e `missing_safety_doc`, que pede comentario de
  documentacao nas funcoes `unsafe` exportadas (comentario e proibido no
  repositorio); o contrato delas e este: os ponteiros vem do C validos pelo
  tempo da chamada, `fs` e o que o `ext2_mount` devolveu e so o
  `ext2_umount` o libera.
- Divisao: o Rust faz todo o trabalho com dados vindos do disco (superbloco,
  descritores de grupo, inodes, blocos diretos e indiretos simples, duplos e
  triplos, entradas de diretorio, links simbolicos rapidos e lentos), sempre
  conferindo limites, porque a imagem pode ser hostil. O C (`fs/ext2.c`) faz
  a ponte com a VFS: vnode por numero de inode, `readpage` para o cache de
  paginas, `readdir` com o deslocamento no diretorio como cookie, `readlink`.
  O acesso ao disco e um callback do C que le setores (metadados pelo cache
  de blocos, dados direto); a memoria do Rust vem do `kmalloc` (alocador
  global com alinhamento extra quando pedido) e um `panic` do Rust vira o
  `panic` do kernel com a mensagem.
- Suportado: revisao 0 e 1, blocos de 1, 2 e 4 KiB, varios grupos,
  `filetype`, `sparse_super`, `large_file`, arquivos esparsos (inclusive
  tabela indireta inteira faltando), so leitura (montado com `ro`).
  Recusado: `extents` e o resto do ext4, `64bit`, `meta_bg` e journal que
  precisa de recuperacao.
- `readlink`/`readlinkat` passaram a funcionar para links do ext2 (o
  caminho ainda nao segue links: 2.7).
- Imagens de teste: o Windows nao tem `mkfs.ext2`, entao `tools/mkext2.py`
  gera imagens rev 1 a partir de um diretorio ou de uma arvore de teste
  (arquivo de 5 MiB, esparso, esparso de 70 MiB com dado so no fim para
  forcar o indireto triplo em blocos de 1 KiB, 200 arquivos num diretorio,
  links rapido e lento, um ELF) com um `MANIFEST` de tamanhos e CRC32. A
  imagem foi conferida por um leitor independente: o 7-Zip extrai todos os
  arquivos e os CRC32 batem, com uma excecao do proprio 7-Zip (ele nao aceita
  ponteiro zero para uma tabela indireta inteira, o que o ext2 do Linux
  aceita; criando a tabela vazia ele le o mesmo arquivo).
- A regressao C gera duas imagens (blocos de 4 KiB e de 1 KiB com 4 grupos),
  liga como `sdc` e `sdd` (`test.ps1 -ExtraDisks`, e o `regress.py` faz o
  mesmo), monta em `/tmp` e roda o `ext2test` (todos os arquivos pelo CRC32,
  links, contagem de diretorio, fim do esparso de 70 MiB, `mmap`, `EROFS`,
  `..`, `st_dev`) e um binario direto do ext2.
- Avaliacao do piloto: build limpo do crate em 0,8 s e sem mudanca em
  0,13 s (sem dependencias; a `core`/`alloc` vem pronta com o alvo); o Rust
  ocupa uns 12 KB no kernel; o `llvm-symbolizer` acha arquivo e linha das
  funcoes Rust no `orion.elf` como acha as de C, e o `panic` chega ao log do
  kernel com a mensagem. O custo foi o alvo `x86_64-unknown-none` (um
  `rustup target add`) e o clippy brigando com a regra de nao comentar.
  Decisao: o Rust continua, nos lugares que o plano lista (TCP, USB,
  parsers de dados de fora).
- Revisao adversarial (a imagem e entrada hostil), corrigido: a tabela de
  descritores de grupo era alocada so com base no superbloco (ate o heap
  inteiro); agora os campos tem limites (blocos por grupo entre 256 e
  8 x tamanho do bloco, no maximo 65536 grupos) e o ultimo bloco que o
  superbloco declara precisa existir no dispositivo. Diretorio com tamanho
  absurdo fazia cada busca percorrer milhoes de blocos: acima de 64 MiB e
  recusado. Tamanho de arquivo de 64 bits sem limite estourava o `kmalloc` no
  `exec`: agora nao passa do que o mapa de blocos consegue enderecar (nem de
  `i64::MAX`), e os bits altos so valem para arquivo regular. Link rapido com
  bloco de atributos estendidos era lido como lento (o tamanho do bloco de
  atributos e descontado, como no Linux). Blocos de 64 KiB eram aceitos e
  depois as entradas de diretorio deles davam `EIO`: so ate 4 KiB agora.
  Entrada de diretorio sem nome ou com `/` ou NUL e ignorada. Alocacoes do
  Rust que nao podiam falhar (e virariam `panic` do kernel sem memoria)
  passaram a devolver `ENOMEM`. O `getdents64` mostra `DT_LNK`, `DT_FIFO` e
  `DT_SOCK`. Nas ferramentas: o `setup.ps1` tinha duas linhas corrompidas
  pela edicao (barra invertida virando caractere de controle), o
  `check.ps1` dizia "nenhum erro" sem conseguir rodar o `cargo`, o
  `build.ps1` apagava o `RUSTFLAGS` de quem chamou, e a regressao nao
  conferia se o `mkext2` deu certo.
- Teste no hospedeiro: o mesmo crate compila com `std` para o Windows (ou o
  Linux do CI) quando roda `cargo test` (`#![cfg_attr(not(test), no_std)]`,
  o alocador e o `panic` do kernel so entram fora do teste). O teste gera as
  imagens com o `mkext2.py`, monta e percorre tudo (diretorios, busca,
  leitura no comeco, meio e fim, links), e depois faz o mesmo com 800 imagens
  com bytes trocados ao acaso nos metadados, em build de depuracao (estouro
  de inteiro vira `panic`, que reprova o teste). O `check.ps1` roda isso. E
  uma vantagem concreta do Rust no piloto: o parser de dados hostis e testado
  fora do kernel, sem QEMU.
- Segunda rodada: limitar o tamanho declarado do diretorio nao limitava o
  trabalho (todos os blocos podiam apontar para o mesmo bloco cheio de
  entradas); agora cada passada no diretorio para com `EIO` depois de 65536
  entradas e o limite de tamanho caiu para 16 MiB. Um `..` que devolve o
  proprio diretorio fazia o `getcwd`/`fchdir` repetir a busca ate encher o
  caminho; agora para com `ENOENT`. `exec` (e o `vfs_read_all` do shell do
  kernel) de arquivo maior que 64 MiB e recusado antes de alocar: um arquivo
  esparso de terabytes custava so alguns blocos na imagem e fazia o kernel
  tentar alocar o heap inteiro com interrupcoes desligadas. Os ponteiros de
  tabela indireta do inode tambem sao conferidos contra o tamanho do sistema
  de arquivos. O fuzz ficou mais forte: muda bytes no comeco e em qualquer
  ponto da imagem, grava valores de fronteira em campos de 16 e 32 bits, usa
  tambem uma imagem de 4 grupos com inodes espalhados, chama a interface
  exportada (cookies do `readdir` em lotes e valores absurdos, `read` em
  deslocamentos extremos, `readlink` com buffers de 1, 59, 60 e 61 bytes),
  sorteia a semente a cada rodada (`ORION_FUZZ_SEED` repete uma) e reprova a
  imagem que passar de 400 mil leituras de disco (laco sem fim vira falha em
  vez de travar o `check.ps1`).

## v2.1.0-a6 (ext2 com escrita)

- O ext2 monta em leitura e escrita por padrao (`-r` ou `-o ro` para so
  leitura). Cai sozinho para so leitura quando o dispositivo nao escreve, os
  descritores de grupo nao fecham, ha `ro_compat` alem de `sparse_super` e
  `large_file`, o superbloco tem o bit de erro ou o primeiro inode livre e
  absurdo. Montado com escrita, o estado perde o bit `VALID` no disco e so o
  ganha de volta no `umount`, como no Linux: queda no meio deixa o `e2fsck`
  avisado.
- Parte 1: `write` e `truncate` em arquivo que ja existe. Bloco novo nasce
  zerado so quando e tabela indireta ou fica parcialmente escrito; tabela
  indireta que esvazia no `truncate` e devolvida. Bitmaps sao gravados na
  hora; os contadores livres do superbloco e dos grupos ficam em memoria e
  vao para o disco no `sync` periodico e no `umount`.
- Parte 2: `create`, `mkdir`, `unlink`, `rmdir` e `rename`. Inode de
  diretorio vai para o grupo com mais blocos livres, o de arquivo para o
  grupo do pai, como no ext2 do Linux, e o contador de diretorios por grupo e
  mantido (o `e2fsck` confere). Entrada nova entra na primeira folga que
  couber; quando nao cabe, o diretorio ganha um bloco (inclusive pelo
  indireto). Entrada apagada e somada a anterior no mesmo bloco; a primeira
  do bloco so zera o inode. Nada e compactado, entao o cookie do `readdir`
  continua valido entre chamadas.
- Arquivo apagado ainda aberto: o `unlink` tira o nome e baixa o
  `links_count`; com zero, o vnode sai do cache marcado como apagado, e o
  inode e os blocos so sao liberados no `release`, quando o ultimo `close`
  solta o vnode. O numero do inode nao e reusado antes disso, porque o bit
  dele continua marcado. Inode com mais de um link (imagem feita por outro
  sistema) so perde o nome.
- Diretorio indexado (`dir_index`, flag `INDEX_FL`, criado pelo Linux ou pelo
  `e2fsck -D`) e mexido como lista linear e perde a flag, como faz o ext2 do
  Linux: a raiz do indice fica escondida na folga do `..` e a leitura linear
  continua certa. Bloco de atributos estendidos do inode apagado tem o
  contador de referencias decrementado e so e liberado quando chega a zero.
  Dispositivo, FIFO e link rapido nao tem blocos para liberar (o `block[]`
  deles guarda outra coisa).
- Teste no hospedeiro: alem das imagens do `mkext2.py`, o `cargo test` usa
  imagens do `mke2fs` de verdade quando ele existe (inodes de 256 bytes,
  `dir_index`, `ext_attr`, `resize_inode`, varios grupos de 2 KiB) e um
  diretorio com indice htree gerado pelo `debugfs` e `e2fsck -D`. O cenario
  cria 250 arquivos de nome comprido (diretorio no indireto), apaga metade,
  recria, move arquivos e diretorios entre pais (conferindo `..` e
  `links_count`), sobrescreve com `rename`, apaga arquivo aberto, esgota os
  inodes ate `ENOSPC`, apaga tudo e confere que blocos e inodes voltaram ao
  numero inicial. O `e2fsck -fn` roda no meio e no fim e precisa sair limpo.
  O fuzz ganhou uma rodada de escrita: imagens mutadas montadas com escrita
  recebem criacao, remocao, `rename` e `write` sorteados e nao podem dar
  `panic`. No Windows, sem `mke2fs` nem `e2fsck`, essas partes sao puladas
  com aviso e o resto roda.
- Parte 3: `mkfs.ext2` e `fsck.ext2` sao programas da musl em C
  (`user/musl/mkfs.ext2`, `user/musl/fsck.ext2`), como o `mkfs` de FAT, e nao
  Rust: o Rust fica no kernel, nos lugares da secao "Linguagens". O formato
  passa a estar descrito em tres lugares (o crate do kernel, o `mkext2.py` e
  estes dois); o que impede que divirjam e o `e2fsck` oficial conferindo os
  tres no hospedeiro.
- `mkfs.ext2` grava o mesmo formato do `mkext2.py`: revisao 1, inodes de 128
  bytes, `filetype`, `sparse_super` e `large_file`, sem `resize_inode`,
  `dir_index` nem `ext_attr`. Bloco de 1 KiB ate 512 MiB e de 4 KiB acima
  (`-b` troca), um inode a cada 4 KiB ate 512 MiB e a cada 16 KiB acima (`-i`
  e `-N` trocam), como o `mke2fs`. Ultimo grupo menor que a propria sobrecarga
  mais 50 blocos e descartado. `lost+found` nasce com 12 blocos (1 KiB) ou
  4 blocos (4 KiB), para o `fsck` nao precisar alocar. Zera o primeiro KiB do
  disco: um setor de boot de FAT antigo faria a deteccao automatica achar FAT.
  Sem blocos reservados para o root, porque o kernel ainda nao os respeita.
  UUID aleatorio do `/dev/urandom`.
- Os dois leem e gravam o dispositivo com `lseek` + `read`/`write`: no Orion,
  `pread`/`pwrite` so valem para arquivo regular (`positional_file` devolve
  `ESPIPE` para dispositivo de bloco). No Linux funcionaria, entao o teste no
  hospedeiro roda com uma biblioteca que faz `pread` falhar e corta cada
  `read`/`write` em 4 KiB, como o kernel faz com arquivo que nao e regular.
- `fsck.ext2` faz as passadas do `e2fsck` numa versao basica: 1) inodes
  (tipo, ponteiros fora da area de dados, bloco com dois donos, tamanho,
  `i_blocks`, campos do ext4 que tem de ser zero, orfaos de queda com arquivo
  apagado aberto); 2) entradas de diretorio (registro corrompido, `.` e `..`,
  entrada para inode livre, nome invalido, tipo errado, diretorio com dois
  nomes); 3) arvore (`..` errado, diretorio desconectado ou em ciclo); 4)
  `links_count`; 5) bitmaps, contadores por grupo, diretorios por grupo e
  totais do superbloco. Com `-y` corrige o que nao perde dado; o que pediria
  religar no `lost+found` (inode sem nome, diretorio desconectado, bloco com
  dois donos, buraco em diretorio) fica como "nao corrigido" e a saida e 4.
  Nao confere o indice htree, os inodes reservados (o inode 7 de
  redimensionamento do `mke2fs`) nem horarios. Recusa disco montado. Saida 0,
  1, 4 ou 8, como o `e2fsck`.
- Validacao no hospedeiro: imagens do `mkfs.ext2` em 15 tamanhos (de 1 MiB a
  600 MiB, tamanhos quebrados e grupo final descartado) passam no `e2fsck -fn`,
  e o crate do kernel montou seis delas (blocos de 1, 2 e 4 KiB, grupo final
  descartado) e rodou nelas o cenario pesado de criar, apagar e renomear. O
  `fsck.ext2` concorda com o `e2fsck` em 70 imagens limpas (do `mkfs.ext2`,
  do `mkext2.py`, do `mke2fs` com htree e inodes de 256 bytes, e as gravadas
  pelos testes do crate) e corrige 12 corrupcoes
  feitas com o `debugfs` (links, bitmaps, contadores, orfao, ponteiro fora do
  disco, `i_blocks`, tamanho, entrada para inode apagado, raiz sem links); o
  `e2fsck` acha a imagem limpa depois. Fuzz com ASan e UBSan: cada imagem
  mutada passa por `-n`, `-y`, `-y` e `-n`, e o segundo `-y` nao pode achar
  mais nada. Ele achou tres erros, corrigidos: leitura alem do fim do disco
  por ponteiro direto invalido de diretorio no `-n`, raiz com zero links
  tratada como inode apagado, e `..` "corrigido" sem haver entrada onde
  gravar (agora o `.` e o `..` sao recriados quando o comeco do bloco esta
  vazio). Rodada final: 480 imagens sem erro.
- Regressao C: depois do `ext2test`, `mkfs.ext2` e `fsck.ext2` recusam o
  `sdc` montado; com os dois desmontados, o `fsck.ext2` confere o `sdc` e o
  `sdd` (o que o kernel gravou tem de estar limpo), o `mkfs.ext2` formata o
  `sdd`, o `fsck.ext2` confere, o kernel monta, o `fstest -a` passa (o
  criterio da etapa) e o `fsck.ext2` confere de novo depois do `umount`.

## v2.1.0-a6, etapa 8.7 (memcpy, memset e memmove em assembly)

- `memcpy`, `memset` e `memmove` do kernel estao em `arch/x86_64/mem.S`. Elas
  ja usavam `rep movsb`/`rep stosb` por assembly embutido no C, entao o ganho
  de desempenho e nulo; o que muda e que o codigo fica onde o plano diz que
  assembly mora, e o `memmove` para tras deixou de ser um laco de bytes em C.
- O `memmove` so copia de tras para frente quando o destino comeca dentro da
  origem; ai usa `std`, `rep movsb` com os ponteiros no ultimo byte e `cld`.
  Isso e seguro porque toda entrada no kernel (`isr_common` e
  `syscall_entry`) faz `cld` antes de chamar C, e o `iretq` devolve a flag de
  direcao de quem foi interrompido; uma interrupcao no meio da copia nao faz
  um `memcpy` do tratador andar para tras. A copia para tras nao tem a
  otimizacao de strings rapidas das CPUs, mas e rara no kernel.
- `memcmp` e as funcoes de string continuam em C: o clang gera para elas
  codigo tao bom quanto o feito a mao.

## v2.1.0-a6, etapa 2.7 parte 1 (links simbolicos)

- A resolucao de caminho da VFS (`walk_core`) segue links: quando um
  componente e link e nao e o ultimo (ou e o ultimo e o chamador quer seguir,
  ou vem uma barra depois), o alvo e lido pela operacao `readlink` do sistema
  de arquivos e emendado no resto do caminho, num buffer alocado so no
  primeiro link. Alvo absoluto recomeca da raiz; relativo, do diretorio que
  contem o link. Limite de 40 links por resolucao (`ELOOP`), como o Linux; o
  `..` depois de um link vai ao pai real do alvo, nao ao do link.
- Quem nao segue o ultimo componente: `lstat`, `newfstatat` com
  `AT_SYMLINK_NOFOLLOW`, `readlink`, `faccessat2` com `AT_SYMLINK_NOFOLLOW`,
  `umount2` com `UMOUNT_NOFOLLOW`, e tudo que passa por `walk_parent`
  (`unlink`, `rmdir`, `rename`, `mkdir`, `symlink`), que segue os links do
  meio mas nunca o ultimo. `open` com `O_NOFOLLOW` no link da `ELOOP`.
  `open` com `O_CREAT` num link segue o alvo e, se ele nao existe, cria o
  alvo, como no Linux; com `O_EXCL` da `EEXIST`. Isso e um laco (nao
  recursao), para 40 links em cadeia nao gastarem a pilha do kernel.
- O caminho em texto que o processo guarda (o cwd e o caminho de cada arquivo
  aberto, usados pelo `getcwd`, pelo `openat` relativo e pelo nome do ponto de
  montagem) e normalizado lexicalmente, o que da errado depois de um link.
  A resolucao avisa quando seguiu algum link, e ai o texto e descartado e
  recalculado a partir do vnode (`vfs_getpath`): `getcwd` depois de
  `chdir` por um link mostra o caminho real, como no Linux.
- Operacao nova `symlink` no `struct fs_type`. tmpfs guarda o alvo como texto
  no no. ext2 guarda alvo de ate 59 bytes dentro do inode (`i_block`, sem
  bloco de dados) e maior num bloco proprio, como o Linux, e o alvo tem de
  caber num bloco. FAT nao tem a operacao e devolve `EPERM`. Alvo vazio da
  `ENOENT`.
- Testes: `cargo test` cria links de 1, 59, 60, 61, 300 e (tamanho do bloco -
  1) bytes em imagens do `mkext2.py` e do `mke2fs`, confere o lugar (inode ou
  bloco), le de volta, renomeia, apaga e confere que blocos e inodes voltaram,
  com `e2fsck -fn` no meio e no fim; o fuzz de escrita sorteia tambem
  `symlink`. O `linktest` (novo) foi conferido contra o Linux do hospedeiro
  antes de entrar e roda na regressao no tmpfs (sequencia A) e no ext2
  formatado pelo `mkfs.ext2` (sequencia C, antes do `fsck.ext2` final):
  relativo, absoluto, cadeia, diretorio no meio do caminho, `..` fisico,
  `getcwd`, laco, pendurado, `O_CREAT` por pendurado, `O_NOFOLLOW`, 40 saltos
  passam e 41 dao `ELOOP`, alvo longo, `rename` e `unlink` do link. A
  sequencia A tambem roda `ln -s` e `readlink` do sbase.

## v2.1.0-a7 (etapa 2.7 parte 2: modo, dono e datas)

- Duas operacoes novas no `struct fs_type`: `getattr` e `setattr`, com uma
  `struct vattr` (modo, dono, grupo, `nlink`, blocos, `atime`, `mtime` e
  `ctime` com nanossegundos). O `stat` passa a sair de `vfs_getattr`, que
  preenche os valores de antes (dono 0, `nlink` 2 ou 1, hora do boot) e deixa
  o sistema de arquivos sobrescrever o que ele sabe. Quem nao tem `setattr`
  (devfs, procfs) da `EPERM`; montagem so de leitura da `EROFS` antes de
  chegar nele.
- O vnode continua guardando o modo (`v->mode`), porque a VFS consulta o tipo
  a toda hora sem trava. O `setattr` grava os bits de permissao nele com
  `__atomic_store_n` depois de gravar no disco; o tipo nunca muda, entao quem
  le sem trava nunca ve um tipo errado.
- `chown` tira `S_ISUID`, e `S_ISGID` quando o grupo tem execucao, de tudo
  que nao e diretorio, mesmo com `-1 -1`, como o Linux faz (a flag
  `VA_KILL_SUID` e aplicada pelo sistema de arquivos dentro da trava dele,
  para nao haver corrida entre ler o modo e gravar). Ainda nao ha conferencia
  de permissao: todo processo e root ate a 4.1, que vai ligar isso.
- `umask` por processo, 022 no boot, herdado no `fork`. `open` com `O_CREAT`
  e `mkdir` passam o modo pedido menos o `umask` ao `create` do sistema de
  arquivos (antes era sempre 0644 ou 0755). O dono do arquivo novo e 0 ate a
  4.1.
- ext2: o Rust ganhou `ext2_setattr` (modo, dono, grupo, `atime`, `mtime`;
  o `ctime` vira a hora da chamada) e o `write_inode` passou a gravar dono e
  grupo, inclusive os 16 bits altos (`l_i_uid_high`/`l_i_gid_high`, como o
  Linux). As datas do ext2 sao segundos de 32 bits com sinal: o C limita o
  que chega a faixa de 1901 a 2038 em vez de truncar. O `create` passou a
  usar so os bits de permissao do modo recebido. O C le os atributos sempre
  do inode (`ext2_stat` dentro do `em->lock`), sem copia no vnode que possa
  ficar velha.
- Ordem de travas do ext2, registrada aqui por ser a primeira etapa depois
  da 2.6 que mexe nele: o `ns_lock` da montagem vem antes do `em->lock`, e
  nunca se chama `vnode_put` segurando o `em->lock`, porque o `release` do
  vnode de um arquivo apagado pega essa trava para liberar o inode. O
  `getattr` e o `setattr` pegam so o `em->lock` e nao soltam vnode.
- tmpfs: cada no guarda dono, grupo e as tres datas em nanossegundos (hora
  real do `timesys_realtime`), mais a contagem de paginas (para o
  `st_blocks`) e de subdiretorios (para o `nlink` de diretorio, 2 + filhos
  diretorios, como no Linux; arquivo apagado ainda aberto tem `nlink` 0).
  `write` e `truncate` atualizam `mtime` e `ctime`; criar, apagar e renomear
  atualizam os do diretorio. A leitura nao atualiza o `atime` (como montar com
  `noatime`): gravar o inode a cada leitura nao vale o custo aqui. As opcoes
  `mode=`, `uid=` e `gid=` da montagem, antes aceitas e ignoradas, agora
  valem para a raiz (padrao `01777`, dono 0).
- FAT: as datas saem da entrada de diretorio (`mtime` da modificacao, `atime`
  do dia do ultimo acesso, `ctime` da criacao, como o `vfat` do Linux), lidas
  como UTC, a mesma hora do RTC (o `vvfat` do QEMU grava a hora local do
  hospedeiro, por isso os arquivos do `out\esp` aparecem deslocados pelo
  fuso). O bit de somente leitura vira modo sem escrita. O `setattr` grava o
  que o FAT consegue guardar: datas (2 s de resolucao, entre 1980 e 2107),
  liga e desliga o somente leitura (`chmod 444`/`644`), e aceita dono 0; o
  resto da `EPERM`, como o `vfat` sem a opcao `quiet`. A raiz e os
  diretorios achados so pelo `..` nao tem entrada conhecida (`has_entry`) e
  recusam mudar datas. O vnode de diretorio que nasceu pelo `..` ganha a
  entrada na primeira busca pelo nome.
- Programas: o `ls` aceita `-l` (modo, links, dono e grupo pelo nome do
  `/etc/passwd` e `/etc/group` quando existe, tamanho, data, `-> alvo`) e
  varios caminhos; sem `-l` a saida e a mesma de antes. O `touch` passou a
  atualizar as datas de arquivo que ja existe (`utimensat`), em vez de so
  abrir. `chown` e `chgrp` do sbase entraram no `sbase-box`. `etc/passwd` e
  `etc/group` (so o root) vao para o `out\esp\etc`. `attrtest` (novo) confere
  `umask`, `chmod`, `fchmod`, `chown`, `fchown`, `lchown`, `fchownat`,
  `utimensat` (com `UTIME_NOW`, `UTIME_OMIT` e `AT_SYMLINK_NOFOLLOW`),
  `futimens`, `utime`, `utimes`, `mtime` e `ctime` mudando no `write`, no
  `truncate` e no diretorio, `st_nlink` e `st_blocks`; deixa um arquivo com
  modo, dono e datas conhecidos que `attrtest -k` confere depois de
  remontar.
- `build.ps1`: quando qualquer fonte do kernel e recompilada, o `main.c`
  tambem e, para a hora do build na linha de boot (`__TIME__`) dizer de fato
  quando o kernel foi montado. Antes ela so mudava quando o proprio `main.c`
  mudava e nao servia para confirmar que o kernel testado era o novo.

## v2.1.0 (etapa 2.8: GPT e raiz em ext2; fecha o M2)

- O kernel le GPT: quando o MBR tem uma entrada do tipo `0xEE` (MBR
  protetor), o cabecalho em LBA 1 e conferido (assinatura, tamanho, LBA dele
  mesmo, CRC32 do cabecalho e do vetor de entradas, limites dentro do disco,
  vetor de no maximo 256 KiB); se o principal estiver ruim, vale a copia do
  ultimo setor, com aviso. Cada entrada com tipo diferente de zero e dentro da
  area utilizavel vira um dispositivo `sdXN` (N e a posicao da entrada, como no
  Linux). O tipo e guardado (`part_kind`): ESP, raiz Linux x86-64
  (`4F68BCE3-...`), dados Linux ou outro. Disco sem `0xEE` continua lido como
  MBR, como antes. O CRC32 e uma funcao nova em `lib/crc32.c`.
- Escolha da raiz: primeiro uma particao GPT de raiz Linux x86-64 montada como
  ext2; se nao houver (ou nao montar), o caminho antigo, que monta como FAT a
  primeira particao que aceitar (o `vvfat` do QEMU continua funcionando). Com a
  raiz em ext2, a ESP do mesmo disco e montada em `/boot/efi`, como nas
  distribuicoes Linux: e onde ficam o Limine e o `orion.elf`. Nao ha opcao
  `root=` na linha de comando ainda; a escolha pelo tipo da particao basta para
  o disco que o `build.ps1 -Image` gera e para o instalador da 7.3.
- Bug achado: o ext2 so montava com escrita se o proprio dispositivo tivesse a
  funcao de escrita, e particao nao tem (a escrita passa pelo disco pai). Ate
  aqui o ext2 so tinha sido montado em disco inteiro (`sdc`, `sdd`), por isso
  nao aparecia. `blockdev_writable` resolve o disco de baixo; o FAT, que fazia
  essa conta a mao com um nivel so, passou a usar a mesma funcao.
- `tools/mkimage.py` gera o `out\orion.img` no hospedeiro, sem depender de
  ferramenta do Windows: MBR protetor, GPT principal e de reserva (128
  entradas), ESP de 64 MiB em FAT16 a partir de 1 MiB e a raiz em ext2 logo
  depois (o conteudo e uma folga, no minimo 256 MiB, blocos de 4 KiB, 8192
  inodes por grupo). O FAT16 e escrito pelo proprio script (setor de boot,
  duas FATs, raiz de 512 entradas, nomes longos com o apelido 8.3), so com o
  que o firmware e o Limine precisam: `EFI/BOOT/BOOTX64.EFI`,
  `boot/limine/limine.conf` e `boot/orion.elf`. A raiz em ext2 sai do
  `mkext2.py` (usado como modulo) com os diretorios que o build gerou em
  `out\esp` (menos `boot` e `EFI`, e sem os arquivos soltos na raiz do
  `out\esp`, que sao restos de testes antigos no `vvfat`), mais os pontos de
  montagem (`/boot/efi`, `/dev`, `/proc`, `/tmp` com `01777`, `/run`, `/mnt`,
  `/root`). Arquivo que comeca com o cabecalho ELF ganha `0755`, o resto
  `0644`. Os GUIDs unicos sao sorteados a cada imagem.
- FAT16 e nao FAT32 na ESP: o firmware do QEMU e o Limine leem os dois, o
  kernel tambem, e o FAT16 e mais simples de gerar certo. A especificacao UEFI
  pede FAT32 na ESP; a 7.3 (instalador) ou a 7.4 (hardware real) trocam se
  algum firmware recusar.
- `build.ps1 -Image` gera a imagem depois do build; `-Run -Image` boota dela.
  `test.ps1 -Image` e `qrun.py --image` usam o `out\orion.img` no lugar do
  `fat:rw:out/esp`, e os dois harness ganharam o passo `reboot:`: encerra o
  QEMU pelo monitor, sobe de novo com os mesmos discos e espera o prompt,
  anotando `----- reboot -----` no log. A regressao inteira (A a E) roda na
  imagem; o `vvfat` saiu da rotina, mas continua sendo o padrao do
  `build.ps1 -Run` e do `test.ps1` sem `-Image`.
- Como a regressao escreve na raiz (testes que criam arquivos em `/`), a
  imagem e regerada em todo build da regressao; `-NoBuild` reaproveita a
  imagem com o que as rodadas anteriores deixaram, como acontecia com o
  `out\esp`.

## v2.2.0-a1 (etapa 3.1: TCP parte 1)

- O TCP e Rust, como o plano pede depois do piloto do ext2. Para ter mais de
  um crate Rust no kernel sem duplicar `core`, o alocador e o `panic` (cada
  `staticlib` traz uma copia deles), `kernel/rust` virou um workspace do
  Cargo: `ext2` e `tcp` sao `rlib`, e `orion` e o unico `staticlib`
  (`liborion_rust.a`), que junta os dois e tem o `kernel.rs` (alocador ligado
  ao `kmalloc` e `panic` do kernel), antes dentro do `ext2`. Perfis e
  `Cargo.lock` ficam na raiz do workspace. O `build.ps1` so compila e liga o
  crate cujo `Cargo.toml` diz `staticlib`; o `Makefile` aponta para o `orion`.
  O `check.ps1` continua rodando clippy e `cargo test` em cada crate.
- Desenho do TCP: o Rust nao faz E/S nem guarda tempo. O C chama
  `tcp_input` com o segmento e os enderecos e recebe de volta o que mandar (por
  enquanto, um `RST`); quem envia e o C, pelo `net_send_ip`. Isso deixa o
  protocolo testavel no hospedeiro, sem QEMU, e vai continuar assim nas
  partes 2 a 4 (conexoes, retransmissao com o relogio passado pelo C).
- Parte 1: cabecalho (com opcoes puladas pelo `data offset`), checksum com o
  pseudo-cabecalho IPv4, tabela de conexoes (busca exata pelos quatro campos e
  depois por quem escuta na porta, com IP local curinga) e `RST` para porta
  fechada pela RFC 793: segmento com `ACK` recebe `RST` com `SEQ = SEG.ACK`;
  sem `ACK`, `RST|ACK` com `ACK = SEG.SEQ + SEG.LEN` (SYN e FIN contam 1); um
  `RST` nunca e respondido. Segmento com checksum errado e contado e
  descartado. Por enquanto a tabela nunca tem entradas (nao ha `listen` nem
  `connect`), entao toda porta esta fechada.
- C: `net.c` entrega o protocolo 6 ao TCP so quando o destino e o IP da
  interface (nada de responder a broadcast). A chamada ao Rust fica sob um
  mutex (`tcp`), e o envio da resposta e feito depois de solta-lo, porque o
  `net_send_ip` pode dormir esperando o ARP. Contadores no comando `net` do
  shell do kernel.
- Verificacao: sem `nmap` no Windows, o criterio ("o hospedeiro ve a porta
  fechada") e testado com o `hostfwd` do QEMU: `test.ps1 -HostFwd` e o passo
  `tcpprobe:<porta>` (tambem no `qrun.py`) conectam do hospedeiro e anotam no
  log se a conexao foi fechada, ficou sem resposta ou recebeu dados. O SLIRP
  aceita a conexao do hospedeiro e manda o SYN ao Orion; o `RST` dele faz o
  SLIRP fechar a conexao do hospedeiro, e o SLIRP so aceita o `RST` se o
  checksum estiver certo. Conferido tambem no `out\net.pcap`: `RST|ACK` com
  `seq 0`, `ack` = SEQ do SYN + 1 e checksum valido. A regressao confere
  tambem os contadores do `net` (segmentos sem erro de checksum e `RST`
  enviados).

## v2.2.0-a2 (etapa 3.2: TCP parte 2)

- O crate `tcp` ganhou conexoes ativas: `SYN_SENT`, `ESTABLISHED`,
  `FIN_WAIT_1/2`, `CLOSE_WAIT`, `CLOSING`, `LAST_ACK`, `TIME_WAIT` e
  `CLOSED`. Cada conexao tem um buffer de recepcao de 32 KiB (a janela que
  anunciamos e o espaco livre nele, sem escala) e um de envio de 32 KiB, que
  guarda os bytes desde `SND.UNA` (o que ja foi mandado e ainda nao foi
  reconhecido fica la para a retransmissao da 3.3). O `SYN` leva a opcao MSS
  (1460) e o MSS do outro lado vem do `SYN|ACK` (536 sem a opcao).
- Recepcao so em ordem: segmento fora de ordem recebe um ACK repetido e e
  descartado, e o pedaco ja recebido de uma retransmissao com sobreposicao e
  cortado. A aceitacao segue a tabela da RFC 793 (segmento dentro da janela),
  e a janela do outro lado so e atualizada pela regra de `SND.WL1`/`SND.WL2`.
  Envio respeita a janela do outro lado e o MSS; com janela zero nada sai ate
  chegar uma atualizacao (a sonda de janela zero e da 3.3). Quando a leitura
  reabre a janela que estava abaixo de um MSS, sai um ACK de atualizacao.
- `TIME_WAIT` ainda sem temporizador: a conexao sai da tabela assim que o
  socket foi fechado e ela chegou a `TIME_WAIT` ou `CLOSED`. Conexao com erro
  (`RST` recebido) fica `CLOSED` ate o `close`, devolvendo o erro no `read`.
- O C nao sabe nada de sequencia: `tcp_write` enfileira, `tcp_output` devolve
  um segmento por vez (dados, `FIN` ou ACK pendente) e o `net.c` envia; depois
  de cada segmento recebido para uma conexao o `net.c` chama o `tcp_output`
  dela de novo (assim o `FIN` e os dados que esperavam janela saem sem
  ninguem estar escrevendo) e avisa o socket. Os envios de uma conexao sao
  serializados por um mutex proprio (`tcp-tx`) para os segmentos nao trocarem
  de ordem entre a montagem e o envio; ordem de travas: `tcp-tx` antes de
  `tcp`. O envio pode dormir no ARP; o gateway do SLIRP ja esta no cache
  depois do `connect`.
- Sockets: `SOCK_STREAM` com `connect`, `read`/`write` (e `send`/`recv` pelo
  `sendto`/`recvfrom`), `poll` e `close` (manda `FIN` e solta a conexao).
  A espera usa um contador de eventos por socket, lido antes de conferir o
  estado e comparado dentro da trava antes de dormir, para nao perder um
  evento que chegue entre a conferencia e o sono. `SOCK_NONBLOCK` e
  `SOCK_CLOEXEC` no tipo do `socket` passaram a valer para todos os tipos.
  Sem `SIGPIPE` na escrita em conexao fechada (so `EPIPE`); `bind`, `listen`
  e `accept` em TCP sao da 3.4.
- `/etc/resolv.conf` (com o DNS do SLIRP) entrou ja aqui, porque o
  `getaddrinfo` da musl le dele; a 3.6 cuida do resto da configuracao.
- Teste: `httpget` (novo, HTTP/1.0, imprime o status, o tamanho e o CRC32 do
  corpo). A regressao sobe `tools/hostweb.py` no hospedeiro (pagina pequena e
  1 MiB de conteudo fixo) e o Orion baixa pelo 10.0.2.2, que o SLIRP leva ao
  127.0.0.1 do hospedeiro: nao depende da internet. O criterio do plano
  (`example.com`) tambem esta na regressao, e depende da internet do
  hospedeiro como o `nslookup`. No Windows o SLIRP nao responde nada quando a
  porta do hospedeiro esta fechada (so o nosso `SYN` aparece no pcap), entao
  esse `connect` termina em `ETIMEDOUT` depois de 20 s.
- `sendmsg` e `recvmsg` (46 e 47) entraram porque o resolvedor de DNS da musl
  usa o `recvmsg`: `sendto`, `recvfrom`, `sendmsg` e `recvmsg` passaram a ser
  feitos em cima de duas funcoes com buffer do kernel (`send_kernel` e
  `recv_kernel`), e as duas novas juntam e espalham os `iovec` (ate 1024,
  2 GiB no total). Sem dados de controle: `msg_controllen` volta 0, e
  `MSG_TRUNC` sai em `msg_flags` quando o datagrama nao coube.
- O `tmptest` conferia vazamento do tmpfs pelo `MemFree`, que tambem muda
  quando o cache de arquivos e despejado ou relido. Com a raiz em ext2, criar
  1000 arquivos empurra para fora da LRU vnodes com paginas de binarios da
  raiz, que voltam depois, e o `MemFree` variou de -262 a +74 paginas entre
  rodadas iguais. A medida passou a ser `MemFree + Cached` (pagina do tmpfs
  nao entra no `Cached`, e despejar ou reler cache nao muda a soma).

## v2.2.0-a3 (etapa 3.3: TCP parte 3)

- O relogio continua fora do Rust: o C passa `now` em milissegundos ao
  `tcp_input`, ao `tcp_output` e ao `tcp_connect`, e uma thread do kernel
  (`tcptimer`) chama `tcp_tick` a cada 50 ms, que devolve um segmento por vez
  (retransmissao, sonda) e os eventos (desistencia, fim do `TIME_WAIT`). O que
  o `tick` manda tambem passa pelo mutex `tcp-tx`.
- Retransmissao pela RFC 6298: RTO inicial de 1 s, SRTT e RTTVAR de
  Jacobson/Karels medidos com um segmento por vez (o `SYN` tambem conta), sem
  medir segmento retransmitido (Karn), RTO entre 200 ms e 60 s, dobrando a
  cada vencimento. Vencido, o envio volta a `SND.UNA` (go-back-N: o
  `SND.NXT` volta e o `SND.MAX` guarda ate onde ja se mandou, para separar
  envio novo de retransmissao) e o `FIN` volta para depois dos dados. O `SYN`
  desiste depois de 5 retransmissoes e os dados depois de 8, com `ETIMEDOUT`.
- Controle de congestionamento Reno simples: `cwnd` inicial de 10 MSS
  (RFC 6928), crescimento exponencial ate o `ssthresh` e linear depois; no
  RTO, `ssthresh` = metade do que esta em voo e `cwnd` = 1 MSS; com 3 ACKs
  repetidos (mesmo `ack`, sem dados, mesma janela, algo em voo), retransmissao
  rapida do segmento em `SND.UNA` e recuperacao com `cwnd` = `ssthresh` +
  3 MSS, voltando a `ssthresh` no proximo ACK novo. A janela efetiva e o
  menor entre a do outro lado e a `cwnd`.
- Recepcao com reordenacao: segmento adiante do `RCV.NXT` (dentro da janela)
  fica guardado, ate 32 KiB, e entra quando o buraco e preenchido; o ACK pula
  tudo de uma vez. `FIN` fora de ordem nao e guardado (o outro lado manda de
  novo).
- Sonda de janela zero: com dados esperando, nada em voo e janela zero, sai a
  cada RTO (dobrando ate 60 s) um ACK com `SEQ = SND.UNA - 1`, que obriga o
  outro lado a responder com a janela atual. A sonda nao conta tentativas: a
  conexao pode esperar a janela abrir indefinidamente, como no Linux.
- `TIME_WAIT` de verdade: 2*MSL com MSL de 30 s; um `FIN` repetido nesse
  estado recebe ACK e reinicia o tempo. Conexao solta (`close` feito) sai da
  tabela quando o `TIME_WAIT` vence.
- O `connect` espera ate 75 s (o `SYN` desiste antes, em uns 63 s) e
  resolve o ARP antes de mandar o `SYN`.
- Teste de perda de verdade: o SLIRP nao perde nada, entao o comando
  `tcploss N` do shell do kernel descarta 1 em cada N segmentos TCP (contados
  juntos nos dois sentidos, antes do `tcp_input` e no envio). O comando `net`
  mostra as retransmissoes e o que foi descartado. A regressao baixa 10 MiB sem
  perda (o criterio da etapa), e com 1 em 10 descartado envia 1 MB (o
  `tools/hostweb.py` ganhou `PUT`, que responde com tamanho e CRC32 do que
  chegou; o `httpget` ganhou `-u`) e recebe 1 MiB, conferindo os CRC32.
  Medido: 10 MiB em 0,7 s sem perda e 16 s com 1 em 20; 2 MB enviados em 15 s
  com 1 em 10 (342 retransmissoes). Com perda alta recebendo, quem manda e o
  TCP do SLIRP, com RTO minimo de 1 s, e fica lento (10 MiB com 1 em 7 levou
  quase 5 minutos).
- No crate, os testes simulam o outro lado: `SYN` com backoff ate
  `ETIMEDOUT`, RTO com volta a `SND.UNA`, amostra de RTT, retransmissao
  rapida, reordenacao, sonda de janela zero, `FIN` retransmitido, desistencia
  dos dados, e duas simulacoes com perda aleatoria (0 a 30% enviando 200 KB e
  0 a 25% recebendo 150 KB com troca de ordem) conferindo o conteudo inteiro.
- O `tmptest` ainda falhava as vezes com `MemFree + Cached` (-32 paginas na
  regressao). Mostrando os componentes, a variacao (de -70 a +184 paginas
  entre rodadas iguais) estava toda no `MemFree`, com `Cached` e
  `KernelHeap` parados: sao paginas de slab do heap do kernel, que entram e
  saem conforme a fragmentacao dos 1000 nos criados e apagados (o
  `KernelHeap` conta bytes em uso, nao as paginas dos slabs; o no do tmpfs
  cresceu na 2.7 e mudou de tamanho de slab). Um vazamento de pagina do tmpfs
  nesse teste seria de pelo menos 1000 paginas (uma por arquivo de 3000
  bytes), entao a tolerancia foi para 256 paginas, e o teste imprime os
  componentes para a proxima investigacao.

## v2.2.0-a4 (etapa 3.4: TCP parte 4, servidor)

- Abertura passiva no crate `tcp`: estados `LISTEN` e `SYN_RECEIVED`. Quem
  escuta e uma entrada da tabela com o lado remoto zerado (a busca da 3.1 ja
  caia nela depois da exata). Um `SYN` cria uma conexao filha em
  `SYN_RECEIVED`, com ISS sorteado por um gerador xorshift semeado pelo C
  (`random_u64` na criacao da pilha), e responde `SYN|ACK` com a opcao MSS. O
  `SYN|ACK` e retransmitido com backoff e a filha some em silencio depois de 5
  tentativas; `SYN` repetido recebe o mesmo `SYN|ACK`; `RST` na sequencia
  certa tira a filha. O ACK do handshake (que pode trazer dados) leva a filha a
  `ESTABLISHED` e a poe na fila de `accept`, e o evento vai para o socket de
  quem escuta. `ACK` solto para quem escuta recebe `RST`.
- `backlog` conta as filhas em `SYN_RECEIVED` mais as que esperam `accept`
  (limitado a 128); com a fila cheia o `SYN` e ignorado, e o cliente tenta de
  novo pelo proprio RTO, como o Linux faz.
- Fechar quem escuta: filhas ainda no handshake somem, e as ja estabelecidas
  e nao aceitas sao soltas com `FIN` (o `tick` agora tambem manda o `FIN` de
  conexao solta que estava esperando). O Linux manda `RST` nesse caso; o
  `FIN` foi escolhido para nao precisar de um caminho de envio novo e o
  cliente ve o fim da conexao do mesmo jeito.
- Porta: `bind` em TCP confere a porta na tabela do Rust (`tcp_port_busy`).
  Sem `SO_REUSEADDR` a porta esta ocupada por qualquer conexao viva nela,
  inclusive em `TIME_WAIT`; com `SO_REUSEADDR` so outro `LISTEN` impede. Porta
  0 no `bind` ou `listen` sem `bind` pegam uma efemera. `connect` depois de
  `bind` ainda usa uma porta efemera nova (o endereco do `bind` so vale para
  quem escuta).
- Sockets: `listen`, `accept` e `accept4` (`SOCK_NONBLOCK`, `SOCK_CLOEXEC`),
  `shutdown` (`SHUT_WR` manda o `FIN` e mantem o socket; `SHUT_RD` faz o
  `read` devolver 0), `getsockname`, `getpeername`, `setsockopt` e
  `getsockopt` (`SO_REUSEADDR`, `SO_TYPE`, `SO_ERROR`, `SO_RCVBUF` e
  `SO_SNDBUF` so para ler, `TCP_NODELAY`). `TCP_NODELAY` aceita so ligar: o
  TCP daqui nao tem Nagle, entao ele ja esta ligado de fato; desligar da
  `ENOPROTOOPT`, como qualquer opcao que nao existe (nada e aceito sem
  efeito). `poll` de quem escuta fica legivel com conexao na fila.
- `select` sobre o `poll`: o laco do `poll` virou uma funcao interna com o
  vetor no kernel, usada pelos dois; os conjuntos de ate 1024 descritores viram
  ate 32 `pollfd` (o limite do `poll`; mais que isso da `EINVAL`), o prazo
  restante volta no `timeval`, e sinal devolve `EINTR` (reiniciado pelo
  `SA_RESTART`). O `pselect6` ficou de fora (a musl no x86_64 usa o `select`).
- Teste do criterio ("`httpd` no Orion responde a `curl` do hospedeiro"): o
  passo `hostget:<porta>:<caminho>` dos harness faz um GET HTTP/1.0 do
  hospedeiro por `hostfwd` e anota status, tamanho e o comeco do corpo no log
  (o `curl` do Windows faria o mesmo, mas o harness nao pode depender dele). O
  `httpd` novo usa `select` com prazo, `accept4`, `getpeername`, `shutdown`
  com `SHUT_WR` e `SO_REUSEADDR`, e a regressao roda dois em seguida na porta
  80 para conferir o `SO_REUSEADDR` com as conexoes da primeira em
  `TIME_WAIT`.

## v2.2.0-a5 (etapa 3.5: loopback e sockets Unix)

- Loopback: o `net_send_ip` para 127.0.0.0/8 ou para o proprio IP monta o
  pacote IP e o poe numa fila do kernel (ate 256 pacotes, depois descarta e
  conta), sem passar pela placa nem pelo ARP. A thread de recepcao esvazia essa
  fila antes de olhar a placa e entrega pelo mesmo `handle_ip`, entao UDP,
  ICMP e TCP funcionam iguais. Pacote para 127.x sai com origem 127.x; para o
  proprio IP, com o proprio IP. O TCP aceita 127.x como endereco local e o
  `connect` para 127.x funciona mesmo sem rede configurada. Contadores no
  comando `net`.
- Sockets Unix (`kernel/src/net/unix.c`): so `SOCK_STREAM` (o `SOCK_DGRAM`
  ficou de fora). Cada ponta tem um buffer de 32 KiB onde a outra escreve; a
  escrita bloqueia com o buffer do outro cheio e o fim de arquivo vem do
  `close` ou do `shutdown(SHUT_WR)` do outro lado. Uma trava giratoria unica
  protege todos os sockets Unix (a contencao e baixa); a espera usa o mesmo
  contador de eventos dos sockets TCP.
- O endereco e um caminho na VFS: o `bind` cria um no `S_IFSOCK` (operacao
  nova `vfs_mknod`) com o `umask`, e o socket que escuta guarda uma referencia
  ao vnode. O `connect` resolve o caminho e procura quem escuta pelo vnode, nao
  pelo texto; apagar o caminho impede conexoes novas mas nao derruba quem ja
  escuta, como no Linux. O tmpfs e o ext2 guardam o no (o ext2 aceita
  `S_IFSOCK` no `create`, sem blocos); o FAT recusa com `EPERM` (antes
  criaria um arquivo comum). `open` num socket da `ENXIO`.
- `connect` completa na hora: a ponta do servidor e criada ja conectada e vai
  para a fila de quem escuta (ate o `backlog`, depois `EAGAIN`); o `accept` so
  a retira. Fechar quem escuta da fim de arquivo as conexoes que ninguem
  aceitou. `socketpair` (53) cria o par direto. `getsockname`, `getpeername`
  e `shutdown` funcionam; o caminho abstrato (que comeca com zero) nao.
  `SO_REUSEADDR` e aceito e nao muda nada (a regra de endereco em uso de um
  socket Unix e o proprio no existir).
- Criterio ("o `wm` e os apps conversam por socket Unix"): o `wm` escuta em
  `/run/wm.sock`, aceita clientes no mesmo `poll` das janelas e atende `term`,
  `procs`, `files <caminho>`, `list` e `ping`; o programa novo `wmctl` manda
  um comando e mostra a resposta. A regressao D digita `wmctl ...` no terminal
  do `wm` pelo passo novo `type:` dos harness (teclas pelo monitor do QEMU) e
  confere no log do `wm` que o terminal novo foi aberto a pedido.

## v2.2.0-a6 (etapa 3.6: wget, nc, httpd e telnet)

- Programas proprios em C, na musl, em vez de portar o `wget` do GNU, o `nc`
  do OpenBSD ou o `telnet` do inetutils: os de verdade puxam TLS, autoconf e
  bibliotecas que a fase 3 nao precisa, e o que o criterio pede (baixar uma
  pagina, ligar dois pontos, falar com um servidor de texto) cabe em poucas
  centenas de linhas. O `httpd` e o da 3.4.
- `wget`: so `http://`, HTTP/1.0 (para nao precisar de `chunked`), segue ate
  5 redirecionamentos (`Location` absoluto ou so caminho; `https://` da erro
  claro), salva com o nome do fim do caminho (`index.html` quando nao ha),
  `-O arquivo` ou `-O -` para a saida, `-q`. Erro HTTP sai com 1 e a mensagem
  do codigo.
- `nc`: cliente (`nc host porta`) e servidor de uma conexao (`nc -l porta` ou
  `-l -p porta`), `-z` so testa a porta, `-w segundos` de espera sem dados. O
  programa sai quando o outro lado fecha; o fim da entrada so vira
  `shutdown(SHUT_WR)` com `-N`, como no `nc` do OpenBSD (na 3.7 a regressao
  achou a corrida que o meio-fechamento automatico causava num `nc -l` em
  segundo plano, que herda o terminal como entrada).
- `telnet`: liga, recusa toda opcao que o outro lado oferece (`WILL` ->
  `DONT`, `DO` -> `WONT`), pula subnegociacao, dobra o byte 255 e manda
  `\r\n` no fim de linha (NVT). Sem modo de caractere: a linha vai inteira.
- O `getaddrinfo` da musl le o `/etc/resolv.conf`, que existe desde a 3.2.
- Testes na sequencia A (o `sh` daqui nao trata aspas, entao os comandos foram
  escritos sem elas): `wget http://example.com` salva a pagina (o criterio;
  depende da internet do hospedeiro), `wget -O` traz 1 MiB do `hostweb` com o
  SHA-256 certo e trata o 404; `nc -l` e `nc` pelo loopback; `nc -z` em porta
  fechada; `nc` falando HTTP com o hospedeiro; `telnet` contra um `nc -l` que
  manda `IAC WILL ECHO`, conferindo que a tela mostra so o texto e que do outro
  lado chega `IAC DONT ECHO` seguido de `oi\r\n`.

## v2.2.0 (etapa 3.7: DHCP fora do kernel; fecha o M3)

- O cliente DHCP saiu do kernel: `net/dhcp.c`, o `ioctl` `SIOCDHCP_ORION`, o
  desvio da porta 68 no `net.c` e o comando `dhcp` do shell do kernel foram
  removidos. O programa `dhcp` faz o protocolo inteiro por um socket UDP comum
  (porta 68, `SO_BROADCAST`): DISCOVER e REQUEST para 255.255.255.255 com o
  bit de broadcast, espera OFFER e ACK com o mesmo `xid` (1, 2, 4 e 8 s por
  tentativa, tres rodadas), recomeca no NAK, configura a interface pelo
  `SIOCSIFCONF_ORION` (endereco, mascara, gateway, DNS) e grava o
  `/etc/resolv.conf` (`-n` deixa como esta). Nao ha renovacao da concessao: o
  SLIRP da 86400 s e o sistema nao fica ligado tanto tempo nos testes; quem
  precisar roda o `dhcp` de novo.
- O kernel ja sabia mandar para broadcast (o `net_resolve` devolve o MAC de
  broadcast para 255.255.255.255 e para o broadcast da sub-rede) e entregar
  UDP sem conferir o destino; o que faltava era o `SO_BROADCAST`: agora, como
  no Linux, mandar para um desses enderecos sem a opcao da `EACCES`.
- Configuracao em `/etc/network`, lida pelo `netup` (novo), que o
  `/etc/autostart` chama no boot (antes a rede so subia com `dhcp` a mao, ou
  no autostart grafico): `eth0 dhcp`, `eth0 static <ip> <mascara> <gateway>
  [dns]` ou `eth0 off`. Um arquivo por sistema e uma linha por interface,
  porque so ha uma placa; o formato e o do `/etc/network/interfaces` do Debian
  em uma linha so. `ifconfig dhcp` passou a chamar o `/bin/dhcp`.
- Criterio ("boot pega IP sem codigo de DHCP no kernel"): a regressao confere
  no log do boot o `init` rodando o `netup`, o `netup` escolhendo DHCP e o
  `dhcp` com o endereco, a mascara, o gateway, o DNS e o servidor, e que
  nenhuma linha do DHCP do kernel aparece.

## v2.3.0-a1 (etapa 4.1: uid, gid e permissoes)

- Credenciais por processo (`kernel/cred.c`): `uid`, `euid`, `suid`, `gid`,
  `egid`, `sgid` e ate 32 grupos suplementares, sob uma trava giratoria
  propria do processo; as threads compartilham (o `setuid` da musl chama a
  syscall em todas, e todas mexem no mesmo lugar). O `init` nasce root e o
  `fork` copia. No `exec`, arquivo com `S_ISUID` troca o `euid` pelo dono e
  com `S_ISGID` (e execucao do grupo) troca o `egid`; o `suid`/`sgid` viram o
  efetivo, como no Linux. A opcao `nosuid` da montagem continua sem efeito.
- Syscalls: `getuid`, `geteuid`, `getgid`, `getegid` deixaram de ser stub;
  `setuid`, `setgid`, `setreuid`, `setregid`, `setresuid`, `setresgid`,
  `getresuid`, `getresgid`, `getgroups` e `setgroups` (so root), com as
  regras do Linux para quem nao e root (so pode trocar para um dos tres ids
  que ja tem).
- `vfs_permission` confere os bits de dono, grupo (efetivo ou suplementar) e
  outros com o `vfs_getattr`. Root passa sem consultar nada, menos na
  execucao: arquivo comum precisa de algum bit `x`, como no Linux (por isso o
  root continua sem custo nenhum de `getattr` na resolucao de caminho, e a
  regressao inteira, que roda como root, nao mudou de tempo). Pontos
  conferidos: busca em cada diretorio do caminho (`x`), `open` (`r`/`w`, e
  `w` para `O_TRUNC`; o arquivo recem-criado nao e conferido), criar, apagar e
  renomear (`w` e `x` no diretorio; com `S_ISVTX`, so o dono do arquivo, o do
  diretorio ou root apagam e renomeiam), `exec` (`x`), `chdir`/`fchdir` (`x`),
  `truncate` (`w`), `access`/`faccessat` com os ids reais (`AT_EACCESS` usa os
  efetivos), `chmod` (so o dono ou root; quem nao e do grupo do arquivo perde o
  `S_ISGID`), `chown` (so root troca o dono; o dono troca o grupo para um
  grupo dele), `utimensat` (hora explicita so o dono ou root; "agora" tambem
  quem pode escrever), `kill`/`tkill`/`tgkill` (mesmo usuario real ou efetivo,
  ou root; o `si_uid` passou a ser o real), `mount`/`umount` (root) e `bind`
  em porta abaixo de 1024 (root).
- Arquivo, diretorio, link e socket novos sao do `euid`/`egid`; em diretorio
  com `S_ISGID`, herdam o grupo do diretorio, e um subdiretorio herda o
  proprio `S_ISGID`. O dono e gravado com um `setattr` logo depois do
  `create` (so quando nao e root com grupo 0, para nao gastar nada no caso
  comum).
- FAT: arquivos passaram a `0755` (diretorios ja eram), que e o que o `vfat`
  do Linux mostra com `umask` 022; com o `exec` exigindo `x`, binarios no FAT
  (o `vvfat` do `build.ps1 -Run`) teriam parado de rodar. O somente leitura
  virou `chmod 555`/`755`. O `cp` passou a copiar os bits de permissao da
  origem (antes criava tudo `0644`), e o `mkext2.py` marca ELF como `0755`
  nas imagens de teste.
- `/etc/shadow` entrou (`0600` de root na imagem, pelo `mkimage.py`), com a
  senha do root travada ate a 4.2. `/dev/tty` passou a `0666`, como no Linux.
- Teste: `permtest` (novo) comeca como root, prepara arquivos e diretorios
  com varios modos e donos, e um filho vira o usuario 1000 do grupo 100 e
  confere cada regra acima, inclusive o criterio da etapa ("usuario comum nao
  le o `/etc/shadow`") e um binario `setuid` de root rodando com `euid` 0;
  roda no tmpfs e na raiz ext2. `id` (novo) mostra os ids com nomes.

## v2.3.0-a2 (etapa 4.2: passwd, shadow, login, su)

- `login`, `su` e `passwd` sao um programa so (`user/musl/login`, com
  `link su passwd` no `build.txt`) que escolhe o papel pelo `argv[0]`; a
  parte comum (ler linha sem eco, conferir a senha, trocar de usuario, subir o
  shell) fica em `auth.c`. Na imagem, `su` e `passwd` sao `setuid` root
  (`04755`, pelo `SPECIAL_MODES` do `mkimage.py`); o `login` nao precisa,
  porque so o `init` (root) o roda.
- Hash: SHA-512 do `crypt` da musl (`$6$`, 5000 rodadas, sal de 16
  caracteres do `/dev/urandom`). O plano aceitava "SHA-256 da musl ou
  proprio"; o `$6$` e o padrao das distribuicoes e a musl ja traz, entao nao
  ha codigo de hash nosso no sistema. O `getspnam` e o `getgrouplist` tambem
  sao os da musl, lendo `/etc/shadow` e `/etc/group`.
- O `/etc/shadow` do repositorio vem com `root` e `orion` (uid 1000, grupo
  100 `users`, `HOME` `/home/orion`, dono dele na imagem), os dois com senha
  `orion`. Os hashes foram gerados com um sal fixo pelo `tools/mkpasswd.py`
  (novo, SHA-512-crypt em Python puro, conferido contra o vetor de teste da
  especificacao), para que o arquivo nao mude a cada build. Senha padrao
  conhecida num sistema de estudo e o que da para fazer sem instalador; o
  README manda trocar com `passwd`.
- `passwd` regrava o arquivo inteiro em `/etc/shadow.novo` (`0600`), faz
  `fsync` e troca pelo `rename`, que e atomico na VFS; uma queda no meio deixa
  o arquivo antigo. O temporario recebe o dono, o grupo e o modo do original
  (sem isso, o `passwd` rodado pelo `orion` deixava o arquivo com o grupo
  `users`, que e o `egid` dele). Usuario comum precisa da senha atual; root
  troca a de qualquer um sem ela. O `su` sem senha so para root.
- O `getgrouplist` da musl repete o grupo primario quando o usuario tambem
  aparece como membro dele no `/etc/group`; o `login`/`su` tiram as
  repeticoes antes do `setgroups`.
- O `init` sobe o `login` (`shell /bin/login` no `/etc/autostart`) e o
  reinicia quando o shell do usuario termina. O `login` ignora `Ctrl+C` ate
  trocar de usuario, recusa depois de 3 senhas erradas (o `init` sobe outro)
  e espera 1 s a cada erro. A senha e lida com o `ECHO` desligado pelo
  `tcsetattr`, que o terminal do console ja respeitava. O `build.ps1 -Gui`
  continua indo direto para a interface grafica como root: tela de login
  grafica nao entrou nesta etapa.
- `/dev/urandom` entrou (o mesmo gerador do `/dev/random`), porque e onde a
  musl e os programas procuram aleatoriedade.
- Harness: o `test.ps1` (parametros novos `-LoginUser`/`-LoginPassword`,
  padrao `root`/`orion`) e o `tools/qrun.py` respondem ao `login:` sozinhos,
  no boot e depois do `reboot:`; o resto da regressao continua num shell de
  root, como antes. O `mkext2.py` passou a gravar dono e grupo por no.
- Teste: na sequencia A o boot entra pelo `login` (o padrao confere que a
  senha nao aparece entre `Senha:` e `Bem-vindo`), e depois um `login` errado
  e certo como `orion`, `su` errado e certo, `passwd` pelo proprio usuario (a
  senha antiga deixa de valer), `/etc/shadow` com `$6$` de sal novo e ainda
  `0600`, root trocando a senha de volta sem a atual e `su` de root sem senha.
  Uma verificacao negativa garante que nenhum texto digitado aparece depois de
  um pedido de senha em toda a sequencia.

## v2.3.0-a3 (etapa 4.3: SMEP e SMAP)

- `cpu_protect_init` (`arch/x86_64/protect.c`) liga `CR4.SMEP` e `CR4.SMAP`
  em cada CPU quando o CPUID (folha 7) mostra suporte; o boot registra
  "SMEP ligado, SMAP ligado" ou "ausente na CPU". Sem suporte o kernel roda
  como antes.
- As unicas portas do kernel para a memoria de usuario continuam sendo as
  duas rotinas de `uaccess.S` (a copia e a de string, com a tabela de
  excecoes da 0.2): elas fazem `stac` antes e `clac` depois, inclusive no
  caminho de falha. Como `stac`/`clac` sao instrucoes invalidas em CPU sem
  SMAP, elas sao puladas por um teste de `smap_enabled` em vez de remendo de
  codigo em tempo de execucao; o custo e uma comparacao por copia. A entrada
  do `syscall` ja limpava o `AC` pelo `FMASK`; a entrada comum de
  interrupcoes e excecoes passou a fazer `clac` (uma interrupcao no meio de
  uma copia, ou um programa que liga o `AC` no proprio `RFLAGS`, nao deixa o
  kernel com o acesso aberto). O `iretq` devolve o `AC` de quem foi
  interrompido.
- Page fault em ring 0 num endereco de usuario: busca de instrucao vira
  "SMEP: o kernel tentou executar codigo de usuario"; qualquer acesso com o
  `AC` desligado (com SMAP ligado) vira "SMAP: o kernel acessou memoria de
  usuario fora das copias de uaccess". Nos dois casos o tratador nao tenta o
  `vma_fault` nem a tabela de excecoes (seria um laco de faltas numa pagina
  presente) e cai no panico normal, com a linha a mais explicando a causa.
- Teste: syscall propria `kfault` (1002), so para root, que de proposito
  chama ou le um endereco de usuario dentro do kernel (devolve
  `EOPNOTSUPP` se a protecao correspondente nao estiver ligada, para nunca
  executar codigo de usuario em ring 0 de verdade). `crash smep` e
  `crash smap` usam essa syscall; a sequencia B termina com os dois, com um
  `reboot:` entre eles. O equivalente no Linux e o `lkdtm`.
- QEMU: `-cpu qemu64,+smep,+smap` no `build.ps1` (parametro `-Cpu`), no
  `test.ps1` e no `qrun.py`; o TCG emula as duas. A regressao inteira roda
  com SMAP ligado, o que confere que nenhum caminho do kernel le memoria de
  usuario sem passar pelo `uaccess`.

## v2.3.0 (etapa 4.4: ASLR basico; fecha a Fase 4)

- A cada `exec`, o topo da pilha desce um numero aleatorio de paginas, de 0
  a 2^22 - 1 (ate 16 GiB abaixo de `0x7ffffffff000`, como os 22 bits do
  Linux), e a base de busca do `mmap` sem endereco sobe de 0 a 2^28 - 1
  paginas a partir de `0x100000000000` (ate 1 TiB, os 28 bits do Linux). O
  sorteio usa o `random_u64` do kernel, o mesmo do `/dev/random`.
- A base do `mmap` fica no processo (`mmap_base`): o `fork` copia (o filho
  tem o mesmo espaco de enderecos), o `exec` sorteia de novo e, se falhar,
  devolve a antiga. Se nao houver espaco acima da base sorteada, a busca
  recomeca do inicio da faixa, entao a faixa util do `mmap` nao encolheu.
- A conferencia do ELF contra a pilha usa o ponto mais baixo que a pilha pode
  ocupar (topo menos o deslocamento maximo menos os 8 MiB de crescimento).
- Ficaram de fora: executavel em posicao aleatoria (os programas sao `ET_EXEC`
  estaticos em `0x1000000`; precisaria de PIE e de carregar `ET_DYN`), `brk`
  aleatorio, deslocamento dentro da pagina do topo da pilha, e um jeito de
  desligar (`setarch -R` / `personality`).
- Teste: `aslrtest` (novo) roda a si mesmo 4 vezes e confere que a pilha e o
  primeiro `mmap` mudam a cada `exec` e ficam dentro das faixas; a regressao
  tambem confere o criterio da etapa, dois `cat /proc/self/maps` seguidos com
  a pilha em enderecos diferentes. O padrao antigo que fixava a pilha em
  `7ffffffff000` virou uma faixa.

## v2.3.1 (etapa 7.1: ISO hibrida)

- Numeracao: a 7.1 e a 7.2 vieram antes das Fases 5 e 6 por causa da ordem
  recomendada do plano ("7.1 e 7.2 cedo"). Para a versao continuar sempre
  crescente sem tomar a 2.4 (Fase 5) nem a 3.0 (Fase 7 inteira), elas viram
  2.3.1 e 2.3.2; a 5.1 abre a 2.4.0-a1.
- `tools/mkiso.c` (novo, C99 puro, compila no Windows com o `zig cc` e no
  Linux com o `cc`) monta a ISO a partir de um diretorio: ISO9660 nivel 2
  (nomes de ate 31 caracteres em maiusculas, `;1` nos arquivos, repeticoes
  desfeitas com `~N`), Rock Ridge (`SP` e `ER` na raiz, `PX` e `NM` em cada
  registro, para os nomes reais em minusculas e os modos), as duas tabelas de
  caminhos, e El Torito com duas entradas: BIOS sem emulacao com o
  `limine-bios-cd.bin` (4 setores de 512, com a "boot info table" que o
  Limine exige) e uma secao UEFI (plataforma `0xEF`) com o
  `limine-uefi-cd.bin`, que e uma imagem FAT com o `BOOTX64.EFI`. Sem Joliet
  nem HFS+: ninguem que le esta ISO precisa.
- Hibrida: os 32 KiB iniciais (a area de sistema, que o ISO9660 nao usa)
  levam um MBR com uma unica particao, tipo `0xEF`, apontando para a imagem
  EFI; e o que faz o firmware UEFI achar o Limine quando a ISO vai para um
  pendrive. Depois o `limine bios-install` (o utilitario do proprio Limine,
  `third_party/limine/limine.c`, compilado do mesmo jeito em
  `out/tools/limine.exe`) poe o estagio 1 no MBR e o estagio 2 nos setores 1 a
  62, o que exige que nenhuma particao comece antes do setor 63; a EFI comeca
  bem depois. O Limine le o ISO9660 do disco inteiro nos dois firmwares,
  entao `boot():/boot/orion.elf` funciona igual em CD e em pendrive.
- `build.ps1 -Iso` compila as duas ferramentas do hospedeiro (so quando o
  fonte muda), monta `out/iso` (kernel, `limine.conf`, os tres arquivos do
  Limine) e gera `out/orion.iso`; `-Run -Iso` boota como CD e `-Bios` troca o
  OVMF pelo SeaBIOS. No Linux, o `qrun.py` faz o mesmo (`make_iso`).
- O kernel registra de onde foi carregado ("kernel lido de /boot/orion.elf no
  CD, sem particao"), pela requisicao `executable_file` do Limine.
- O NVRAM do OVMF guarda a ordem de boot. Com o arquivo de variaveis
  compartilhado com os boots do disco, o OVMF tentava o disco antigo e caia
  no "EFI Internal Shell" em vez do CD; e um boot do CD tambem gravava la a
  ordem nova, que depois desviava os boots do disco. O `test.ps1` e o
  `qrun.py` passaram a copiar as variaveis limpas do QEMU a cada partida (o
  harness nunca dependeu de nada guardado nelas), e o `build.ps1 -Run -Iso`
  usa um arquivo separado (`out/ovmf-vars-iso.fd`), para nao mexer no do
  `-Run` normal.
- Harness: `test.ps1 -Iso`, `-IsoDisk` (a ISO como disco, que e o que um
  pendrive gravado com `dd` ou Rufus em modo DD parece para o firmware) e
  `-Bios`, e o passo `reboot:` aceita opcoes (`reboot:uefi,cd`,
  `reboot:bios,isodisk`) para trocar firmware e midia no meio do roteiro. O
  `qrun.py` e o `regress.py` ganharam o mesmo. A sequencia F (nova) boota a
  ISO nos quatro modos e confere, em cada um, o firmware, a midia e o kernel
  chegando ao fim do boot; enquanto nao ha raiz (7.2), ele para no shell do
  kernel. O `check.ps1` passou a compilar os `tools/*.c` com `-Wall -Wextra`.
- VirtualBox nao esta instalado nesta maquina; o teste nele (parte do
  "pronto quando") fica para o dono.

## v2.3.2 (etapa 7.2: sistema live)

- A ISO leva `boot/initramfs.cpio`, carregado pelo Limine como modulo
  (`module_path` acrescentado ao `limine.conf` so na copia da ISO). Formato
  cpio `newc` (o do Linux): cabecalho ASCII de 110 bytes, nome, dados, tudo
  alinhado em 4. Sem compressao: o Limine nao descomprime e um descompressor
  no kernel nao paga 49 MiB numa ISO de 56 MiB.
- `tools/mkinitrd.py` (novo) monta o cpio a partir da mesma arvore que o
  `mkimage.py` usa para a raiz ext2 (`root_tree`), entao modos, donos,
  `setuid` do `su`/`passwd`, `/etc/shadow` `0600` e `/home/orion` do usuario
  1000 saem iguais nos dois; as datas sao a constante do `mkext2.py`, para o
  arquivo nao mudar a cada build.
- `vfs_init` tenta o initramfs antes dos discos: se o modulo `initramfs`
  existe, monta um tmpfs como raiz e desempacota nele (`fs/initramfs.c`,
  diretorios, arquivos e links, com `setattr` de modo, dono e data); o resto
  do boot (devfs, `/tmp`, `/run`, `/proc`, `/mnt`) segue igual. Uma entrada
  ruim gera aviso e o resto continua; um cabecalho corrompido para o
  desempacotamento com o que ja veio.
- Depois de desempacotar, a memoria do modulo (que o Limine marca como
  "kernel/modules" e o `pmm` nunca contou) entra no alocador por
  `pmm_add_region`: os dados existem uma vez so, no tmpfs. No QEMU com 512 MiB,
  o live sobe com 518 MiB de total e 53 MiB usados.
- A raiz live e so de RAM: nada sobrevive ao reboot, e e isso que a regressao
  confere. Com a ISO gravada num pendrive o kernel tambem monta a FAT da
  particao EFI em `/mnt`, inofensivo.
- Teste: a sequencia F entra pelo `login` nos quatro boots e confere em cada
  um a raiz `initramfs / tmpfs`; no primeiro, `/bin` inteiro, donos e modos,
  escrita e leitura na raiz, `hello`, `lua`, `su`; no segundo, que o arquivo
  gravado antes sumiu; no ultimo, o wad do Doom.

## v2.4.0-a1 (etapa 5.1: uACPI)

- uACPI 6.1.1 (MIT) em `third_party/uacpi` (so `include`, `source`, licenca
  e README, sem mudar uma linha), compilado junto do kernel pelo `build.ps1`
  e pelo `Makefile` com as mesmas flags, sem nenhum warning. A 6.1.1 e uma
  versao de correcao de duas semanas; a 7.0.0 tinha saido dois dias antes.
- `drivers/acpi_os.c` implementa a interface `uacpi_kernel_*`: tabelas e
  regioes de memoria pelo HHDM quando a pagina ja esta mapeada e pela janela
  de MMIO quando nao esta (com um cache, porque a janela so cresce e o
  `unmap` e vazio), PCI pelo espaco de configuracao legado (leituras e
  escritas de 8 e 16 bits viram 32 bits com mascara), portas de E/S, mutex
  com prazo (tentativas de 1 ms), eventos como semaforos com fila de espera,
  spinlocks, o tempo do TSC, e uma thread `acpi-work` que executa os
  trabalhos adiados (metodos de GPE e `Notify`). A interrupcao do SCI e uma
  IRQ ISA (a 9 no QEMU), pelo mesmo caminho das outras.
- `drivers/power.c`: `power_init` roda logo depois do `pci_init` (antes dos
  drivers, para eles poderem pedir rotas): `uacpi_initialize`, carga do
  namespace, `_PIC(1)` quando as IRQs vao pelo I/O APIC (precisa vir depois
  da carga e antes da inicializacao; na ordem errada o uACPI recusa e o `_PRT`
  devolve as rotas do PIC), inicializacao do namespace, GPEs, o evento fixo do
  botao de energia e a tabela `_PRT` da ponte PCI raiz (`PNP0A08`/`PNP0A03`).
  No QEMU leva uns 60 ms.
- Rotas de IRQ do PCI: `power_pci_route` acha a entrada do `_PRT` para o
  slot e o pino; com fonte, le a IRQ, o disparo e a polaridade do `_CRS` do
  link. A e1000 passou a usar a GSI que o `_PRT` da (22 no q35, nivel, ativa
  em baixo), roteada pelo `irq_attach_gsi` num vetor a partir de `0x50`; se
  nao houver rota, volta para a linha de IRQ do espaco de configuracao.
- `poweroff`: grava os discos (`vfs_sync`), `uacpi_prepare_for_sleep_state`
  (S5, roda o `_PTS`), desliga as interrupcoes, para as outras CPUs e
  `uacpi_enter_sleep_state`. `reboot`: grava os discos e `uacpi_reboot`
  (registrador de reset do FADT); se voltar, o controlador do teclado e, por
  ultimo, a IDT vazia. O botao de energia agenda o desligamento na thread de
  trabalho (o evento chega dentro da interrupcao).
- Syscall `reboot` (169) com os numeros magicos e os comandos do Linux
  (`RESTART`, `POWER_OFF`, `HALT`; `CAD_ON`/`CAD_OFF` aceitos sem efeito), so
  para root. Programa `poweroff` (com `reboot` e `halt` pelo `argv[0]`): grava
  e chama a syscall; quem nao e root recebe o `EPERM` do kernel. O shell do
  kernel ganhou `poweroff`, e o `reboot` dele passou pelo mesmo caminho.
- Harness: passo `monlog:<comando>`, que manda o comando ao monitor do QEMU e
  grava a resposta no log. Com `-no-reboot -no-shutdown` o QEMU pausa tanto
  no desligamento quanto no reset ("paused (shutdown)"); a regressao separa os
  dois pela linha do kernel (S5 ou registrador de reset) e confere que o
  reboot nao precisou do controlador do teclado.
- Teste no VirtualBox: nao ha VirtualBox nesta maquina; fica para o dono.

## v2.4.0-a2 (etapa 6.1: protocolo de janelas)

- Numeracao: a Fase 6 e a 2.5 no plano, mas a Fase 5 nao terminou; a 6.1
  segue a numeracao corrente (2.4.0-a2), como a 7.1 e a 7.2 seguiram a 2.3.
- Memoria compartilhada: `MAP_SHARED` com escrita passou a valer para
  arquivos do tmpfs. O `fs_type` ganhou `getpage`, que devolve a pagina do
  proprio arquivo com uma referencia a mais (alocando o buraco, se houver);
  o `vma_fault` mapeia essa pagina direto, gravavel, e o `munmap`, o `exit` e
  o `fork` ja tratavam paginas com contagem de referencias. O arquivo e a
  janela veem os mesmos bytes, sem copia. Fora do tmpfs (ext2, FAT) a escrita
  compartilhada continua `ENODEV`: sem rastrear paginas sujas, gravar de
  volta no disco seria fingir. Com o arquivo aberto so para leitura,
  `PROT_WRITE` da `EACCES`, como no Linux.
- Por que um arquivo e nao `memfd`/`SCM_RIGHTS`: o sistema ainda nao passa
  descritores por socket Unix, e um arquivo em `/run/wm` (tmpfs) com o
  caminho na resposta do `wm` e o "objeto do wm" do plano sem mecanismo novo
  no kernel. O `wm` cria o arquivo, o mapeia tambem e o apaga quando a
  janela fecha.
- Protocolo (`/run/wmapp.sock`, socket Unix proprio, conexoes persistentes,
  uma linha de texto por mensagem): o app manda `janela <l> <a> <titulo>` e
  recebe `ok <id> <arquivo> <l> <a>`; desenha em `0x00RRGGBB` e manda `mostra`
  (o `wm` marca a area da janela como suja e copia convertendo para o formato
  do framebuffer); `titulo <texto>` e `fecha`. O `wm` manda `tecla <ascii>
  <scancode> <mods>` para a janela em foco, `clique <x> <y> <botoes>` em
  coordenadas da janela, e `fechar` quando a janela e fechada no `x` ou no
  Ctrl+Q; se o app sai, a conexao cai e a janela some. Texto e nao binario
  porque sao poucas mensagens por quadro e fica facil de ler no log.
- O socket de comandos (`/run/wm.sock`, do `wmctl`) continuou como era: uma
  linha e a conexao fecha.
- `hello-gui` (novo): abre uma janela 360x200 com um degrade, conta teclas e
  cliques, marca os cliques, troca o titulo a cada evento e fecha com `q`.
- Teste: `shmtest` (novo, sequencia A) confere o `MAP_SHARED` de arquivo
  entre `read`/`write`, entre processos que mapeiam o mesmo arquivo e entre
  pai e filho, o `EACCES` e o `ENODEV` no ext2. A sequencia D abre o
  `hello-gui` pelo terminal grafico, manda duas teclas e um clique e confere
  pelo log do `wm` que cada evento foi e voltou (o titulo muda), a captura de
  tela com o degrade, e o fechamento com `q`.

## v2.4.0-a3 (etapa 6.2: libgui)

- `user/libgui` (`gui.h`, `gui.c`) fica fora de `user/musl` para nao virar
  um programa; cada app que a usa poe no `build.txt` `flag -I../../libgui
  -I../wm`, `src ../../libgui/*.c` e `dep ../../libgui/*.h` (o mesmo formato
  que o `build.ps1` e o `tools/musl_build.py` ja entendiam). Ligada
  estaticamente, como o resto. A fonte e a mesma 8x8 do `wm`, em dobro.
- Esconde o protocolo da 6.1 (`gui_open`, `gui_run`, `gui_set_title`) e da
  componentes sem alocacao dinamica, num vetor fixo por janela: rotulo,
  botao, campo de texto (com cursor, Backspace e Enter), caixa de opcao,
  lista com barra de rolagem (setas e Page Up/Down pelo teclado; setas da
  barra e clique no item pelo mouse) e barra de menus com menus suspensos.
- Teclado em tudo: Tab troca o foco (o componente em foco ganha borda
  amarela), Enter ou espaco aciona, F10 abre o menu, setas andam nele, Esc
  fecha. Alem de acessivel, e o que deixa o teste deterministico: o QEMU
  mistura movimentos relativos de mouse em fila (um deslocamento grande
  seguido de outro acabou num lugar errado na primeira tentativa), entao a
  regressao usa o mouse so em movimentos curtos a partir de uma posicao
  conhecida.
- O app redesenha a janela inteira a cada evento e manda `mostra`; o `wm`
  so copia a area da janela. Areas sujas no app ficam para a 6.5.
- `SIGPIPE`: o `wm` passou a ignorar (escrever para um app que acabou de
  sair o mataria) e devolve o padrao aos shells que abre, porque o `SIG_IGN`
  passa pelo `exec`; a `libgui` tambem ignora e nao responde depois de
  receber `fechar`.
- `guidemo` (novo) usa todos os componentes e troca o titulo a cada acao,
  que e o que o log do `wm` mostra e a sequencia D confere: texto, botao pelo
  Tab e Enter, caixa de opcao, lista com setas e Page Down, botao que
  acrescenta item, menu Ajuda pelo mouse e menu Arquivo pelo teclado.

## v2.4.0-a4 (etapa 6.3: fontes TrueType)

- `stb_truetype.h` 1.26 (commit `6e9f34d` do `nothings/stb`, MIT ou dominio
  publico) em `third_party/stb`, sem mudanca; entra so no `wm` (`wm/font.c`
  define a implementacao, com `STBTT_STATIC`, e desliga tres avisos do
  cabecalho com `#pragma clang diagnostic` em volta do `#include`, para o
  `check.ps1` continuar limpo sem mexer no arquivo de terceiros).
- Fontes: DejaVu Sans Mono (terminal) e DejaVu Sans (barra e titulos) da
  DejaVu 2.37 (licenca Bitstream Vera com as alteracoes da DejaVu em dominio
  publico), so os dois `.ttf` usados mais licenca, autores e README, em
  `third_party/dejavu` (1,1 MB, versionados como o resto de terceiros). O
  `build.ps1` e o `tools/musl_build.py` copiam para `/usr/share/fonts`.
- Cache de glifos: os 95 caracteres ASCII de cada fonte sao rasterizados uma
  vez, na primeira vez que aparecem, e guardados como mapa de alfa; desenhar
  e misturar o alfa com o fundo (`gfx_blend_alpha`, que desfaz o formato do
  framebuffer, mistura em 8 bits por canal e refaz). Fora do ASCII vira `?`,
  como na fonte 8x8.
- A celula do terminal sai da metrica da fonte (avanco do `M` e altura da
  linha): com 20 px fica 10x21. A area suja das linhas do terminal passou a
  usar essa altura; com a altura antiga ficavam restos do cursor. Paineis
  (processos, arquivos, menu, "Sobre") continuam na fonte 8x8: as janelas
  deles vao ser refeitas como apps na 6.6.
- Sem os `.ttf` (por exemplo, um `esp` antigo), o `wm` avisa e volta para a
  fonte 8x8 inteira.
- Prova do antialias na regressao: o `wm` relata, ao carregar, quantos
  niveis de alfa o glifo `A` tem (dezenas; uma fonte de bitmap teria um so),
  e no fim quantos glifos foram para o cache. A amostra de cores da captura
  nao serve aqui: ela pega um pixel a cada 997 e quase nunca cai numa borda
  de letra.

## v2.4.0-a5 (etapa 6.4: imagens PNG e JPEG)

- `stb_image.h` 2.30 (commit `013ac3b` do `nothings/stb`, MIT ou dominio
  publico) em `third_party/stb`, sem mudanca, so com PNG e JPEG
  (`STBI_ONLY_PNG`, `STBI_ONLY_JPEG`, sem HDR). Compilado no `wm`
  (`wm/img.c`) e no `imginfo`, com os mesmos `#pragma` da 6.3.
- Papel de parede: `tools/mkwallpaper.py` (novo, Python puro com `zlib`, sem
  PIL) desenha uma nebulosa com estrelas e a constelacao de Orion em 1280x800
  e grava um PNG de verdade; o build so refaz quando o script muda. Fica em
  `/usr/share/wallpapers/orion.png`. O `wm` carrega pelo `stb_image`, estica
  para a tela se o tamanho for outro (vizinho mais proximo), converte uma vez
  para o formato do framebuffer e copia linha a linha no fundo; sem o
  arquivo, volta ao degrade.
- `share/` (novo no repositorio) e copiado para `/usr/share` pelo build, como
  o `etc/` vai para `/etc`. Leva as imagens de teste: um JPEG e um PNG
  pequenos com quatro quadrantes de cores conhecidas, gerados uma vez com o
  PIL e versionados (o build nao depende do PIL).
- `imginfo` (novo): decodifica e mostra formato, tamanho, canais, cor media e
  a cor de cada quadrante; e o teste de JPEG da regressao. Icones ficaram para
  o lancador da 6.7.
- A verificacao de cores da captura do `hello-gui` (6.1) perdeu forca com o
  papel de parede, que sozinho ja da centenas de cores na amostra; o
  `hello-gui` continua provado pelas linhas do log do `wm`. A captura do boot
  grafico passou a conferir o papel de parede (250 cores ou mais; sem ele
  eram 75).

## v2.4.0-a6 (etapa 6.5: compositor com areas sujas)

- O que a etapa pede ja existia em boa parte desde o `wm` da Fase 1: o `wm`
  desenha num back buffer e so copia para o framebuffer os retangulos sujos
  (ate 24 por quadro, juntados quando passam disso), e o cursor e desenhado
  por ele por cima, sujando so os dois retangulos do cursor ao mover. A
  medida veio primeiro: parado, o `wm` gasta 0,2 a 0,3% de CPU (2 ou 3 tiques
  de 10 ms em 10 s), porque so o relogio e a barra mudam, uma vez por
  segundo, e o `poll` dorme 250 ms entre as voltas.
- O que faltava eram areas sujas dentro dos apps: o `mostra` da 6.1 sempre
  fazia o `wm` copiar a janela inteira. O protocolo ganhou `mostra <x> <y>
  <l> <a>` (so aquela area, em coordenadas da janela; `mostra` sozinho
  continua valendo a janela toda). A `libgui` guarda uma assinatura do estado
  visual de cada componente (texto, selecao, rolagem, caixa marcada, foco) e,
  a cada evento, redesenha so os que mudaram e manda a area que os cobre; um
  menu abrindo ou fechando ainda redesenha tudo, porque cobre os outros. No
  roteiro da sequencia D, o `guidemo` mandou 16 dos 24 quadros como parciais
  e o `wm` copiou 2,6 milhoes de pixels, contra 5,8 milhoes com quadros
  inteiros.
- O `wm` conta, por app, quadros, quadros parciais e pixels copiados, e
  escreve no log quando o app sai.
- `cpuuso <nome> [segundos]` (novo) le `utime` e `stime` do
  `/proc/<pid>/stat` antes e depois e da a porcentagem. Na sequencia D roda no
  terminal grafico com a saida em `/dev/tty` (o terminal fisico, que e a
  serial): o passo `type:` do harness aprendeu `>`, `<`, `|` e `&`.
- Achado da regressao desta etapa: o teste de `mremap` do `mhello` falhava
  em cerca de 1 de cada 8 execucoes desde o ASLR da 4.4 (as tres regressoes
  seguintes passaram por sorte). Ele mapeava 4 paginas e contava com a pagina
  seguinte livre para pendurar ali um "bloqueador" com `MAP_FIXED_NOREPLACE`;
  com a base do `mmap` sorteada, as 4 paginas as vezes caiam num buraco do
  tamanho exato e o bloqueador dava `EEXIST`. Nada no `mmap` garante a pagina
  seguinte livre, entao o erro era do teste: agora ele mapeia 5 paginas e troca
  a quinta por um mapeamento separado com `MAP_FIXED`, e 40 execucoes seguidas
  passaram. O teste tambem deixou de ler `MAP_FAILED + 8192` quando uma
  verificacao anterior falha, e mostra os enderecos quando o `mremap` falha.

## v2.4.0-a7 (etapa 6.6 parte 1: visualizador de imagens)

- A 6.6 ("um app por etapa") vai em partes, um app por commit, cada um com
  captura na sequencia B.
- `libgui`: componente tela (`gui_canvas`), em que o proprio app desenha por
  uma funcao de pintura; o app aumenta `version` quando o desenho muda, e a
  assinatura das areas sujas da 6.5 passa a incluir esse numero, entao a tela
  so e redesenhada quando muda. Tela nao recebe foco do Tab; clique nela chama
  a acao com a posicao em `click_x`/`click_y`. Tambem `gui_blit` (copiar um
  bloco de pixels) e o gancho `on_key`, que deixa o app tratar teclas antes
  dos componentes (o `imgview` usa para `q`, `f` e as setas).
- `imgview` (novo): abre um PNG ou JPEG, ajustado a area de 640x380 mantendo
  a proporcao (vizinho mais proximo) ou em 1:1, e anda pelas outras imagens da
  mesma pasta com as setas ou os botoes. Decodifica com o `stb_image`, como o
  `wm`. O titulo traz nome e tamanho.
- Regressao: `Test-ShotPixel` (novo, no `regressao.ps1` e no `regress.py`)
  confere a cor de um pixel da captura com tolerancia; e o que prova que a
  imagem certa esta na tela, ja que a contagem de cores ficou inutil com o papel
  de parede. A sequencia B abre o `imgview` no JPEG de teste, confere os
  quadrantes vermelho e amarelo na captura, passa para o PNG pela seta e fecha
  com `q`.

## v2.4.0-a8 (etapa 6.6 parte 2: monitor do sistema)

- `/proc/stat` (novo): linha `cpu` com o total e uma `cpuN` por CPU, no
  formato do Linux (user, nice, system, idle e zeros, em centesimos de
  segundo), mais `ctxt`. Os tiques vem do proprio `sched_tick`: cada CPU conta
  em `percpu` os tiques em modo usuario, no kernel e na thread ociosa. Contar
  no tique, e nao pelo `run_us` da thread ociosa, deixa os tres numeros na
  mesma base e sem trava extra. Nao ha `nice`, `iowait` nem `irq` separados.
- `libgui`: `on_tick` e `tick_ms` na `struct gui`. O `gui_run` espera o socket
  com `poll` ate o proximo tique (relogio `CLOCK_MONOTONIC`), chama o `on_tick`
  e redesenha; sem `on_tick`, nada muda para os outros apps. O `version` da
  6.6 parte 1 serve para qualquer componente: a lista do monitor o aumenta a
  cada amostra, porque a assinatura nao olha o texto dos itens.
- `monitor` (novo): uma amostra por segundo. CPU total pelo `/proc/stat`,
  memoria pelo `/proc/meminfo` (usada = total - livre) e processos pelo
  `/proc/<pid>/stat` (estado, RSS e CPU de cada um pela diferenca de tiques).
  Graficos de 60 amostras em uma tela (`gui_canvas`). A lista guarda o
  processo escolhido pelo pid entre amostras; "Encerrar" manda `SIGTERM` e
  recusa o `init` e o proprio monitor. O titulo muda so quando muda o numero
  de processos, quando manda o sinal e quando o processo sai, para nao encher
  o log do `wm` a cada segundo; o processo morto conta como saido ja no estado
  `Z`, porque o shell so o colhe no proximo prompt.
- O `wm` corta o titulo em 31 caracteres; o titulo do sinal ficou curto
  (`monitor: TERM <pid> <nome>`).
- Regressao: a espera depois do `history` na sequencia A passou de 1 s para
  3 s. Nesse ponto o historico ja tem dezenas de linhas, e com a maquina
  carregada a lista ainda chegava pela serial quando o `forktest` era
  digitado; a verificacao falhava sem erro no Orion. A sequencia A confere o
  `/proc/stat`; a B abre o `monitor`, escolhe o `sleep 100 &` na lista pelo
  teclado, encerra, ve o processo sair, captura a tela e fecha com `q`. A
  linha do kernel sobre o `SIGTERM` pode cair entre os dois titulos do
  monitor no log, entao a verificacao aceita qualquer linha no meio.
- Regressao D: o comeco esperava tempos fixos (16 s de boot e 13 s para o
  `cpuuso wm 10`). Numa rodada com a maquina lenta, o shell ainda estava no
  `cpuuso` quando o `hello-gui &` foi digitado; ele abriu uns 10 s depois e
  recebeu as teclas do `guidemo`, e 13 verificacoes cairam juntas. Os dois
  passos agora esperam o que aconteceu de fato (`until:` pela linha do `wm`
  pronto e pela linha do `cpuuso`).

## v2.4.0-a9 (etapa 6.6 parte 3: gerenciador de arquivos)

- `libgui`: lista ganhou a acao `open`, separada da `action` de selecao.
  Enter na lista ou clique no item que ja estava escolhido chamam `open`; sem
  `open`, o Enter continua chamando a `action`, como no `guidemo`. Nao ha
  clique duplo: o protocolo do `wm` nao manda o tempo do clique.
- `arquivos` (novo): campo do caminho (Enter navega), lista com as pastas
  primeiro e o tamanho dos arquivos, `..` para subir (tambem Backspace e o
  botao "Subir"), "Nova pasta" com o nome do campo e "Apagar" que so apaga no
  segundo aperto sobre o mesmo item (`rmdir` ou `unlink`); mudar a escolha
  cancela. Imagens abrem no `imgview` por `fork` e `execl`; os filhos sao
  colhidos no tique de 1 s com `waitpid(WNOHANG)`. Outros arquivos so mostram
  o tamanho, ate o editor da proxima parte. As teclas `q` e Backspace valem
  quando o foco nao esta num campo de texto.
- O painel de arquivos do proprio `wm` (Ctrl+F) fica, porque a sequencia D o
  testa; trocar os atalhos do `wm` pelos apps e assunto do lancador da 6.7.
- Regressao B: abre `arquivos /usr/share`, entra em `imagens` pelo teclado,
  abre o `teste.jpg` (sobe o `imgview`), fecha, cria e apaga `pasta-nova`
  com a confirmacao e fecha com `q`; a captura confere o campo do caminho e
  o texto da lista.

## v2.4.0-a10 (etapa 6.6 parte 4: editor de texto)

- `editor` (novo, separado do `edit` de terminal): o texto e um vetor de
  linhas, cada uma com o proprio buffer que cresce por `realloc`; o desenho
  e numa tela (`gui_canvas`) com celulas de 16x18, rolagem vertical e
  horizontal para manter o cursor visivel, e o cursor e uma barra de 2 px.
  O `on_key` fica com todas as teclas (setas, Home, End, PgUp, PgDn, Delete,
  Backspace, Enter, Tab vira 4 espacos, Ctrl+S salva), entao os botoes
  "Salvar" e "Fechar" so respondem ao mouse; o clique na tela posiciona o
  cursor.
- Gravacao: `O_TRUNC` e as linhas com `\n` no fim de cada uma. Ao abrir, o
  `\r` e descartado e a linha vazia depois do ultimo `\n` nao vira linha nova,
  entao abrir e salvar sem mexer devolve o mesmo arquivo para texto com fim de
  linha do Unix. Arquivo que nao existe abre vazio e e criado no primeiro
  Ctrl+S.
- O titulo muda so quando o estado muda (`editor: nome`, `editor: nome *`
  com mudanca nao salva, `editor: salvou nome`), para nao encher o log do
  `wm` a cada tecla. Ctrl+Q fecha pelo `wm` sem perguntar; avisar de mudanca
  nao salva precisaria de o `wm` pedir licenca ao app antes de fechar.
- `arquivos`: arquivo que nao e imagem abre no `editor`.
- Regressao B: abre `/tmp/nota.txt` (novo), digita duas linhas, apaga com
  Backspace, sobe com a seta, vai ao fim com End, acrescenta um ponto, salva
  com Ctrl+S e fecha com Ctrl+Q; depois do `wm`, o `cat /tmp/nota.txt` na
  serial confere o que foi gravado, e a captura confere o cursor no fim da
  primeira linha.

## v2.4.0-a11 (etapa 6.6 parte 5: configuracoes)

- `wm`: o papel de parede deixou de ser fixo. Ao iniciar, le `/etc/wm.conf`
  (linhas `chave=valor`; por ora so `fundo=<imagem>` ou `fundo=nenhum`) e cai
  no `orion.png` se nao houver arquivo. O socket de comandos ganhou
  `fundo <imagem|nenhum>`: carrega a imagem, troca o fundo e redesenha a tela
  inteira; imagem que nao existe ou nao decodifica responde `erro:` e deixa o
  fundo como estava. O `wm` nao grava a configuracao; quem grava e o app.
- Um arquivo de texto em `/etc` em vez de Lua: e uma chave so, e o formato
  `chave=valor` deixa o `wm` sem depender do interpretador. Se as opcoes
  crescerem (tema, fontes), a troca para Lua fica para a etapa que precisar.
- `config` (novo): mostra versao (`/proc/version`), CPU (`/proc/cpuinfo`),
  memoria e tempo ligado (`/proc/meminfo`, `/proc/uptime`, a cada segundo) e a
  rede (`SIOCGIFCONF_ORION`, como o `ifconfig`); lista as imagens de
  `/usr/share/imagens` e `/usr/share/wallpapers` mais o degrade, marca a atual
  com `*`, e o Enter ou "Aplicar" manda `fundo` ao `wm` e so grava o
  `/etc/wm.conf` se o `wm` aceitou. O nome do programa e curto (`config`)
  porque o titulo do `wm` tem 31 caracteres.
- Regressao B: o `config` aplica o `teste.jpg` (a captura confere os
  quadrantes vermelho e verde no fundo da tela) e depois o `teste.png`, fecha
  com `q`; depois do `wm`, o `cat /etc/wm.conf` confere o arquivo e um `wm`
  novo, aberto e fechado pelo shell da serial, carrega o fundo dele.

## v2.4.0-a12 (etapa 6.6 parte 6: terminal como app)

- `libgui`: `on_watch` e `watch_fd` na `struct gui`. O `gui_run` poe o
  descritor no mesmo `poll` do socket do `wm` (e do prazo do `on_tick`); o
  socket tem a vez quando os dois estao prontos, e o app que receber fim de
  arquivo poe `watch_fd = -1` para nao girar em vazio.
- `terminal` (novo): abre a pty com `orion_openpty` e o `sh` como o `wm` faz,
  e reaproveita o `wm/term.c` (o mesmo interpretador ANSI do terminal do
  `wm`), compilado junto pelo `build.txt`. Desenha as celulas numa tela com a
  fonte 8x8 ampliada da `libgui` (16x18 por celula; a fonte vetorial so existe
  dentro do `wm`). As teclas estendidas viram as mesmas sequencias que o `wm`
  manda para os terminais dele.
- Tamanho 48x26: o `wm` recusa janela de app maior que 800x520, e a primeira
  tentativa (60x22, 988x424) foi recusada. O `sh` recebe `COLUMNS` e `LINES`
  no ambiente, alem do `TIOCSWINSZ`.
- O app fecha quando a pty da fim de arquivo ou quando o `waitpid` do tique
  (500 ms) ve o `sh` sair, o que cobre a pty que nao avisa no `poll`. Se a
  janela for fechada pelo `wm`, o app fecha a pty e mata o `sh` com
  `SIGKILL`.
- Regressao B: a primeira versao do teste gravava um texto num arquivo e o
  conferia depois; com a janela recusada, as teclas cairam no terminal do
  `wm` e a verificacao passou assim mesmo. O teste agora grava o
  `printenv COLUMNS`, que so da `48` no `sh` do app. Depois vem um `sleep 50`
  cortado por Ctrl+C (o `echo` seguinte so roda se o Ctrl+C funcionou), um
  `ls` para a captura (cursor na linha do prompt) e `exit`, que fecha a
  janela.

## v2.4.0-a13 (etapa 6.7 parte 1: barra de tarefas, Alt+Tab, minimizar e maximizar)

- A 6.7 vai em duas partes: esta (janelas) e o lancador de apps com a
  captura da sequencia D com 3 apps abertos.
- Minimizar marca a janela e a manda para o fundo da pilha; `desk_focused`,
  `desk_at` e o desenho pulam as minimizadas. Alt+Tab manda a janela em foco
  para o fundo e foca a que sobe; se ela estiver minimizada, e restaurada.
  Assim o Alt+Tab passa por todas, inclusive as minimizadas, e repetido
  N - 1 vezes volta a minimizada, sem precisar de uma lista separada.
- Barra de tarefas de 30 px embaixo, um botao por janela na ordem de
  criacao (o lugar de cada uma nao muda com o foco); clique restaura a
  minimizada, minimiza a que esta em foco e foca as outras. Ela e desenhada
  depois das janelas, entao fica sempre visivel.
- Maximizar so vale para o terminal do `wm`: ele ocupa a area de trabalho
  (entre a barra de cima e a de tarefas), o `term` muda de tamanho guardando
  o texto (`term_resize`, que sobe as linhas se o cursor ficaria de fora) e a
  pty recebe o tamanho novo (`TIOCSWINSZ`). Janelas de app tem o tamanho do
  buffer compartilhado escolhido pelo app, e o protocolo nao tem
  redimensionar; o botao de maximizar nem aparece nelas.
- Teclas: Alt+Tab, Alt+F9 (minimiza) e Alt+F10 (maximiza ou volta), alem
  dos botoes no titulo. As teclas deixam o teste da sequencia D sem depender
  da posicao do mouse, que o QEMU entrega por movimentos relativos.
- `WIN_MAX` de 8 para 16 (na D ja eram 7 janelas abertas) e
  `TERM_MAX_COLS` de 96 para 128 (o terminal maximizado tem 126 colunas).

## v2.4.0-a14 (etapa 6.7 parte 2: lancador de apps)

- O menu "Orion" do `wm` virou o lancador: terminal do `wm`, terminal app,
  arquivos, editor (`/root/notas.txt`), monitor, imagens (o `imgview` abre o
  papel de parede), configuracoes, sobre e encerrar. O `wm` lanca com `fork`
  e `execl` de `/bin/<app>`, fecha no filho o framebuffer, a entrada e as
  ptys (os sockets ja sao `CLOEXEC`) e colhe os filhos no laco que ja existia.
- Ctrl+P e Ctrl+F abrem o `monitor` e o `arquivos` no lugar dos paineis 8x8
  do proprio `wm`, que eram o que a 6.3 deixou para virar app. Os paineis
  ficam so pelo `wmctl procs` e `wmctl files`, para nao perder o que eles
  testam (protocolo de comandos, janela de painel).
- O menu ganhou teclado: Alt+F1 abre, setas escolhem, Enter lanca, Esc fecha;
  enquanto ele esta aberto as teclas sao dele. O motivo de fazer agora: o
  passo antigo da sequencia D que abria o menu pelo mouse nunca abriu, porque
  o QEMU junta o `mouse_move -640 -400` com o movimento seguinte e o cursor
  ficava em x=0, fora do botao; a captura `reg-gui-4-menu` saia sem menu e
  nenhuma verificacao olhava. O passo foi trocado pelo teclado, e a captura
  agora confere o item destacado.
- Criterio da 6.7: a sequencia D termina com 3 apps abertos (`monitor` pelo
  Ctrl+P, `arquivos` pelo Ctrl+F e `config` pelo menu); a captura confere a
  janela do `config` na frente e os tres na barra de tarefas.

## v2.4.0-a15 (etapa 5.2 parte 1: MSI e a AHCI por interrupcao)

- A 5.2 vai em duas partes: esta (MSI, contadores por CPU, AHCI) e a rede.
  A `e1000` do QEMU (82540EM) nao tem MSI; a rede por MSI precisa da `e1000e`
  (82574L), que fica para a parte 2.
- PCI: `pci_find_cap` anda pela lista de capacidades e `pci_enable_msi`
  programa endereco (`0xFEE00000` com o id do LAPIC de destino), dado (o
  vetor, entrega fixa, borda) e liga a MSI com uma mensagem so; o INTx do
  dispositivo fica desligado no registrador de comando.
- Vetores 0x80 a 0x9F para MSI, alocados com nome (`interrupts_alloc_vector`);
  a CPU de destino sai de um rodizio que comeca na CPU 1 (`irq_next_cpu`),
  para tirar as IRQs dos dispositivos da CPU 0, que ja recebe as da ISA.
- Contadores por CPU e por vetor no `isr_dispatch`; `/proc/interrupts` (no
  formato do Linux, uma coluna por CPU e o nome no fim) e o `irq` do shell do
  kernel mostram os dois.
- AHCI: antes, o fim de cada comando era esperado com `thread_yield` em laco
  (a IRQ nem era ligada). Agora, com MSI, a porta liga as interrupcoes de fim
  de FIS e de erro, e quem espera dorme numa fila; o tratador limpa o `PxIS`
  e acorda a fila segurando a mesma trava que quem espera usa para conferir o
  `PxCI`, entao o aviso nao se perde entre a conferencia e o sono. O sono tem
  prazo de 10 ms como rede de seguranca. Sem MSI (ou antes do escalonador), a
  espera continua como era.
- Regressao A: o `cat /proc/interrupts` roda depois do `history`. O
  historico do `sh` guarda 50 comandos e, com o `cat /proc/stat` da 6.6, a
  sequencia A ja chegava a 50 antes do `history`; um comando a mais antes dele
  empurra para fora o `ls /bin | wc`, que a verificacao procura na linha 1.
  Comando novo na A antes do `history` precisa levar isso em conta.

## v2.4.0-a16 (etapa 5.2 parte 2: rede por MSI)

- O driver da `e1000` tenta MSI antes da GSI do `_PRT` e da IRQ legada:
  aloca um vetor, programa a MSI na proxima CPU do rodizio e, se a placa nao
  tiver a capacidade, devolve o vetor e segue como antes. Assim o mesmo
  driver serve a `e1000` (82540EM, sem MSI no QEMU) e a `e1000e` (82574L,
  `8086:10d3`), que aceita os descritores legados e o MAC em RAL/RAH do
  jeito que o driver ja usa.
- O QEMU dos testes passou a usar a `e1000e` por padrao (`test.ps1 -Nic`,
  `qrun.py --nic`, e o `regress.py` le o `-Nic` de cada sequencia). A
  sequencia D roda com `-Nic e1000` para a placa antiga continuar coberta: o
  DHCP do autostart passa por ela, pela GSI do `_PRT`.
- Com a AHCI na CPU 1 e a rede na CPU 2, as IRQs de dispositivo saem da CPU 0;
  a sequencia A confere no `/proc/interrupts`, depois do trafego de rede, a
  linha da `e1000` so na CPU 2.

## v2.4.0-a17 (etapa 5.3 parte 1: transporte virtio e virtio-blk)

- A 5.3 vai em partes: transporte e virtio-blk (esta), virtio-net, e a
  sequencia V da regressao rodando disco e rede pelo virtio.
- Transporte PCI moderno (virtio 1.0) so, sem o legado por portas de I/O: as
  capacidades de fornecedor (0x09) dao comum, notificacao, ISR e configuracao
  do dispositivo, cada uma numa regiao de BAR (o `pci_bar_address` entende BAR
  de 64 bits). O driver aceita so `VIRTIO_F_VERSION_1`, e a placa transicional
  (`1af4:1001`, a que o QEMU poe no q35) e a so-moderna (`1af4:1042`) passam
  pelo mesmo caminho.
- Fila: descritores, anel do driver e anel do dispositivo numa area contigua
  de paginas, ate 128 entradas; o virtio-blk manda um pedido por vez (cabecalho,
  dados, status, tres descritores fixos) sob um mutex, como a AHCI, com um
  buffer de 64 KiB.
- MSI-X (`pci_enable_msix`): a tabela fica numa BAR; programa a entrada com
  o mesmo endereco e dado da MSI, desmascara e liga a MSI-X com a mascara da
  funcao ligada durante a escrita. A fila usa a entrada 0; a configuracao
  fica sem vetor. Sem MSI-X, a espera vira laco com `thread_yield`.
- A espera do pedido e a da AHCI: dorme na fila de espera acordada pela
  interrupcao, com a mesma trava, e prazo de 10 ms como rede de seguranca.
- Disco `vdX` registrado como bloco comum: as particoes GPT e a busca da raiz
  ja eram por tipo, entao o boot pelo virtio (OVMF le o virtio-blk) monta a
  raiz em `vda2` sem mudanca no VFS.
- Regressao: sequencia V nova, com a imagem no virtio-blk (`-Bus virtio` no
  `test.ps1`, `--bus` no `qrun.py`): `fstest -a` na raiz, a MSI-X contada no
  `/proc/interrupts` e um arquivo gravado que sobrevive ao reboot.

## v2.4.0-a18 (etapa 5.3 parte 2: virtio-net)

- Camada `nic_*` (`drivers/nic.c`) entre a pilha de rede e o driver: o
  driver que acha a placa registra um `struct nic_ops` (MAC, link, receber,
  enviar, contadores, fila de espera da recepcao) e a pilha (`net.c`,
  `socket.c`) so chama `nic_*`. A primeira placa registrada vale; sem placa, a
  fila de espera e uma de reserva, para a thread da rede dormir do mesmo jeito.
  A `e1000` passou a procurar so placa Intel entre as de rede, em vez de
  reclamar da primeira que nao for.
- virtio-net: fila 0 de recepcao com 64 buffers de 2 KiB postados depois do
  `DRIVER_OK` (o virtio proibe notificar antes), cada um repostado ao ser lido;
  fila 1 de transmissao, sem interrupcao, com os buffers liberados no proprio
  envio seguinte. Todo quadro leva o cabecalho de 12 bytes do virtio 1.0
  (zerado: sem checksum nem segmentacao na placa). MAC e estado do link vem da
  configuracao do dispositivo (`VIRTIO_NET_F_MAC` e `F_STATUS`). A MSI-X da
  fila de recepcao acorda a thread da rede, como a IRQ da `e1000`.
- Sequencia V: disco e rede pelo virtio (`-Bus virtio -Nic virtio-net-pci`),
  com o servidor do hospedeiro: DHCP, ping, 10 MiB por TCP conferidos por
  crc32 e as duas MSI-X contadas no `/proc/interrupts`. Com isso a regressao
  roda nos dois modos (AHCI e `e1000e` de A a F, virtio na V), que e o
  criterio da 5.3.
- A sessao caiu no meio da escrita desta parte (Windows desligou sem aviso,
  sem QEMU rodando): os fontes gravados no ultimo minuto ficaram so com bytes
  nulos e foram refeitos a partir do commit da parte 1, que estava integro.

## v2.4.0-a19 (etapa 5.4: NVMe)

- Driver NVMe (`drivers/nvme.c`) para o primeiro controlador da classe
  01/08: desliga o controlador, monta a fila admin (16 entradas) e liga com
  comandos de 64 bytes e respostas de 16; `Identify` do controlador (modelo)
  e do namespace 1 (tamanho e formato do bloco). So aceita bloco de 512 bytes,
  que e o que a camada de bloco usa; outro tamanho e recusado com aviso.
- Uma fila de I/O (ate 32 entradas, ou menos se o `CAP.MQES` mandar) e um
  pedido por vez sob mutex, com um buffer de 64 KiB: a primeira pagina vai no
  PRP1, a segunda no PRP2 e, acima de duas paginas, o PRP2 aponta uma lista de
  PRP montada uma vez so. Escrita termina com `Flush`. As respostas sao
  reconhecidas pelo bit de fase, e o doorbell da fila de respostas anda junto.
- MSI-X na entrada 0 (a fila de respostas de I/O usa o vetor 0 com interrupcao
  ligada); quem espera dorme como na AHCI e no virtio. Os comandos admin, que
  so rodam na partida, esperam por laco.
- Nome `nvme0n1`, e a camada de bloco passou a por um `p` entre o nome do disco
  e o numero da particao quando o nome termina em digito (`nvme0n1p2`), como o
  Linux; `sda2` e `vda2` nao mudam.
- Regressao: sequencia N, com a imagem num NVMe do QEMU (`-Bus nvme` no
  `test.ps1` e no `qrun.py`): o OVMF boota pelo NVMe, a raiz monta em
  `nvme0n1p2`, `fstest -a` na raiz, a MSI-X contada e um arquivo que sobrevive
  ao reboot.

## v2.4.0-a20 (etapa 5.5: xHCI e enumeracao USB, em Rust)

- O USB e o segundo lugar do Rust que o plano listou depois do TCP:
  `kernel/rust/usb` (`orion_usb`). O C fica com o que e do kernel: achar o
  controlador no PCI (classe 0C/03/30), mapear a BAR, e funcoes finas que o
  Rust chama (`usb_dma_alloc`, `usb_now_us`, `usb_sleep_ms`, `usb_log`), porque
  `phys_to_virt` e `inline` e `log_info` e macro. O Rust faz o controlador e
  interpreta os descritores.
- Descritores (`desc.rs`) sao dados que vem do dispositivo, entao o parser nao
  confia em nada: confere tamanho e tipo, anda pela configuracao pelo `bLength`
  ate o menor entre o `wTotalLength` e o que chegou, para no comprimento zero, e
  troca caractere fora do ASCII por `?` nas strings. Os testes no hospedeiro
  usam os descritores do teclado do QEMU e cortam e trocam cada byte.
- `xhci.rs` so compila no alvo do kernel (`target_os = "none"`): o `cargo test`
  do crate guarda-chuva compila o `orion_usb` como dependencia comum no
  Windows, onde as funcoes do kernel nao existem.
- Controlador: tira a posse do firmware (capacidade estendida 1), para,
  reinicia, `MaxSlotsEn` ate 32, `DCBAA` com os buffers de rascunho que ele
  pedir, anel de comandos e anel de eventos de 256 TRBs com o bit de ciclo e
  o TRB de ligacao, e liga. Nesta etapa os eventos sao lidos por laco com
  prazo; interrupcao fica para quando houver transferencia continua (5.6).
- Enumeracao: em cada porta com algo ligado e ainda nao habilitada, reset da
  porta (preservando os bits RW1C e limpando as mudancas depois); `Enable
  Slot`, contexto de entrada com o slot e o EP0 (tamanho de pacote pela
  velocidade), `Address Device`, e transferencias de controle (Setup, Data,
  Status) para o descritor do dispositivo, a configuracao inteira, o idioma e
  as strings de fabricante e produto.
- O registrador de capacidades e lido em 32 bits: o QEMU devolveu 0 na leitura
  de 16 bits da versao (a primeira rodada mostrou "xHCI 0.00").
- `/proc/usb` e o `lsusb` (novo) mostram porta, slot, `ID vvvv:pppp`,
  fabricante, produto, versao USB, velocidade e a classe (do dispositivo ou da
  primeira interface). `test.ps1 -Usb` e `qrun.py --usb` ligam um `qemu-xhci`
  com `usb-kbd` e `usb-tablet`; a sequencia U nova confere o controlador, a
  enumeracao e o `lsusb`.

## v2.4.0-a21 (etapa 5.6: teclado e mouse USB HID)

- So o protocolo de boot do HID (subclasse 1, protocolo 1 teclado e 2 mouse):
  relatorio fixo de 8 bytes no teclado e de 3 ou 4 no mouse, sem interpretar o
  descritor de relatorio. Tablet e outros HID ficam de fora (o tablet do QEMU
  nao e de boot; por isso o teste usa `usb-mouse`).
- Para cada interface de boot: `Set Configuration`, `Configure Endpoint` do
  endpoint de interrupcao (contexto do slot copiado do contexto de saida, com
  o numero de entradas atualizado; intervalo convertido para as unidades do
  xHCI conforme a velocidade), `Set Protocol` (boot) e `Set Idle`, e 8 TRBs
  Normal de 64 bytes na fila. Cada evento de transferencia acha o buffer pelo
  proprio TRB apontado e devolve o TRB a fila.
- Os eventos sao lidos por uma thread do kernel (`usbd`) a cada 4 ms, so se
  houver HID; leitura e relatorio do `/proc/usb` usam uma trava, porque as duas
  mexem no estado do controlador. Interrupcao do xHCI ainda fica de fora.
- O teclado USB entra pelo mesmo caminho do PS/2: o Rust compara o relatorio
  com o anterior (`hid.rs`, testado no hospedeiro) e gera teclas do scancode
  set 1 (com `0xE0` nas estendidas); modificadores que descem vem antes das
  teclas e os que sobem vem depois, para Ctrl+C funcionar. O `keyboard.c`
  passou a ter `keyboard_feed` com trava, chamado pela IRQ do PS/2 e pelo USB.
  O mouse entra por `mouse_inject`, que usa a mesma fila e os mesmos eventos
  do PS/2. Sem repeticao automatica de tecla.
- Sem controlador PS/2 (a porta 0x64 le 0xFF), o `keyboard_init` avisa e nao
  liga a IRQ 1. `test.ps1 -NoPs2` e `qrun.py --no-ps2` sobem o q35 com
  `i8042=off`, e a sequencia U roda assim: o `evtest` recebe tecla, Ctrl, seta,
  movimento e botao vindos do USB.

## v2.4.0-a22 (etapa 5.7: pendrive USB)

- Armazenamento USB so pelo Bulk-Only (interface 08/06/50, SCSI
  transparente), que e o que pendrives usam. `msc.rs` (testado no hospedeiro)
  monta o CBW de 31 bytes e confere o CSW de 13 (assinatura, tag e status) e
  os comandos SCSI (INQUIRY, TEST UNIT READY, READ CAPACITY(10), READ(10),
  WRITE(10)); READ(10) limita o disco a 2 TiB, o que basta para pendrive.
- `Configure Endpoint` ficou geral (varios endpoints num comando so: os dois
  bulk aqui, o de interrupcao no HID) e o numero de contextos do slot vira o
  maior DCI configurado.
- Um pedido SCSI e tres transferencias bulk (CBW, dados, CSW) com um buffer
  de 64 KiB; leitura e escrita maiores vao em pedaços de 128 blocos. Na parte
  1 tudo rodava com a trava do controlador e a espera tratava no meio os
  eventos de teclado e mouse, e erro ou STALL so falhavam o pedido; a parte 2
  (abaixo) trocou isso.
- O disco so e registrado depois que o controlador fica publicado, porque o
  registro ja le a tabela de particoes, e essa leitura passa pela trava do
  controlador. O nome e o primeiro `sdX` livre, como o Linux faz com pendrive;
  o C serializa leitura e escrita com um mutex por disco. So blocos de 512.
- Regressao U: um `usb-storage` em branco (`-UsbDisk`, imagem de 16 MiB
  recriada a cada rodada) e formatado pelo `mkfs`, montado, gravado,
  desmontado e montado de novo, e um programa copiado para ele roda de la. O
  pendrive do QEMU e USB 3, entao a enumeracao numa porta USB 3 tambem ficou
  coberta.

## v2.4.0-a23 (etapa 5.7 parte 2: despachante unico, prazos e recuperacao)

O que mudou na 5.7 e por que:

- Despachante unico do anel de eventos. Antes, quem esperava (comando,
  transferencia de controle, bulk do disco) lia o anel de eventos por conta
  propria e tratava ali dentro o que fosse de teclado e mouse; com o disco
  esperando 5 s por uma transferencia, o teclado ficava preso a essa espera.
  Agora so o despachante le o anel: ele roda na interrupcao (MSI-X, MSI se nao
  houver, e por consulta a cada 4 ms na thread `usbd` se nenhuma das duas
  existir) e tambem quando quem espera chama `pump()`, o que cobre o boot,
  antes das interrupcoes. Cada conclusao vai para o dono pelo endereco do TRB
  (o evento traz o endereco do TRB que terminou), nao so por slot e endpoint:
  quem envia registra o endereco do TRB com IOC antes de escrever o bit de
  ciclo e tocar a campainha, e o despachante marca aquele registro como
  pronto e acorda quem espera. Os relatorios HID nao tem registro: o
  despachante os reconhece pelo anel do endpoint, entrega ao teclado ou mouse
  e recoloca o TRB.
- Travas. O estado do despachante fica atras de um spin lock com
  interrupcoes desligadas (`EVLOCK`, com `usb_irq_save`/`usb_irq_restore`),
  nunca um mutex, porque ele roda na interrupcao. Ordem: trava do controlador
  (`BUSY`) antes de `EVLOCK`, e `EVLOCK` antes das travas de teclado, mouse e
  `/dev/input` (o despachante chama `keyboard_feed` e `mouse_inject`, que ja
  eram seguras para interrupcao porque o PS/2 as chama dali). Nada dentro de
  `EVLOCK` pega `BUSY`, nem dorme, nem acorda alguem com a trava na mao: o
  `usb_wake` (fila de espera com spin lock proprio, chamavel da interrupcao)
  vem depois de soltar `EVLOCK`. `BUSY` nao e mais spin puro: quem a disputa
  cede a CPU (`usb_yield`).
- O disco dorme sem a trava do controlador. Comando do xHCI e transferencia de
  controle esperam com `BUSY` na mao (sao curtos e mexem no estado do
  controlador); a transferencia bulk e enfileirada com `BUSY` e esperada sem
  ela, na fila de espera do USB. O mutex por disco do C continua
  serializando os pedidos de um mesmo pendrive.
- Prazos. Comando 2 s, controle 2 s, bulk 5 s. No prazo estourado sem DMA
  pendente esquecido: o registro vira "cancelado", o endpoint leva `Stop
  Endpoint` e `Set TR Dequeue Pointer` para depois do TRB cancelado, o pedido
  devolve erro (EIO no bloco) e o disco passa a responder EIO dali em diante
  (marcado morto), sem prender teclado e mouse, que nao dependem dele. Uma
  conclusao que chegue depois para um registro cancelado e ignorada e contada
  (`/proc/usb`: "conclusao(oes) tardia(s) ignorada(s)"); registro cancelado
  sem conclusao e liberado depois de 10 s. O mesmo vale para o endpoint 0: no
  prazo estourado ele e parado e a fila avancada; num STALL leva `Reset
  Endpoint` e `Set TR Dequeue`, porque senao o endpoint 0 ficava parado para
  sempre depois de um descritor recusado.
- REQUEST SENSE e recuperacao do Bulk-Only. CSW com status 1 leva a um REQUEST
  SENSE e a chave, o ASC e o ASCQ vao para o log (formato fixo e descritor);
  o pedido falha sem repetir, porque o erro e do comando. CSW invalido,
  status 2 (erro de fase), STALL ou outro erro levam a recuperacao de reset
  da especificacao: Bulk-Only Mass Storage Reset na interface,
  `CLEAR_FEATURE(ENDPOINT_HALT)` nos dois endpoints bulk e, do lado do xHCI,
  `Reset Endpoint` + `Set TR Dequeue` no endpoint que ficou parado (estado 2 no
  contexto de saida) ou, se ele nao parou, `Stop Endpoint` e um `Configure
  Endpoint` que tira e poe o endpoint de volta (zera o toggle do lado do
  hospedeiro, como o `CLEAR_FEATURE` zera do lado do pendrive). Depois disso o
  comando e repetido uma vez.
- Bloco diferente de 512 e recusado com mensagem clara ("pendrive com blocos
  de N bytes recusado; so 512 e suportado"). Aceitar 4096 mexe no cache de
  paginas, no FAT e no ext2 de uma vez; fica para quando houver um pendrive
  assim para testar.
- Teste. O comando `usbstall sdX` do shell do kernel manda um CBW com
  assinatura errada (o `usb-storage` do QEMU responde com STALL), faz a
  recuperacao, confere TEST UNIT READY e READ(10) e, por fim, um READ(10) fora
  do disco, que volta com status 1 e passa pelo REQUEST SENSE (chave 5, ASC
  0x21). A sequencia U ganhou um segundo pendrive, criado no hospedeiro pelo
  `tools/mkfat.py` (FAT16 sem particoes, rotulo HOSPEDE, com `LEIAME.TXT`): o
  Orion o le e grava `NOVO.TXT`, e depois de o QEMU sair o hospedeiro le o
  arquivo de volta com o mesmo `mkfat.py`. O xHCI do QEMU passou a ter 8
  portas USB 2 e 8 USB 3 (`p2=8,p3=8`): com 4 portas, o quarto dispositivo
  faz o QEMU pendurar um hub automatico, e hub ainda nao tem driver.

## v2.4.0-a24 (etapa 5.7 parte 3: remocao e segundo plug)

- Disco que some. A camada de blocos ganhou contagem de referencias
  (`blockdev_get`/`blockdev_put`): cada montagem e cada arquivo aberto de
  `/dev/sdX` seguram o disco (ou a particao). `blockdev_remove` marca o disco
  e as particoes como removidos, descarta o cache de setores dele (contando os
  setores sujos perdidos, porque nao ha mais onde grava-los) e, dali em diante,
  toda leitura e escrita falha antes de chegar ao driver. O disco removido some
  do `/dev`, do `/proc/diskstats` e da busca da raiz, mas a entrada continua
  existindo, com o nome reservado, ate o ultimo usuario soltar; ai ela e dada
  como liberada, o nome fica livre e o driver e avisado (`release`) para soltar
  a memoria dele. Como o `umount` ja ignorava erro de sincronizacao, desmontar
  o pendrive removido funciona: as escritas pendentes falham e a montagem sai.
- As entradas da tabela de discos nao sao reaproveitadas (o indice vira o
  inode no `/dev`, e uma entrada nova com o mesmo inode poderia casar com um
  vnode velho no cache); o limite subiu de 16 para 32 entradas, o que da para
  varios plugs por boot. Do lado do USB cabem 16 registros de pendrive por
  boot.
- O FAT devolve EIO, e nao "sem espaco" nem "inexistente", quando a falha
  vem de disco removido (`blockdev_is_gone`).
- Remocao no xHCI. O evento Port Status Change acende um bit no despachante;
  a thread `usbd` (que agora sempre roda) le os bits, reconhece a mudanca de
  conexao, confirma os bits de mudanca da porta e, com a trava do controlador:
  marca o dispositivo como removido, o disco como morto, conclui com "sumiu"
  as esperas daquele slot (quem esperava acorda na hora, sem esperar o prazo),
  tira os pipes HID do despachante, manda `Disable Slot`, zera a entrada do
  DCBAA e devolve a memoria do endpoint 0, dos contextos e dos pipes HID. A
  memoria do pendrive (aneis bulk e buffers) so volta quando a camada de
  blocos libera o disco, porque ate la pode haver uma leitura em andamento
  usando o buffer. O aviso a camada de blocos (`usb_disk_gone`) vem depois de
  soltar a trava do controlador: quem segura o cache de setores pode estar
  esperando essa trava, e a remocao precisa do cache.
- Conexao nova (o mesmo caminho do boot: reset da porta, Enable Slot, Address
  Device, descritores, HID ou Bulk-Only) roda na thread `usbd`. No boot, os
  bits de mudanca de todas as portas sao confirmados depois da enumeracao;
  senao uma porta vazia com o bit de conexao ja aceso nunca mais gera evento.
- Teste do prazo: o comando `usbtimeout sdX` do shell do kernel enfileira a
  proxima transferencia sem tocar a campainha, o que simula um pendrive que
  nao responde. No QEMU o `device_del` e percebido pela porta antes do prazo
  (as esperas do slot terminam como "sumiu"), entao o caminho do prazo
  estourado so e exercitado assim.
- Interrupcao sem alocacao: os eventos de tecla agora vao para um vetor
  reservado no pipe HID, porque o despachante roda na interrupcao e nao deve
  chamar o alocador do kernel ali.
- Teste: na sequencia U o pendrive do hospedeiro e montado, removido com
  `device_del` (leitura e escrita dao EIO, o `lsusb` perde o dispositivo, o
  `umount` funciona e libera o disco, o shell segue respondendo), ligado de
  novo com `device_add` (volta como `sdc` e monta com os dois arquivos), e o
  `usbtimeout` estoura o prazo de 5 s com o teclado funcionando depois. O
  segundo pendrive usa `-blockdev` em vez de `-drive`, porque o QEMU apaga o
  `-drive` junto com o dispositivo no `device_del` e o `device_add` nao teria
  o que ligar.
- Sem suporte a hub USB (registrado em 2026-10-19). Dispositivo atras de um
  hub nao e visto: o hub aparece no `lsusb` com a classe "hub", mas as portas
  dele nao sao ligadas nem enumeradas. Isso inclui hub externo, teclado ou
  monitor com hub embutido e o hub interno de muitos notebooks (webcam,
  Bluetooth e leitor de cartao costumam ficar atras dele; as portas externas
  em geral vao direto no xHCI). Decisao: suporte basico a hub USB 2 entra como
  parte extra da Fase 5 (5.10, depois da 5.9 e antes da 7.3), porque o teste
  em hardware real (7.4) depende de teclado, e teclado atras de hub e comum;
  hub USB 3 (o lado SuperSpeed) fica para depois. Ate la, o roteiro do teste
  em hardware real manda ligar teclado e pendrive direto numa porta do PC.

## Harness: regressao mais rapida (2026-10-19, junto da v2.4.0-a25)

- O `cmd:` esperava sempre o `-CommandDelay` (1,5 s) e o `wait:N` sempre N
  segundos, mesmo com o comando ja terminado. Agora o `cmd:` segue assim que
  o prompt volta (o `-CommandDelay` vira teto), e o `wait:N` logo depois de um
  comando termina quando o prompt volta e a saida fica 0,4 s quieta (N vira
  teto). Comando com `&` (segundo plano) mantem a espera fixa, porque ali o
  prompt volta antes do trabalho acabar.
- O que conta como prompt: so o que aparece depois da quebra de linha que
  fecha o eco do comando (nunca o prompt antigo nem a linha "orion> comando")
  e so se a ultima linha recebida for inteira um prompt (`<dir> $ `,
  `<dir> # ` ou `orion> `), sem nada depois. Um `$ ` no meio da saida nao
  dispara.
- Caso conhecido em que o prompt engana: programa que abre outro shell (o
  `ptytest` mostra o prompt do shell de dentro do pty). Ali a espera vira
  `until:` com o texto do fim do programa.
- `-SlowWaits` (test.ps1, regressao.ps1) e `--slow-waits` (qrun.py,
  regress.py), ou a variavel `ORION_SLOW_WAITS`, voltam as esperas fixas
  antigas. Verificacao que comecar a falhar de vez em quando: o primeiro passo
  e rodar a sequencia com `-SlowWaits`; se passar, a culpa e da espera.
- Cada `test.ps1` imprime quanto tempo foi para o boot e para cada tipo de
  passo, e a regressao imprime o tempo total. (Detalhe do PowerShell: numa
  tabela com uma chave chamada `keys`, `$tabela.Keys` devolve o valor dessa
  chave; o resumo usa `GetEnumerator()`.)
- Juntar boots ficou de fora: cada sequencia sobe uma maquina diferente
  (rede com hostfwd, grafico, discos extras, ISO, virtio, NVMe, USB sem PS/2),
  e junta-las mudaria os nomes dos discos e exigiria reescrever verificacoes.

## v2.4.0-a25 (etapa 5.7 parte 4: contexto de interrupcao e STALL com dados)

- Contador de interrupcao por CPU (`irq_depth` no `percpu`). Toda entrada
  passa pelo `isr_dispatch`: IRQ legada, timer do LAPIC, IPIs, MSI e MSI-X e
  NMI sobem o contador em volta do handler; as outras excecoes nao sobem, mas
  herdam o valor, entao um page fault dentro de um handler conta como
  interrupcao, e o de um programa ou de uma syscall nao. O contador desce
  antes do `sched_check_preempt`, e a troca de thread so acontece com o
  contador em 0 (interrupcao mais de fora). O despachante do USB chamado pela
  thread `usbd` roda com o contador em 0 e nao conta.
- Aviso: `MIGHT_SLEEP` em `kmalloc`, `kzalloc`, `kcalloc`, `krealloc`,
  `mutex_lock`, `wait_sleep*`, `thread_yield`, `thread_sleep_ms` e
  `thread_block`. So o contador decide: interrupcoes desligadas com trava de
  spin presa nao avisam. `mutex_trylock`, `kfree` e o resto que nao dorme nao
  avisam. A mensagem "BUG: <funcao> dentro de interrupcao, chamado de
  <endereco> (vetor, CPU)" sai uma vez por ponto de chamada: uma tabela fixa
  de 64 enderecos preenchida com `cmpxchg`, sem trava e sem alocar memoria.
  O `kmalloc` passou a ter um miolo interno (`alloc_raw`) para o endereco
  apontar quem chamou o `kzalloc` ou o `krealloc`, e nao a funcao do meio.
- O log ja usava trava de spin com interrupcoes desligadas e tolerava
  reentrada na mesma CPU, entao pode ser chamado de IRQ. No NMI nada vai
  direto ao log: o aviso fica num campo por CPU (sem trava) e sai no fim do
  proximo `isr_dispatch` daquela CPU com o contador em 0.
- Na regressao, panico: o `test.ps1` e o `qrun.py` passam
  `-fw_cfg name=opt/orion/strict`; o kernel so le o fw_cfg quando o CPUID diz
  que ha hypervisor e a assinatura "QEMU" confere. Num PC de verdade fica so o
  aviso no log.
- `irqtest` (shell do kernel) prova tudo isso com um vetor proprio mandado
  para a mesma CPU: kmalloc duas vezes do mesmo ponto (um aviso), trylock (nenhum),
  kmalloc com trava de spin fora de IRQ (nenhum), timer aninhado (o handler
  de teste faz o EOI e reabre as interrupcoes, porque o LAPIC segura o timer,
  vetor 0x40, enquanto um vetor de prioridade maior esta em servico; o tick
  aninhado chega com profundidade 2 e a thread no fim e a mesma) e um NMI que
  so anota. No Orion os handlers rodam com interrupcoes desligadas, entao
  aninhamento so acontece se um handler reabrir as interrupcoes.
- `usbstall`: le o setor 0 antes do STALL, recupera, zera o buffer de DMA e
  le de novo; os 512 bytes tem de ser iguais. Roda no pendrive feito no
  hospedeiro (setor 0 com conteudo conhecido e assinatura 55aa), o que prova
  que a sequencia do endpoint foi zerada dos dois lados.
- Ganchos de teste que quebram o estado do hardware de proposito (`usbstall`,
  que manda um CBW invalido e provoca STALL; `usbtimeout`, que cala a
  campainha e deixa o pendrive morto; `irqtest`, que reabre interrupcoes
  dentro de um handler e manda NMI para a propria CPU) so funcionam no modo
  estrito, ou seja, no QEMU de teste com `-fw_cfg name=opt/orion/strict`.
  Fora dele respondem EPERM com uma mensagem, e a funcao de baixo (no C e no
  Rust) recusa tambem, nao so o comando do shell. Decidido pelo dono em
  2026-10-19: num PC de verdade um comando de teste digitado por engano nao
  pode derrubar o pendrive nem mexer no controlador de interrupcoes. O
  `tcploss` fica de fora da regra: descarta segmentos no software e volta ao
  normal com `tcploss 0`, sem mexer em hardware.
- Primeiro achado do aviso, na regressao: o botao de energia (SCI do ACPI,
  IRQ 9) fazia `kmalloc` dentro da interrupcao, porque o uACPI mapeia a faixa
  de I/O dos registradores de evento a cada leitura (`uacpi_kernel_io_map`) e
  o mapa alocava um handle. O handle agora carrega base e tamanho no proprio
  valor (bit 62 de marca, base nos 16 bits de baixo, tamanho nos 17
  seguintes), sem alocar, e o `io_unmap` nao faz nada. De quebra, uma faixa
  de 64 KiB a partir da porta 0 deixou de estourar o tamanho de 16 bits.
- A sequencia F (os quatro boots da ISO, que e o que vai para o PC de
  verdade) roda sem o fw_cfg (`-NoStrict`): confere que o modo estrito fica
  desligado, que `irqtest`, `usbstall` e `usbtimeout` respondem EPERM e que
  nenhum aviso de contexto de interrupcao aparece nos boots.
- Segundo achado, no estresse (fork em massa): avisos falsos em contexto de
  thread, com "vetor 0". O `in_interrupt()` lia o contador em duas etapas
  (endereco da CPU pelo GS, depois o campo) com a thread preemptivel; se ela
  migrasse entre as duas, lia o contador de outra CPU, que podia estar no
  meio de uma interrupcao. Agora a leitura e o relatorio pegam CPU, vetor e
  profundidade com interrupcoes desligadas, e a mensagem traz a
  profundidade. Ficaram duas guardas baratas: troca de thread
  (`schedule_locked`) com o contador acima de zero tambem avisa, e o contador
  abaixo de zero e panico.
- Auditoria do mesmo padrao (ponteiro da CPU atual e depois um campo, com a
  thread preemptivel), pedida pelo dono em 2026-10-19. O per-CPU fica no GS
  (`percpu.self` e os campos lidos com `mov %gs:offset`). Seguros, e por que:
  - `thread_current()` e `percpu_cpu_id()`: uma instrucao so relativa ao GS;
    o valor e o da CPU que executa naquele instante, sem janela para migrar.
  - `percpu_get()` so aparece com interrupcoes desligadas: `isr_dispatch`
    (porta de interrupcao), `sched_tick` e `smp_watchdog_tick` (timer),
    `sched_make_ready`, `thread_unblock`, `thread_block`,
    `sched_block_switch`, `schedule_locked` e `sched_post_switch` (todos com
    `runq_lock`, pego com interrupcoes desligadas), `sched_check_preempt` (so
    chamado do fim da interrupcao) e `sched_init`/`sched_init_ap` (boot da
    CPU, antes de haver troca de thread).
  - `gdt_set_kernel_stack` (chamado na troca de thread) e `gdt_in_ist` (no
    tratamento de excecao): interrupcoes desligadas.
  - `spin_set_owner`/`spin_held`: o dono e gravado com a trava ja pega; as
    travas de spin sao pegas com interrupcoes desligadas, menos a `tlb_lock`
    do `smp_tlb_shootdown_sync`, que fica com interrupcoes ligadas para a
    propria CPU responder a IPI. Ali a thread pode migrar com a trava na mao e
    o `owner_cpu` fica velho, mas ele so serve para o relatorio de deadlock e
    ninguem faz `ASSERT_HELD` nela.
  - `smp_report_cpu` compara a CPU alvo com a atual no caminho de deadlock,
    que termina em panico; uma migracao ali so muda qual CPU recebe o NMI de
    relatorio.
  - `busy_cpu[]` do comando `preempt` do shell: so mostra em que CPU a thread
    de teste estava, e a amostra pode ser de um instante antes da migracao.
  Com risco, so o proprio `in_interrupt()`, que lia o ponteiro e depois o
  campo; agora ele le `irq_depth` com um `mov %gs:offset` so, sem desligar
  interrupcoes (o relatorio, que e raro, continua pegando CPU, vetor e
  profundidade com interrupcoes desligadas).
- Preempcao em modo kernel e travas de spin (comeco da 8.2, 2026-10-19). O
  `sched_check_preempt` roda no fim de toda interrupcao, venha ela do usuario
  ou do kernel, entao uma thread do kernel pode ser preemptada segurando uma
  trava de spin pega com interrupcoes ligadas (a `tlb_lock`, por exemplo).
  Agora cada thread conta as travas de spin que segura (`spin_count`): o
  `spin_set_owner` (usado por `spin_lock`, `spin_try_lock` e
  `spin_lock_irqsave`) sobe o contador do dono, e o `spin_unlock` desce o
  contador do dono (`owner_thread`), e nao o da thread que estiver rodando.
  Isso deixa certa a passagem da `runq_lock` na troca de thread: quem pega e
  a thread que sai e quem solta e a que entra (`sched_post_switch`, inclusive
  pelo `thread_trampoline` de uma thread nova); o -1 cai na que saiu, com a
  `runq_lock` ainda presa, entao ela nao pode estar rodando em outra CPU.
  Trava pega num handler de IRQ sobe e desce o contador da thread
  interrompida antes do fim do handler. O `sched_check_preempt` nao troca de
  thread com o contador acima de zero: conta um adiamento por CPU
  (`preempt_deferred`) e deixa o pedido marcado para a proxima interrupcao.
  Nenhum lugar solta trava sem o `spin_unlock` (conferido), senao o contador
  ficaria preso e a thread nunca mais seria preemptada. A 8.2 (kernel
  preemptivo com contagem de travas seguras) parte daqui.
- `in_interrupt()` virou um `movl %gs:offsetof(irq_depth)` so.
- `tmptest`: o teste de 1000 arquivos falhou pela terceira vez com a fila da
  `kreclaim` ainda cheia (ate 268 paginas logo depois do ultimo `unlink`). Em
  vez de subir a tolerancia, ele passou a medir de novo a cada 10 ms enquanto
  as paginas livres ainda sobem, ate 1 s.

## v2.4.0-a26 (etapa 5.8 parte 1: driver HDA)

- Controlador: reset pelo `GCTL.CRST` (baixa, espera, sobe e espera 2 ms
  para os codecs se anunciarem no `STATESTS`), CORB e RIRB de 256 entradas
  na mesma pagina. Os verbos do codec vao pelo CORB e a resposta e lida por
  consulta no RIRB (prazo de 200 ms por verbo), sem interrupcao: so a
  configuracao usa verbo, e e rara. Depois de cada resposta o `RIRBSTS` e
  limpo e o `RINTCNT` fica em 255: o QEMU (e controladores reais) param de
  andar no CORB quando o contador de respostas chega ao limite sem o status
  ser limpo. Resposta nao solicitada (bit 4 do `response_ex`) e pulada.
- Codec sem IDs fixos: o driver acha o AFG pelo tipo de grupo de funcao, le
  as capacidades, a configuracao padrao e a lista de conexoes de cada
  widget, e escolhe os pinos de saida pela configuracao padrao (capaz de
  saida, conectividade diferente de "nenhuma", dispositivo line out, depois
  alto-falante, depois fone). De cada pino ele desce pelas conexoes (ate 6
  nos, sem passar duas vezes pelo mesmo) ate um conversor de saida que nao
  seja digital; o primeiro DAC achado e usado por todos os pinos que chegam
  nele (ate 4), entao num notebook alto-falante e fone tocam juntos. Em cada
  no do caminho: energia D0 (se o no controla energia), selecao de conexao
  (seletor e pino com mais de uma entrada), amplificador de entrada do mixer
  na entrada usada e de saida de todo no com amplificador, sem mudo e no
  ganho de 0 dB (o "offset" da capacidade do amplificador, nao o maximo,
  para nao estourar caixa de som). No pino: saida ligada, HP ligado se for
  fone, e EAPD se o pino tiver (amplificador externo de muitos notebooks,
  que sem isso ficam mudos).
- Pensando no hardware real (7.4), ficou de fora: deteccao de fone
  (unsolicited response e troca entre alto-falante e fone), codecs com
  quirks (pinos com configuracao padrao errada, GPIO para ligar
  amplificador), HDMI/DisplayPort (conversores digitais sao pulados) e mais
  de um codec (usa o primeiro que respondeu). Se a 7.4 voltar muda, o
  primeiro suspeito e o caminho escolhido (o log mostra pino e caminho) e o
  EAPD.
- Stream: o primeiro stream de saida (indice = numero de streams de
  entrada, do `GCAP`), tag 1, BDL de 8 entradas de 4 KiB (anel de 32 KiB,
  ~170 ms em 48 kHz estereo de 16 bits) com IOC em cada entrada, a BDL numa
  pagina propria (alinhada bem alem dos 128 bytes pedidos), o mesmo formato
  no `SDnFMT` e no conversor (`SET_CONV_FORMAT`) e a tag no conversor
  (`SET_CONV_STREAM_CHAN`). Interrupcao por MSI; sem MSI, uma thread consulta
  a cada 5 ms.
- Posicao: o driver le o `LPIB` na interrupcao e em cada escrita e mantem
  contadores que so crescem (consumido, escrito e "ja zerado"). Tudo que o
  hardware vai tocar e ainda nao foi escrito e zerado na hora: com a fila
  vazia toca silencio, nunca o ultimo trecho repetido. Escrita atrasada
  (consumido passou o escrito) conta como falta de dado e pula para a
  posicao do hardware.
- Parar com dreno espera o hardware passar do ultimo byte escrito mais
  60 ms de silencio: parando no ultimo byte, o FIFO do controlador (e o
  buffer do QEMU) perdia o fim do som (o tom de 2 s saia com 1,96 s).
- Teste: `hdatone [ms] [Hz]` no shell do kernel gera um seno (aproximacao de
  Bhaskara em inteiros, o kernel nao usa ponto flutuante) so no canal
  esquerdo. A sequencia S liga `ich9-intel-hda` com o codec `hda-output` e
  `-audiodev wav`; o `test.ps1` fecha o QEMU pelo monitor (`quit`) antes da
  leitura, para o cabecalho do WAV sair certo, e o `tools/wavcheck.py` confere
  no hospedeiro: nao mudo, duracao na margem, ~1 kHz pelos cruzamentos de
  zero, canal direito em silencio e nenhum buraco de mais de 10 ms, sem
  comparar amostra por amostra. O audio so e ligado nessa sequencia.
