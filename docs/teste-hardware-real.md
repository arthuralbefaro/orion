# Teste em hardware real

Roteiro para bootar o Orion num PC ou notebook de verdade pelo pendrive e
contar o que funcionou. Nao precisa instalar nada no disco do computador: a
ISO boota com o sistema inteiro em memoria e nada e gravado no disco, a
menos que voce rode o `instalar`.

> **MAQUINA COM OUTRO SISTEMA (Windows, Linux) NO DISCO: TESTE SO O MODO
> LIVE.** Nao rode o `instalar`. O modo live nao monta sozinho nenhuma
> particao de disco interno (NVMe, SATA): so pendrive, e somente leitura.
> O instalador lista os discos com modelo, tamanho e particoes, marca com
> `*** TEM OUTRO SISTEMA ***` o disco que tem Windows ou Linux e so apaga
> esse disco se voce digitar `apagar <nome>`; mesmo assim, nao use. A
> instalacao em pendrive ou SSD externo e o dual boot sem apagar nada estao
> no plano (`docs/PLANO.md`, "Depois da 7.4") e ainda nao existem.

## 1. Gerar a ISO

No Windows, na pasta do projeto:

    .\build.ps1 -Iso

O arquivo sai em `out\orion.iso` (uns 65 MiB). A mesma ISO serve para CD e
para pendrive, em BIOS e em UEFI.

## 2. Gravar o pendrive no Windows

Use um pendrive de 1 GB ou mais. **Tudo que estiver nele sera apagado.**

- **Rufus** (recomendado): escolha o pendrive em "Dispositivo", a
  `orion.iso` em "Selecao de boot" e clique em "Iniciar". Quando o Rufus
  perguntar o modo de gravacao, escolha **"Gravar em modo Imagem DD"** (nao o
  modo ISO): a ISO do Orion e hibrida e precisa ir byte a byte.
- **balenaEtcher**: "Flash from file", escolha a ISO, escolha o pendrive,
  "Flash". Ele ja grava byte a byte.

Depois de gravar, o Windows pode dizer que o pendrive "precisa ser
formatado": ignore e nao formate.

## 3. Opcoes do firmware (BIOS/UEFI do computador)

Entre no menu do firmware (normalmente `F2`, `Del` ou `Esc` logo ao ligar;
em notebooks as vezes `F1` ou `F10`).

- **Secure Boot: desligado.** O carregador do Orion (Limine) nao e assinado;
  com Secure Boot ligado o firmware recusa o pendrive.
- **Modo de boot:** o Orion boota em **UEFI** (o normal em PC de 2012 para
  ca) e em **BIOS/Legacy**. Teste primeiro em UEFI. Se houver opcao **CSM**
  (Compatibility Support Module), ela so importa para o teste em BIOS: ligue
  o CSM para testar BIOS, desligue para UEFI puro.
- **Fast Boot:** desligue, se existir; com ele alguns firmwares nem leem o
  USB no boot.
- Para escolher o pendrive sem mexer na ordem de boot, use o menu de boot
  rapido (`F12`, `F11`, `F8` ou `Esc`, conforme o fabricante). O pendrive
  pode aparecer duas vezes: "UEFI: nome do pendrive" (UEFI) e so o nome
  (BIOS).

## 4. O que conferir em cada etapa do boot

1. **Menu do Limine** (fundo escuro, entrada "Orion"): sobe sozinho em 1 s.
   Se nem isso aparecer, o problema e anterior ao Orion (gravacao do
   pendrive, Secure Boot ou modo de boot).
2. **Log do kernel** (linhas `[info]` coloridas). Vale anotar, se
   aparecerem:
   - `acpi ... uACPI pronto` (ACPI funcionando: desligar e reiniciar);
   - `ahci` ou `nvme` com o disco do computador (o Orion so le; nada e
     gravado sem o `instalar`);
   - `usb ... xHCI ... porta N: teclado HID pronto` (teclado USB);
   - `e1000` ou `rede` (placa de rede; so modelos Intel e1000/e1000e e
     virtio sao suportados);
   - `hda` (audio), `bga` ou `virtio-gpu` (troca de resolucao; em placa de
     video de verdade nenhum dos dois aparece e a tela fica no modo que o
     firmware escolheu);
   - `vfs ... modo live: disco interno X nao e montado sozinho` (esperado em
     maquina com disco interno: o live nao toca nele);
   - `irq ... tempestade de interrupcao: vetor N ... mascarado` (uma IRQ
     que nao parava de disparar foi desligada; o sistema segue, mas anote
     o vetor e o nome);
   - qualquer linha `[WARN]` ou `[ERR ]`.
