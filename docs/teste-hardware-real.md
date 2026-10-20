# Teste em hardware real

O roteiro completo (gravar o pendrive no Windows, opcoes do firmware, o que
conferir em cada etapa do boot e o que anotar se algo falhar) entra na etapa
7.4. Esta pagina ja guarda as limitacoes que o teste precisa levar em conta.

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
