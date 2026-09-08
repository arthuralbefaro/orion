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
