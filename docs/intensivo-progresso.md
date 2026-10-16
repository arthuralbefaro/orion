# Intensivo Orion - progresso

Diario do trabalho sobre o `docs/PLANO.md`. Uma linha por etapa:
`[x]` feita, `[~]` em andamento, `[!]` bloqueada, `[ ]` nao comecada.
Este arquivo manda: uma sessao nova retoma da primeira etapa que nao esta `[x]`.

## Como retomar

    cd C:\dev\orion
    .\check.ps1          # precisa terminar com "Nenhum erro ou warning."
    .\regressao.ps1      # A (texto e rede), B (grafica), C (disco), D (GUI)
    git log --oneline -10

Estresse (etapas que mexem em trava, escalonador, memoria, processos, threads
ou sinais), 5 vezes seguidas:

    .\test.ps1 -Smp 4 -Commands @("stress 3000 16 | stress 3000 16 | stress 3000 16 | stress 3000 16")

Proxima etapa: 0.1.

## Checklist

| # | Etapa | Estado | Inicio | Fim | Commit |
| --- | --- | --- | --- | --- | --- |
| 0.1 | Commit da v1.9 e dos programas novos | [~] | 19:22 | | |
| 0.2 | Copia segura de memoria de usuario | [ ] | | | |
| 0.3 | Mutex que dorme, AHCI | [ ] | | | |
| 0.4 | FAT e cache de blocos com mutex | [ ] | | | |
| 0.5 | Harness em Python e CI | [ ] | | | |
| 0.6 | Shell: `&`, jobs, fg, wait, `VAR=x cmd`, `&&`, `\|\|`, `;` | [ ] | | | |
| 0.7 | Limites: 256 fds, 256 processos, ulimit | [ ] | | | |
| 1.1 | Pilha inicial do Linux (auxv) | [ ] | | | |
| 1.2 | Syscalls de partida da musl | [ ] | | | |
| 1.3 | Arquivos no formato do Linux | [ ] | | | |
| 1.4 | Memoria do jeito Linux (VMAs, mmap) | [ ] | | | |
| 1.5 | Construir a musl | [ ] | | | |
| 1.6 | Threads (clone) | [ ] | | | |
| 1.7 | futex | [ ] | | | |
| 1.8 | Tempo e sinais completos | [ ] | | | |
| 1.9 | dash, lua, sbase | [ ] | | | |
| 1.10 | Doom | [ ] | | | |
| 1.11 | tcc dentro do Orion | [ ] | | | |
| 1.12 | Programas na musl, aposentar a libc propria | [ ] | | | |
| 2.1 | VFS com inodes | [ ] | | | |
| 2.2 | tmpfs | [ ] | | | |
| 2.3 | procfs | [ ] | | | |
| 2.4 | Cache de paginas unificado | [ ] | | | |
| 2.5 | ext2 leitura | [ ] | | | |
| 2.6 | ext2 escrita, mkfs.ext2, fsck | [ ] | | | |
| 2.7 | Links simbolicos, chmod, chown, utime | [ ] | | | |
| 2.8 | GPT e raiz em ext2 | [ ] | | | |
| 3.1 | TCP parte 1 | [ ] | | | |
| 3.2 | TCP parte 2 | [ ] | | | |
| 3.3 | TCP parte 3 | [ ] | | | |
| 3.4 | TCP parte 4 | [ ] | | | |
| 3.5 | Loopback e AF_UNIX | [ ] | | | |
| 3.6 | wget, nc, httpd, telnet | [ ] | | | |
| 3.7 | DHCP fora do kernel | [ ] | | | |
| 4.1 | uid/gid e permissoes | [ ] | | | |
| 4.2 | passwd, shadow, login | [ ] | | | |
| 4.3 | SMEP e SMAP | [ ] | | | |
| 4.4 | ASLR basico | [ ] | | | |
| 5.1 | uACPI | [ ] | | | |
| 5.2 | MSI/MSI-X | [ ] | | | |
| 5.3 | virtio-blk e virtio-net | [ ] | | | |
| 5.4 | NVMe | [ ] | | | |
| 5.5 | xHCI | [ ] | | | |
| 5.6 | USB HID | [ ] | | | |
| 5.7 | USB armazenamento | [ ] | | | |
| 5.8 | Audio HDA | [ ] | | | |
| 5.9 | Video BGA e virtio-gpu | [ ] | | | |
| 6.1 | Protocolo de janelas | [ ] | | | |
| 6.2 | libgui | [ ] | | | |
| 6.3 | Fontes TrueType | [ ] | | | |
| 6.4 | Imagens PNG/JPEG | [ ] | | | |
| 6.5 | Compositor com areas sujas | [ ] | | | |
| 6.6 | Apps | [ ] | | | |
| 6.7 | Area de trabalho | [ ] | | | |
| 7.1 | ISO hibrida | [ ] | | | |
| 7.2 | Live | [ ] | | | |
| 7.3 | Instalador | [ ] | | | |
| 7.4 | Hardware real | [ ] | | | |
| 7.5 | Pacotes | [ ] | | | |
| 8.1 | Fila por CPU e nice | [ ] | | | |
| 8.2 | Kernel preemptivo | [ ] | | | |
| 8.3 | Fuzzer de syscalls | [ ] | | | |
| 8.4 | OOM | [ ] | | | |
| 8.5 | Perfilador | [ ] | | | |
| 8.6 | Pilhas do kernel medidas | [ ] | | | |

## Diario

### Sessao 1 - 2026-10-16

Ponto de partida (19:22): `.\check.ps1` limpo; `.\regressao.ps1` VERMELHA,
14 de 81. Todas as falhas na sequencia A, do `ping` em diante: o kernel entrou
em panico (`assert falhou: sched_runq_held()` em `wait.c`) no segundo `ping`.
