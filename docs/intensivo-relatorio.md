# Intensivo Orion - relatorio

## Sessao 1 - 2026-10-16 19:22 a 2026-10-17 08:50

Fases 0 e 1 do `docs/PLANO.md` completas: 19 etapas, uma por commit, todas com
`.\check.ps1` limpo, regressao verde e, nas que mexem em trava, memoria,
processos, threads ou sinais, o estresse 5x verde. Marco **M1** fechado
(versao 2.0.0, tag `v2.0`): o Orion roda `dash`, `lua`, `tcc` e Doom
compilados para Linux sem mudar uma linha deles.

### Etapas

| Etapa | Versao | Commit | Regressao depois |
|---|---|---|---|
| 0.1 Estabilidade SMP, ps/kill/uptime/free/time, /dev/null, zero e random | 1.9 | `58bb747` | 92 |
| 0.2 Copia segura de memoria de usuario | 1.9.1 | `8b3d171` | 97 |
| 0.3 Mutex que dorme, AHCI | 1.9.2 | `4d55e72` | 99 |
| 0.4 FAT e cache de blocos com mutex | 1.9.3 | `076285d` | 102 |
| 0.5 Harness em Python e CI | 1.9.4 | `bc9a8e0` | 105 |
| 0.6 Shell com `;`, `&&`, `\|\|`, `&`, jobs, fg, wait | 1.9.5 | `d924ee2` | 118 |
| 0.7 256 descritores, limites, ulimit | 1.9.6 | `0af3cc4` | 123 |
| 1.1 Pilha inicial do Linux (auxv), SSE por thread | 2.0.0-a1 | `88ce390` | 129 |
| 1.2 Syscalls de partida da musl, 64 sinais | 2.0.0-a2 | `e809b3a` | 134 |
| 1.3 Arquivos no formato do Linux | 2.0.0-a3 | `dd00ea2` | 139 |
| 1.4 Areas de memoria, paginas sob demanda, mmap de arquivo | 2.0.0-a4 | `0edfa7e` | 149 |
| 1.5 Programas contra a musl com `zig cc` | 2.0.0-a5 | `a37486a` | 154 |
| 1.6 Threads (`clone`) | 2.0.0-a6 | `5871386` | 164 |
| 1.7 futex | 2.0.0-a7 | `f40fab9` | 179 |
| 1.8 Tempo e sinais completos | 2.0.0-a8 | `a75d6ab` | 196 |
| 1.9 dash, lua, sbase | 2.0.0-a9 | `7208886` | 207 |
| 1.10 Doom | 2.0.0-a10 | `fe72b5e` | 213 |
| 1.11 tcc dentro do Orion | 2.0.0-a11 | `4f1b270` | 218 |
| 1.12 Programas proprios na musl, libc propria aposentada | 2.0.0 | (commit do M1, tag `v2.0`) | 218 |

### Antes e depois

| | Inicio da sessao | Fim da sessao |
|---|---|---|
| Verificacoes da regressao | 81 (4 sequencias) | 218 (5 sequencias, mais o estresse 5x) |
| Programas em `/bin` | 40, todos da libc propria | 91, todos contra a musl: 53 proprios, 4 testes da musl, 5 portados (dash, lua, sbase-box, doom, tcc) e 29 utilitarios do sbase |
| libc | propria, 1313 linhas | musl (via Zig 0.16.0); a propria saiu |
| Software de terceiros rodando | nenhum | dash, lua, sbase (97 utilitarios), tcc, Doom |
| Chamadas de sistema documentadas | sem tabela | `docs/SYSCALLS.md`, 101 entradas, numeros do Linux |
| Kernel | | +5489 / -1091 linhas em 62 arquivos (sem contar a 1.12) |

### Os tres problemas que mais custaram

1. **Corridas de memoria e de sinais com threads.** As revisoes adversariais
   (cada achado conferido por um ou dois revisores independentes) acharam,
   entre outros: paginas voltando ao alocador antes do shootdown de TLB (outra
   CPU podia escrever numa pagina ja reaproveitada); uma thread nova liberada
   antes do criador terminar de solta-la; o endereco do handler de sinal lido
   duas vezes sem trava, o que deixava outra thread fazer o `sysret` voltar para
   endereco nao canonico (#GP em ring 0 com a pilha do usuario). Tudo corrigido
   antes dos commits; esta registrado em `docs/DECISOES.md`.
2. **O timer do QEMU TCG chega bem abaixo dos 1000 Hz nominais** (uns 70 por
   segundo por CPU). Contar ticks dava 19 ms de CPU para um laco de 300 ms. O
   tempo de CPU passou a ser medido pelo TSC na troca de contexto. O mesmo
   efeito faz uma CPU ociosa demorar uns 15 ms para pegar um processo novo
   (nao ha IPI de "acorde"; pendente para o escalonador).
3. **A `libc.a` do Zig 0.16 nao e completa.** `memcpy`, `strlen` e a
   matematica foram para a `libzigc` e a `compiler_rt` do Zig; o tcc dentro do
   Orion precisa de uma libc so. O gerador do tcc junta as tres e cria os
   `crti.o`/`crtn.o` que o Zig nao gera.

### Bloqueios

Nenhuma etapa ficou `[!]`. Pendencias registradas (nas entradas de
`docs/DECISOES.md` e no diario):

- NMI que chegue na janela entre a entrada da syscall e o `swapgs` (so com NMI
  externo; etapa de robustez).
- Panico do kernel quando falta memoria para tabela de paginas, inclusive num
  `mremap` grande (etapa 8.4).
- O escalonador nao acorda CPU ociosa quando entra trabalho na fila.
- O `rev` do sbase no commit usado esta quebrado no proprio upstream (fica fora
  de `/bin`).
- `ITIMER_VIRTUAL`/`ITIMER_PROF`, `mremap` com `MREMAP_FIXED`, som no Doom e a
  tecla Pause (sequencia E1 do teclado).

### Para o dono fazer

- **O CI nunca rodou**: o workflow em `.github/workflows` existe desde a 0.5,
  mas nada foi enviado ao GitHub (push e proibido na sessao). Vale conferir no
  primeiro push; ele usa `make` (que agora chama `tools/musl_build.py`) e
  `tools/regress.py`.
- **Python 3 virou requisito do build** (os geradores do dash, do sbase e do
  tcc). O `setup.ps1` instala; o `build.ps1` acha pelo `tools/python.ps1`.
- **Primeiro build precisa de internet**: baixa o Zig (cerca de 100 MB) e o
  Freedoom (24 MB), os dois com SHA-256 conferido e fora do git.
- **Etapa 7.4 (hardware real)** continua reservada para voce.
- Opcional: avisar o sbase do `rev` quebrado no commit `3de61ef`.