3. **Login**: `orion login:` aparece. Usuario `root`, senha `orion`.
4. **Shell**: `/ $`. Teste:
   - `ls /bin` (sistema em memoria);
   - `lsusb` (dispositivos USB);
   - `free` e `top 1` (memoria e CPUs);
   - `ping 1.1.1.1 3` se houver cabo de rede com DHCP (`dhcp` primeiro, se o
     endereco nao veio sozinho);
   - `wm` para o ambiente grafico (sai com `Ctrl+Alt+X`);
   - `play --tom 440 2000` (deve tocar um tom de 2 s, so no lado esquerdo);
   - `doom` (acha sozinho o `freedoom1.wad` em `/usr/share/games/doom`; o
     `freedoom2.wad` esta na mesma pasta, `doom -iwad
     /usr/share/games/doom/freedoom2.wad`; sai com `Esc` e o menu).
5. **Shell do kernel** (`Ctrl+K` alterna): `pci`, `lsblk`, `irq`, `cpus`,
   `acpi`, `fb` ajudam a entender o hardware. O `irq` mostra, por CPU, os
   ultimos 16 vetores atendidos, a profundidade de interrupcao e a IRQ que
   mais disparou no ultimo segundo.
6. **Pilhas do kernel**: `cat /proc/stacks` no shell normal mostra o maior
   uso de pilha por thread; anote a linha `maior uso`.
7. **Desligar**: `poweroff` (pelo ACPI). Anote se a maquina desligou de
   verdade.

## 5. Se algo falhar, anote e mande

- **Foto da tela** no ponto onde parou (de preferencia legivel, as ultimas
  20 linhas sao as que importam).
- **O ultimo texto que apareceu** antes de travar ou reiniciar.
- **Modelo do computador** (ou da placa-mae), processador e quantidade de
  memoria; versao do firmware, se aparecer na tela dele.
- **Modo de boot usado** (UEFI ou BIOS/CSM) e se o Secure Boot estava
  desligado.
- **Se o teclado respondeu** (de que tipo: USB direto, atras de hub, PS/2,
  notebook) e em que porta estava.
- Se chegou ao shell: a saida de `lsusb`, `pci` (`Ctrl+K`, depois `pci`) e
  `lsblk`.
- **Se aparecer `EXCECAO NAO TRATADA` ou `KERNEL PANIC`:** o relatorio e
  longo e todas as linhas importam. Fotografe (ou transcreva) em especial:
  a linha `thread tid=...` com a faixa da pilha; a linha `>>> a pilha
  acabou ao entrar no vetor N ... RIP=...` se houver; o bloco `CPUn:
  profundidade de interrupcao ... ultimos vetores: ...` de cada CPU; e a
  lista inteira de `pilha de chamadas` (os enderecos `ffffffff8...`). Com
  esses enderecos e o `out\orion.elf` da mesma ISO, `llvm-addr2line -f -e
  out\orion.elf <endereco>` diz a funcao de cada um. Se a tela rolar e
  cortar o comeco, uma foto do que sobrou ainda vale.

## 5a. Segunda rodada no notebook ASUS Vivobook (depois do `#DF` de 2026-10-21)

Use a ISO gerada deste commit (`build.ps1 -Iso`; a data do `out\orion.iso`
e do `out\orion.elf` tem de ser a mesma compilacao) e **so o modo live**:

1. Boot em UEFI pelo pendrive, como da primeira vez. Confira no log:
   `modo live: disco interno nvme0n1 nao e montado sozinho` (e **nenhum**
   `/mnt montado ... nvme0n1p1`).
2. Se chegar ao `login:`, entre como `root`/`orion` e rode, nessa ordem:
   `cat /proc/stacks`; `Ctrl+K`, `irq`, `pci` (anote o ID da placa Wi-Fi,
   `14c3:xxxx` ou parecido, e o do controlador de rede, se houver), `lsblk`,
   `Ctrl+K`; `lsusb`; `free`; `top 1`; `wm` (sai com `Ctrl+Alt+X`);
   `poweroff`. O teclado do notebook e PS/2 e deve funcionar; o touchpad
   (I2C) nao.
