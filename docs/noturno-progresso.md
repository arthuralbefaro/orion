# Turno noturno - progresso

Checklist de uma linha por etapa. `[x]` feita, `[~]` parcial, `[ ]` nao comecada.
Este arquivo permite retomar o trabalho sem ler o historico da conversa.

Base do turno: commit `fe29851` (v1.0), `.\check.ps1` limpo, regressao verde.

| # | Etapa | Estado | Inicio | Fim | Commit |
| --- | --- | --- | --- | --- | --- |
| 4.1 | Filas de espera e poll (v1.1) | [x] | 22:52 | 02:35 | `5601c92` |
| 4.2 | Sinais (v1.2) | [x] | 02:35 | 14:55 | `80361d7` |
| 4.3 | Escrita no FAT e redirecionamentos (v1.3) | [ ] | 14:55 | - | - |
| 4.4 | Teclas estendidas e edicao de linha (v1.4) | [ ] | - | - | - |
| 4.5 | Fork copy-on-write (v1.5) | [ ] | - | - | - |
| 4.6 | Rede (v1.6) | [ ] | - | - | - |
| 4.7 | GUI: boot direto e apps (v1.7) | [ ] | - | - | - |
| 4.8 | Extras | [ ] | - | - | - |

## Como retomar

    cd c:\dev\orion
    .\check.ps1          # precisa terminar com "Nenhum erro ou warning."
    .\regressao.ps1      # sequencias A (texto) e B (grafica); exit 0 = verde
    git log --oneline -5

A proxima etapa e a primeira linha com `[ ]` na tabela acima.

## Diario

### 4.1 Filas de espera e poll - 22:52 as 02:35 - commit 5601c92 - FEITO
`kernel/wait.{h,c}`: waitqueue com `struct wait_entry` na pilha do dormidor e
lista por thread (`t->waits`), o que permite dormir em varias filas (base do
poll). Ordem de travas do sistema: trava de dados -> `runq_lock`.
Migrados: disciplina de linha, PTY (master e slave), pipes, `waitpid` e
`/dev/input`. Nao sobrou nenhum `thread_sleep_ms(2..5)` de espera ocupada.
`SYS_POLL` (7) sobre `file_ops.poll`; `SYS_FCNTL` (72) com `O_NONBLOCK`; o `wm`
usa `poll` e nao usa mais o timeout via ioctl.
Dois problemas achados: (1) colher threads mortas dentro da `runq_lock` invertia
a ordem contra `proc_lock` do `waitpid` - a colheita foi movida para depois do
unlock em `sched_post_switch`; (2) uma fila de espera por processo exigiria
garantir a vida do `struct process` do pai na hora de colher o filho, entao
`waitpid` usa uma fila global unica de saidas.
Medido com o `wm` aberto e parado, 4 CPUs: 31 trocas de contexto em 3,2 s
(~10/s no total). Antes, cada leitor bloqueado acordava a cada 2 ms e o `wm` a
cada 20 ms.

### 4.2 Sinais - 02:35 as 14:55 - commit 80361d7 - FEITO
`kernel/signal.{h,c}`: pendentes, mascara, handlers por processo, quadro de
sinal empilhado na pilha do usuario (128 bytes abaixo do rsp, pulando a red
zone) e `rt_sigreturn`. Entrega no retorno de syscall, de IRQ e de excecao.
Syscalls: kill(62), rt_sigaction(13), rt_sigprocmask(14), rt_sigreturn(15),
alarm(37), setpgid(109), getpgid(121). O trampolim de retorno vem da libc
(`sa_restorer`), como no Linux.
Excecoes de ring 3 viram SIGSEGV/SIGILL/SIGFPE; `write` em pipe sem leitor gera
SIGPIPE; o pai recebe SIGCHLD. Chamadas bloqueantes devolvem `EINTR`.
Grupo de primeiro plano fica na disciplina de linha; `TIOCSPGRP` funciona no TTY
e no PTY; Ctrl+C manda SIGINT ao grupo. O `sh` usa grupos, refaz o `waitpid` em
EINTR e imprime `[morto por sinal N]`.
Status de saida passou ao formato do Linux (`WIFSIGNALED`/`WTERMSIG`).
Tres problemas achados: (1) o contador de interrupcoes de timer anda ~10x mais
devagar que o tempo real neste QEMU com as CPUs ociosas, esticando todo sleep e
alarm - `time_ticks()` agora sai do TSC; (2) `process_thread_reaped` zerava
`pml4_phys` fora do `proc_lock`, deixando um `waitpid` concorrente colher e
liberar o mesmo `struct process` (kfree duplo, so aparecia com o `wm` rodando,
que faz `waitpid(WNOHANG)` a cada volta); (3) uma edicao minha de `ldisc.c` nao
casou o padrao e falhou em silencio, deixando `interrupt_pgid` sempre 0 - por
isso o primeiro Ctrl+C nao matava nada.
Fora do escopo: zumbi orfao cujo pai morreu antes de colher fica registrado ate
o boot seguinte (aparece em `procs`); nao vaza paginas, so o `struct process`.

### 4.3 Escrita no FAT e redirecionamentos - inicio 14:55
