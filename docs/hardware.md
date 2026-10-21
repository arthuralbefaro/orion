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
  thread `init`, antes de o `init` imprimir a primeira linha. Estouro da
  pilha do kernel de 16 KiB: uma interrupcao chegou quando o codigo
  interrompido so tinha 16 bytes de pilha, o `isr_common` (`RIP =
  isr_common+3`) empurrou tres registros e o quarto caiu na pagina de
  guarda. Os registros mostram um `mmap(0, 0x5000, leitura e escrita,
  privado e anonimo)` do `init` recem-feito e pouco codigo do kernel
  executado desde a entrada; o ponteiro para o quadro da entrada, se for o
  que esta em RDI e RBX, poe o comeco da entrada 288 bytes acima do fundo da
  pilha, e nao no topo. Duas explicacoes fecham com os numeros: o topo da
  pilha usado na entrada vinda do usuario estava errado, ou uma cadeia de
  chamadas (talvez recursiva) consumiu os 16 KiB. O relatorio da versao
  testada nao seguia a cadeia de chamadas, entao nao da para separar as
  duas. A ISO nova confere o topo da pilha em toda entrada vinda do usuario
  (panico com os numeros se estiver errado), segue a cadeia pelo `RBP` no
  relatorio, dobra a pilha para 32 KiB e acrescenta o limite de
  aninhamento, o anel de vetores por CPU e o detector de tempestade de IRQ.
  Detalhes: `docs/DECISOES.md`, v3.0.0-a11 parte 2.
- **Montagem automatica da ESP do Windows com escrita** (`/mnt montado:
  FAT32 em nvme0n1p1`). Nao chegou a gravar nada, mas era um risco; o modo
  live agora so monta pendrive, e somente leitura.

### Limitacoes conhecidas nesta maquina

- **Audio:** o HDA encontrado (`1002:1640`) e o audio HDMI da GPU AMD, e o
  codec dele nao tem caminho de um pino de saida ate um DAC (so pinos
  digitais). O audio analogico do notebook (alto-falantes e fone) passa pelo
  AMD ACP (Audio Coprocessor), que fica fora do escopo. Sem som.
- **Rede:** nao ha placa Intel e1000 nem virtio; o Wi-Fi do notebook (o
  Bluetooth `13d3:3556` e o par de um modulo MediaTek, provavelmente MT7921;
  confirmar com `pci` no shell do kernel e anotar o ID aqui) nao tem
  driver. Caminho mais curto: adaptador USB de rede cabeada (CDC-ECM ou
  Realtek RTL8153), anotado no plano.
- **Video:** sem driver para a GPU AMD; a tela fica no modo que o firmware
  entregou (framebuffer do Limine).
- **Touchpad:** e I2C (nao PS/2); o `mouse` PS/2 nao encontra o dispositivo
  auxiliar. Sem driver.
- **COM1:** nao existe UART em 0x3F8 (porta le 0xFF); a serial e detectada
  como ausente e a IRQ4 fica desligada.

### Pendente

A 7.4 continua `[!]` ate a ISO nova (v3.0.0-a11 ou posterior) passar no
modo live nesta maquina: chegar ao login, rodar os comandos do roteiro e
desligar pelo `poweroff`.
