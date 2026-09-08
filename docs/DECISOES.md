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