3. Se cair de novo em `#DF` ou em panico, mande o relatorio inteiro como
   descrito na secao 5. Agora ele traz a cadeia de chamadas pelo `RBP`
   (`cadeia pelo RBP`, com as repeticoes resumidas), o vetor em que a pilha
   acabou e o estado de interrupcao de cada CPU, o que basta para achar a
   causa. Se o defeito do primeiro teste se repetir, o esperado agora e um
   `KERNEL PANIC` com uma destas linhas, em vez do `#DF`:
   - `a volta ao anel 3 falhou no iretq em ...: ... quadro devolvido: RIP=...
     CS=... RFLAGS=... RSP=... SS=...`;
   - `entrada do anel 3 (...) com o GS do kernel ja carregado ...`;
   - `o kernel rodava com o GS do usuario: vetor N (...) em RIP=...`;
   - `entrada do anel 3 (...) com a pilha errada na CPU N: ...`.
   Anote a linha inteira, com todos os numeros: ela diz qual caminho voltou
   ao usuario errado.
4. Se o sistema travar sem mensagem, anote o ultimo texto e, se der, se o
   `Caps Lock` ainda acende (diz se o teclado ainda e atendido).

## 5b. Terceira rodada no notebook (ISO 3.0.0-a14, depois do `#GP` com SS=18)

A segunda rodada (ISO a13) achou a causa: o `sysret` da AMD deixava o anel
3 com SS=0x18, e o `iretq` da primeira interrupcao vinda do usuario dava
`#GP`. A a14 corrige o STAR. Mesmo procedimento da 5a, so modo live, com
a ISO 3.0.0-a14 (a versao aparece no log, `Orion 3.0.0-a14`):

1. Confira no log do boot a linha `syscall/sysret ativos ... sysret volta
   com CS=23 SS=1b na Intel e CS=23 SS=1b na AMD`.
2. Depois do login, rode primeiro `segtest 5`. Ele leva uns 5 s, usa as 8
   CPUs, e o resultado esperado e `segtest: TUDO OK` com `CS=23 SS=1b no
   comeco` e `0 com seletor errado`. Anote as quatro linhas `segtest:`.
3. Siga com os comandos do passo 2 da secao 5a e o `poweroff`.
4. Se aparecer um panico, anote a linha inteira como na 5a. Uma linha nova
   possivel: `volta ao anel 3 por iretq (...) com seletores errados: CS=...
   SS=...`.

## 6. Instalar no disco (opcional, so se quiser)

**Isto apaga o disco escolhido inteiro.** So faca num computador ou disco
de teste, nunca na maquina que tem o Windows. No shell do sistema live:
`instalar` sem argumento lista cada disco com modelo, tamanho e particoes e
diz quais nao tem outro sistema; depois `instalar /dev/sdX` (ou
`/dev/nvme0n1`) e digite o nome do disco (`sdX`) quando pedir. Disco marcado
com `*** TEM OUTRO SISTEMA ***` (NTFS, ESP com `\EFI\Microsoft`, particao de
recuperacao, raiz Linux) nunca e oferecido como destino e so e apagado se
voce digitar `apagar <nome>`. Tire o pendrive e reinicie: o disco boota
sozinho em UEFI. Instalacao para boot BIOS ainda nao existe.

## Limitacoes conhecidas

- **Hub USB: so o lado USB 2.** Desde a etapa 5.10 o Orion enxerga
  dispositivos atras de hub USB 2 (hub externo, teclado ou monitor com hub
  embutido, o hub interno de notebooks). Num hub USB 3, os dispositivos USB 2
  funcionam e os USB 3 (pendrive azul numa porta azul do hub) ainda nao sao
  vistos. O caminho de "Transaction Translator" (teclado e mouse full/low
  speed atras de hub high speed, o caso mais comum em PC de verdade) foi
  feito pela especificacao mas nunca testado: o hub do QEMU e full speed. Se
  o teclado nao responder atras de um hub, ligue-o direto numa porta do PC e
  anote: modelo do hub, se o teclado funciona direto e o que o `lsusb`
  mostra (pelo shell do kernel, `Ctrl+K`, se o teclado da porta direta
  funcionar).
- **Placa de rede:** so Intel e1000/e1000e (82540, 82574 e parecidas) e
  virtio. Realtek, Broadcom e Wi-Fi nao funcionam.
- **Video:** sem driver para placas de video de verdade; a tela fica no modo
  que o firmware entregou (o `fbset` so troca de modo em maquina virtual).
- **Audio:** HDA com um codec; deteccao de fone e codecs com ajustes
  especiais do fabricante nao estao feitos. Se ficar mudo, anote o que o
  `hda` (no shell do kernel) mostra.
- **Discos:** AHCI (SATA), NVMe e pendrive USB; RAID do firmware e
  controladores especiais nao.
