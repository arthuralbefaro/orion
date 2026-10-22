# Hardware real: resultado dos testes (etapa 7.4)

Registro do que o Orion fez em cada computador de verdade em que foi
testado. O roteiro do teste esta em `docs/teste-hardware-real.md`.

## Notebook ASUS Vivobook, AMD Ryzen (2026-10-21, ISO do commit 83f0f55)

Maquina unica do dono, com Windows instalado no NVMe. Boot pela ISO live
gravada num pendrive, em UEFI. Por isso, so o modo live e testado nela:
nada de `instalar`.

### Funcionou

| Area | Resultado |
|---|---|
| CPUs | 8 CPUs online de 8 no MADT (xAPIC nos APs); fila de prontos por CPU; SMEP e SMAP ligados |
| PCI | 29 dispositivos encontrados |
| ACPI | uACPI pronto em 52 ms; botao de energia ligado; `_PRT` com 4 rotas. O AML do firmware tem um erro (`\_SB_.PCI0.GPP2.WLAN._STA: object already exists`, um `MethodOp` pulado); o uACPI segue mesmo assim |
| NVMe | `1cc1:41c8` em 01:00.0, `SM2P41C8-512GC5`, versao 1.4, fila de 32, MSI-X no vetor 128 (CPU 1); 4 particoes GPT lidas: ESP de 200 MiB, 16 MiB reservada, 487271 MiB (Windows), 897 MiB (recuperacao) |
| USB | xHCI `1022:1503` em 03:00.3, versao 1.20, 3 portas, MSI-X no vetor 129 (CPU 2); hub high speed `05e3:0610` com 4 portas e TT por porta na porta 2; Bluetooth `13d3:3556` (full speed) na porta 2.4 |
| Audio HDA | controlador `1002:1640` em 03:00.1 encontrado (e o audio HDMI da GPU; ver limitacoes) |
| Raiz | tmpfs do initramfs (438 entradas, 58067 KiB); `/tmp`, `/run`, `/proc` e `/dev` montados |
| Timer e IRQs | interrupcoes habilitadas e timer ok |
| init | `init iniciado` |

### Falhou

- **`#DF` (double fault) na CPU 1 logo depois de `init iniciado`**, na
  thread `init`, logo depois de um `mmap(0, 0x5000)` do `init`. Mecanismo
  (reproduzido no QEMU com o mesmo padrao de registros): o kernel entrou no
  anel 0 com o GS do usuario carregado; o `percpu_get()` leu `%gs:` com
  base 0 e fez `#PF`; a entrada antiga decidia o `swapgs` pelo anel de onde
  a excecao veio (anel 0: nao troca) e entrou de novo no mesmo
  `percpu_get()`. Cada volta gasta 272 bytes, ate a pilha de 16 KiB acabar
  (`RIP = isr_common+3`, `RAX = 1` do `percpu_ready()`, RCX, RDX, RSI e R8
  a R11 ainda com os valores do usuario). O que deixou o GS do usuario no
  anel 0 foi uma volta ao anel 3 (`iretq`) que falhou depois do `swapgs`,
  confirmado na segunda rodada (abaixo). A ISO a12 decide o `swapgs` pelo
  `MSR_GS_BASE` em toda entrada, entao o laco nao acontece mais, e para com
  uma mensagem que diz qual dos casos foi e com os numeros. Tambem confere
  o topo da pilha em toda entrada vinda do usuario, segue a cadeia pelo
  `RBP` no relatorio e acrescenta o limite de aninhamento, o anel de
  vetores por CPU e o detector de tempestade de IRQ (so para linhas do I/O
  APIC). A pilha continua com 16 KiB: o laco esgotaria qualquer tamanho.
  Detalhes: `docs/DECISOES.md`, v3.0.0-a11 parte 2 e v3.0.0-a12.
- **Montagem automatica da ESP do Windows com escrita** (`/mnt montado:
  FAT32 em nvme0n1p1`). Nao chegou a gravar nada, mas era um risco; o modo
  live agora so monta pendrive, e somente leitura.

### Limitacoes conhecidas nesta maquina

- **Audio:** o HDA encontrado (`1002:1640`) e o audio HDMI da GPU AMD, e o
  codec dele nao tem caminho de um pino de saida ate um DAC (so pinos
  digitais). O audio analogico do notebook (alto-falantes e fone) passa pelo
  AMD ACP (Audio Coprocessor), que fica fora do escopo. Sem som.
