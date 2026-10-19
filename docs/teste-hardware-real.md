# Teste em hardware real

O roteiro completo (gravar o pendrive no Windows, opcoes do firmware, o que
conferir em cada etapa do boot e o que anotar se algo falhar) entra na etapa
7.4. Esta pagina ja guarda as limitacoes que o teste precisa levar em conta.

## Limitacoes conhecidas

- **Sem hub USB.** Dispositivo atras de um hub nao e visto pelo Orion: hub
  externo, teclado ou monitor com hub embutido e o hub interno de muitos
  notebooks (onde costumam ficar webcam, Bluetooth e leitor de cartao). Ligue
  o teclado, o mouse e o pendrive direto numa porta do PC; se o teclado nao
  responder, tente outra porta (de preferencia uma da placa-mae, atras do
  gabinete) e anote qual funcionou. O suporte basico a hub USB 2 esta
  planejado para a etapa 5.10.
