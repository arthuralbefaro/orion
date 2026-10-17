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