- **Rede:** nenhuma placa de rede suportada: nao ha Intel e1000 nem virtio,
  e nao ha porta de rede cabeada no notebook. Caminho mais curto: adaptador
  USB de rede cabeada (CDC-ECM ou Realtek RTL8153), etapa 9.3 do plano.
- **Wi-Fi: modelo ainda nao identificado.** O Bluetooth `13d3:3556` sugere
  um modulo MediaTek (talvez MT7921), mas o ID PCI da placa ainda nao foi
  anotado: no proximo boot, `lspci` (ISO 3.0.0-a15 ou posterior) mostra a
  linha da classe `0280` (rede sem fio) com o `vendor:device`. Sem driver.
- **Video:** sem driver para a GPU AMD; a tela fica no modo que o firmware
  entregou (framebuffer do Limine).
- **Touchpad:** e I2C (nao PS/2); o `mouse` PS/2 nao encontra o dispositivo
  auxiliar. Sem driver.
- **COM1:** nao existe UART em 0x3F8 (porta le 0xFF); a serial e detectada
  como ausente e a IRQ4 fica desligada.

### Segunda rodada (2026-10-22, ISO 3.0.0-a13, Ventoy em modo normal, UEFI)

- O modo live nao montou o disco: `modo live: disco interno nvme0n1 nao e
  montado sozinho (use mount -r)`.
- Logo depois de `init iniciado`, o relatorio novo mostrou a causa em vez
  do `#DF`: `a volta ao anel 3 falhou no iretq ... #GP (erro 18) ...
  quadro devolvido: RIP=1007d02 CS=23 RFLAGS=10202 RSP=7ffde379fb50 SS=18`.
  Nenhuma IRQ mascarada; os ultimos vetores eram o timer.
- Causa: o kernel gravava o STAR sem o RPL 3 no seletor de volta do
  `sysret`. A Intel (e o QEMU) forca o RPL 3 do SS no `sysret`; a AMD nao,
  e o anel 3 rodava com SS=0x18. A primeira interrupcao vinda do usuario
  empilhava esse SS e o `iretq` de volta era recusado. No primeiro teste,
  esse mesmo `iretq` com o GS do usuario carregado virou o laco de `#PF` e
  o `#DF`. Corrigido na v3.0.0-a14 (`docs/DECISOES.md`).

### Terceira rodada (2026-10-22, ISO 3.0.0-a14, Ventoy em modo normal, UEFI): SUCESSO

- Boot completo ate o login; o modo live nao montou o `nvme0n1`.
- `segtest 5`: TUDO OK nas 8 CPUs com 16 threads; 99855 leituras de CS/SS
  depois de syscall e de troca de CPU, 2494 sinais, 4 filhos de `fork`,
  10704 voltas ao anel 3 por `iretq` conferidas pelo kernel, nenhuma com
  seletor errado.
- O `doom` abriu o framebuffer (1920x1080, escala 2), mas sem argumentos
  nao achou o IWAD: o WAD estava em `/doom`, e o doomgeneric so procurava
  no diretorio atual. O `find / -name '*.wad'` nao achou nada porque o `sh`
  do Orion passa as aspas para o `find` (sem aspas ele acha). Corrigido na
  3.0.0-a15: os dois Freedoom ficam em `/usr/share/games/doom` e o `doom`
  acha o WAD sozinho.
- Faltavam comandos para ver o PCI e o log do kernel no shell normal; a
  3.0.0-a15 traz `lspci` (`/proc/pci`, com o driver de cada dispositivo) e
  `dmesg` (`/proc/kmsg`).

Com isso a etapa 7.4 fecha (marco M5: o Orion boota num computador fisico
pelo pendrive e chega ao shell). O teste do Doom com a ISO 3.0.0-a15 fica
como conferencia, nao como condicao da etapa.

### Limitacoes que ficam (resumo)

- Sem rede: nenhuma placa suportada; Wi-Fi ainda nao identificada.
- Sem som: o audio analogico passa pelo AMD ACP, nao suportado; o HDA
  presente e so o HDMI da GPU.
- Sem driver de video da GPU AMD, de touchpad I2C e de COM1 (nao existe).
