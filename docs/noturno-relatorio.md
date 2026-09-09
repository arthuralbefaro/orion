# Turno noturno - relatorio

Ponto de partida: commit `fe29851` (Orion v1.0, interface grafica), regressao de
27 verificacoes em duas sequencias.

Ponto de chegada: Orion v1.8, regressao de 80 verificacoes em quatro sequencias,
verde. Oito etapas planejadas, oito entregues.

## O que foi entregue

| Etapa | Versao | Commit | O que ficou pronto |
| --- | --- | --- | --- |
| 4.1 | v1.1 | `5601c92` | filas de espera de verdade, `poll`, `fcntl`/`O_NONBLOCK` |
| 4.2 | v1.2 | `80361d7` | sinais POSIX, grupos de processo, `alarm`, tempo pelo TSC |
| 4.3 | v1.3 | `a0da561` | escrita no FAT, cache de blocos, redirecionamentos, 13 utilitarios |
| 4.4 | v1.4 | `ff02fda` | teclas estendidas, modo raw, edicao de linha com historico, `edit` |
| 4.5 | v1.5 | `8e8b469` | `fork` copy-on-write com contagem de referencias por pagina |
| 4.6 | v1.6 | `32b9937` | driver e1000, ARP/IPv4/ICMP/UDP, sockets, DHCP, DNS |
| 4.7 | v1.7 | `9fcd874` | boot direto na GUI, janelas de processos e arquivos, menu |
| 4.8 | v1.8 | (ver `git log`) | variaveis de ambiente e `PATH`, `top`, `env`, `printenv`, `clear` |

Numeros: 27 -> 80 verificacoes automaticas; 20 -> 40 programas em `/bin`;
duas -> quatro sequencias de teste (texto e rede, grafica, disco cru, boot
grafico).

## Os problemas mais interessantes

**1. A interrupcao da placa de rede travava o boot por causa do driver de disco.**
Ligar a IRQ 11 da e1000 passou a travar o boot de forma intermitente. A IRQ 11 e
compartilhada com o controlador AHCI, que e um driver de polling e nunca limpava
o registrador de status de interrupcao `PxIS`. A linha, que e sensivel a nivel,
ficava presa em alto e a CPU entrava em tempestade de interrupcoes. Enquanto a
IRQ estava mascarada ninguem notou. A correcao foi no AHCI: desligar as proprias
interrupcoes e limpar o status depois de cada comando.

**2. Uma funcao de dormir que so era segura por sorte.**
`wait_sleep_timeout` pegava a trava do escalonador com um `spin_lock` puro. Todos
os chamadores existentes ja vinham com interrupcao desabilitada, porque todos
seguravam uma trava tomada com `spin_lock_irqsave` - a pre-condicao existia mas
nao estava escrita em lugar nenhum. A pilha de rede foi a primeira a chamar sem
trava, com interrupcao ligada, e um tick do timer dentro dessa janela travava a
CPU contra ela mesma. Hoje a funcao salva e restaura o estado das interrupcoes,
como o `poll_table_sleep` ao lado dela ja fazia.

**3. Copy-on-write tambem falta em ring 0.**
Com `CR0.WP` ligado, uma syscall que escreve no buffer do usuario (`read`,
`getcwd`) leva falta de escrita numa pagina marcada como COW igual ao programa -
so que com CPL 0. A primeira versao do handler exigia o bit de usuario no codigo
de erro e o kernel entrava em panico. Reproduzi de proposito antes de corrigir, e
o `forktest` tem uma verificacao dedicada: o filho chama `getcwd` num buffer que
nunca tocou em modo usuario.

Menção honrosa: o console do kernel nao entendia sequencia ANSI nenhuma, entao o
`edit` despejava os escapes como texto e cada tecla rolava a tela varias vezes -
e rolar 1280x800 e um memmove de 4 MiB. O editor ficava segundos atras do
teclado, e foi isso que apareceu na regressao como "69 paginas vazando".

## O que ficou de fora

- **TCP e `wget`.** O passo pedia como opcional; a maquina de estados com
  retransmissao e janela nao cabia no tempo da etapa. UDP, ICMP e DNS cobrem o
  que os programas pedidos precisam.
- **ISO para BIOS.** Nem `xorriso` nem `mkisofs` existem nesta maquina e o
  `winget` nao tem nenhum dos dois. O Limine ja traz `limine-bios-cd.bin`;
  falta so a ferramenta que monta o ISO9660 com El Torito.
- **Escrita no `out\esp` do QEMU.** O `vvfat` aborta o proprio QEMU com escrita
  de verdade. Todo teste pesado de disco usa `-Disk`, um disco cru formatado
  pelo `mkfs` de dentro do Orion.

## O problema numero um em aberto

Duas travadas raras, nao reproduziveis, as duas em momentos de criacao ou saida
de processo com 4 CPUs:

- na sequencia C, um `#PF Page Fault ... ring 0, busca de instrucao` em RIP=0
  durante o `mkfs`, na CPU 2, com o quadro de excecao inconsistente (RSP=0x400,
  CS=0x270);
- na sequencia A, o sistema parou de responder na serial logo depois de o
  `forktest -s` terminar, sem panico nenhum.

Nenhuma das duas voltou a acontecer em varias execucoes seguidas das mesmas
sequencias. Como nao dava para reproduzir, o que entrou foi diagnostico: o dump
de excecao nao tratada agora imprime tid, nome e CPU da thread, os limites da
pilha dela e se o RSP esta dentro - na proxima vez da para dizer de imediato se e
estouro de pilha de kernel. **E por onde eu comecaria amanha.**

## Uma nota sobre o metodo

A regressao pagou o proprio custo varias vezes. Os tres bugs de kernel mais
serios do turno (a tempestade de IRQ, a trava do escalonador e o COW em ring 0)
apareceram como marcador vermelho antes de aparecerem como sintoma, e o
"vazamento de 69 paginas" que virou o parser de ANSI no console so foi notado
porque a verificacao de memoria e automatica. O custo de manter os marcadores
atualizados a cada etapa e pequeno perto disso.
