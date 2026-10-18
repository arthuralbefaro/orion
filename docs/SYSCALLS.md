# Chamadas de sistema do Orion

Numeros do Linux x86_64 onde existe equivalente; os proprios do Orion ficam
acima de 1000. Estado:

- **completa**: faz o que o Linux faz para os casos que os programas usam.
- **parcial**: funciona, mas falta parte do comportamento do Linux (dito na
  coluna de notas).
- **stub**: devolve sucesso sem fazer o trabalho. So existe onde o plano pede.

Toda chamada que recebe ponteiro copia com `copy_from_user`/`copy_to_user` e
devolve `EFAULT` para ponteiro invalido (0.2).

| Num | Nome | Estado | Notas |
|---|---|---|---|
| 0 | read | completa | buffer intermediario no kernel; arquivos regulares em pedacos de ate 64 KiB; em socket TCP le o que chegou, bloqueia ate chegar algo e devolve 0 no FIN do outro lado (3.2) |
| 1 | write | completa | idem; em socket TCP bloqueia enquanto o buffer de envio (32 KiB) esta cheio (3.2) |
| 2 | open | parcial | `O_CREAT`, `O_EXCL`, `O_TRUNC`, `O_APPEND`, `O_NONBLOCK`, `O_DIRECTORY`, `O_CLOEXEC`, `O_NOFOLLOW` (`ELOOP` no link); segue links simbolicos, e `O_CREAT` por um link pendurado cria o alvo (2.7); arquivo novo com o modo pedido menos o `umask` (tmpfs e ext2; o FAT ignora o modo) |
| 3 | close | completa | |
| 4 | stat | completa | `struct stat` do Linux (144 bytes); `st_dev` e o numero da montagem, `st_ino` e estavel enquanto o arquivo existe (2.1); modo, dono, grupo, `st_nlink`, `st_blocks` e datas vem do sistema de arquivos (2.7): ext2 e tmpfs guardam tudo, FAT da as datas da entrada e o bit de somente leitura, devfs e procfs dao dono 0 e a hora do boot; pipe e socket dao a hora do boot |
| 5 | fstat | completa | idem |
| 6 | lstat | completa | nao segue o ultimo link (2.7) |
| 7 | poll | completa | ate 32 descritores; `EINTR` com sinal; socket que escuta fica legivel com conexao na fila (3.4) |
| 8 | lseek | completa | `SEEK_SET`, `SEEK_CUR`, `SEEK_END` |
| 9 | mmap | parcial | anonimo, `MAP_FIXED`, arquivo `MAP_PRIVATE`, `MAP_SHARED` anonimo e de arquivo so leitura, dispositivo (`/dev/fb0`); `MAP_SHARED` de arquivo com escrita da `ENODEV` (2.4) |
| 10 | mprotect | completa | divide areas; `PROT_NONE` preserva paginas populadas |
| 11 | munmap | completa | qualquer faixa alinhada; shootdown de TLB com varias threads |
| 12 | brk | completa | ate 64 MiB |
| 13 | rt_sigaction | completa | `sa_restorer` obrigatorio; `SA_RESTART`, `SA_SIGINFO` (siginfo e ucontext no formato do Linux), `SA_NODEFER`, `SA_RESETHAND`; `SA_ONSTACK` aceito e ignorado (`sigaltstack` e stub); `sigsetsize` = 8 |
| 14 | rt_sigprocmask | completa | mascara no formato Linux (bit `sig-1`), 64 sinais; `sigsetsize` = 8 |
| 15 | rt_sigreturn | completa | volta por `iretq`, restaura registradores e SSE |
| 16 | ioctl | parcial | `TCGETS`, `TCSETS`/`W`/`F`, `TIOCGWINSZ`, `TIOCSWINSZ`, `TIOCGPGRP`, `TIOCSPGRP`, `TIOCINQ`, framebuffer, input e rede (`SIOCGIFCONF_ORION` e `SIOCSIFCONF_ORION`; o `SIOCDHCP_ORION` saiu na 3.7 com o DHCP do kernel) |
| 17 | pread64 | completa | arquivos regulares; `ESPIPE` em pipe, terminal e dispositivo |
| 18 | pwrite64 | completa | idem; escrever depois do fim preenche o buraco com zeros |
| 19 | readv | completa | ate 1024 pedacos |
| 20 | writev | completa | ate 1024 pedacos |
| 21 | access | completa | permissoes com os ids reais (4.1); `W_OK` em montagem so de leitura da `EROFS` |
| 22 | pipe | completa | |
| 23 | select | parcial | sobre o `poll`: ate 32 descritores marcados; prazo restante volta no `timeval` (3.4) |
| 24 | sched_yield | completa | |
| 25 | mremap | parcial | memoria anonima privada: encolhe, cresce no lugar, ou move com `MREMAP_MAYMOVE` (leva as paginas sem copiar); sem `MREMAP_FIXED` nem area de arquivo/compartilhada |
| 28 | madvise | parcial | `MADV_DONTNEED` descarta paginas privadas; as outras dicas sao aceitas sem efeito |
| 32 | dup | completa | |
| 33 | dup2 | completa | ate o `RLIMIT_NOFILE` |
| 34 | pause | completa | dorme ate chegar um sinal; `EINTR` depois do handler |
| 35 | nanosleep | completa | interrompivel: `EINTR` com o tempo restante em `rem`; acorda no tick seguinte ao prazo |
| 36 | getitimer | parcial | so `ITIMER_REAL` |
| 37 | alarm | completa | em cima do `ITIMER_REAL` |
| 38 | setitimer | parcial | so `ITIMER_REAL`, com intervalo; resolucao de 5 ms |
| 39 | getpid | completa | |
| 41 | socket | parcial | `AF_INET` com `SOCK_STREAM` (TCP, 3.2), `SOCK_DGRAM` e `SOCK_RAW`; `AF_UNIX` com `SOCK_STREAM` (3.5); `SOCK_NONBLOCK` e `SOCK_CLOEXEC` |
| 42 | connect | parcial | UDP fixa o destino; TCP faz o handshake (o `SYN` e retransmitido com backoff e desiste em uns 63 s com `ETIMEDOUT`), `ECONNREFUSED` com `RST`, `EINPROGRESS` com `O_NONBLOCK` (3.3) |
| 43 | accept | completa | bloqueia ate ter conexao na fila; endereco do cliente (3.4) |
| 44 | sendto | parcial | UDP e ICMP (broadcast so com `SO_BROADCAST`, senao `EACCES`, 3.7); em TCP e um `write` (o endereco e ignorado); sem `SIGPIPE` com a conexao fechada, so `EPIPE` |
| 45 | recvfrom | parcial | UDP e ICMP; em TCP e um `read` (sem endereco de volta); `flags` ignoradas |
| 46 | sendmsg | parcial | como o `sendto`, juntando ate 1024 `iovec`; sem dados de controle (3.2) |
| 47 | recvmsg | parcial | como o `recvfrom`, espalhando nos `iovec`; `msg_controllen` volta 0, `MSG_TRUNC` em `msg_flags` (3.2) |
| 48 | shutdown | completa | `SHUT_WR` manda `FIN`, `SHUT_RD` faz o `read` devolver 0 (3.4) |
| 49 | bind | parcial | UDP e ICMP; TCP confere a porta (`EADDRINUSE`, respeitando `SO_REUSEADDR`), porta 0 pega uma efemera; o endereco vale so para `listen` (3.4); `AF_UNIX` cria o no `S_IFSOCK` no caminho (tmpfs e ext2; FAT da `EPERM`) (3.5) |
| 50 | listen | completa | `backlog` de 1 a 128, contando conexoes no handshake e na fila (3.4) |
| 51 | getsockname | completa | (3.4) |
| 52 | getpeername | completa | (3.4) |
| 53 | socketpair | parcial | so `AF_UNIX` com `SOCK_STREAM` (3.5) |
| 54 | setsockopt | parcial | `SO_REUSEADDR`; `SO_BROADCAST` em UDP e ICMP (3.7); `TCP_NODELAY` so ligado (nao ha Nagle); o resto da `ENOPROTOOPT` (3.4) |
| 55 | getsockopt | parcial | `SO_REUSEADDR`, `SO_TYPE`, `SO_ERROR`, `SO_RCVBUF`/`SO_SNDBUF` (32 KiB) e `TCP_NODELAY`; o resto da `ENOPROTOOPT` (3.4) |
| 56 | clone | parcial | `CLONE_THREAD` (com `CLONE_VM` e `CLONE_SIGHAND`), `CLONE_SETTLS`, `CLONE_PARENT_SETTID`, `CLONE_CHILD_SETTID`, `CLONE_CHILD_CLEARTID`; sem `CLONE_THREAD` e um fork com pilha opcional (sem `CLONE_VM` de verdade) |
| 57 | fork | completa | copy-on-write; respeita `RLIMIT_NPROC` |
| 58 | vfork | parcial | igual ao `fork` (copy-on-write) |
| 59 | execve | completa | pilha inicial do Linux com `auxv` (1.1); so `ET_EXEC` |
| 60 | exit | completa | encerra so a thread; a ultima encerra o processo |
| 61 | wait4 | completa | `WNOHANG`; `rusage` so com `ru_utime` e `ru_stime` |
| 62 | kill | completa | pid, grupo (`-pgid`) e 0 |
| 63 | uname | completa | |
| 72 | fcntl | parcial | `F_GETFL`/`F_SETFL` (`O_NONBLOCK`), `F_DUPFD`, `F_DUPFD_CLOEXEC`, `F_GETFD`, `F_SETFD`; sem travas de arquivo |
| 74 | fsync | parcial | descarrega tudo (`sync`) |
| 76 | truncate | completa | como o `ftruncate`, pelo caminho |
| 77 | ftruncate | completa | estender preenche com zeros; `EINVAL` em descritor so de leitura |
| 79 | getcwd | completa | |
| 80 | chdir | completa | o processo guarda o vnode do diretorio (2.1); o texto do `getcwd` acompanha |
| 81 | fchdir | completa | |
| 82 | rename | completa | entre diretorios do mesmo sistema de arquivos; `EXDEV` entre montagens; por cima de destino existente (no FAT apaga o destino e depois renomeia, sem ser atomico) |
| 83 | mkdir | completa | modo pedido menos o `umask` (2.7) |
| 84 | rmdir | completa | |
| 87 | unlink | completa | |
| 90 | chmod | completa | grava no inode do ext2 e no tmpfs; no FAT so liga e desliga o somente leitura (`0644`/`0444`, `0755`/`0555`), o resto da `EPERM`, como o `vfat` do Linux; devfs e procfs dao `EPERM`; `EROFS` em montagem so de leitura; so o dono ou root (4.1) (2.7) |
| 91 | fchmod | completa | como o `chmod`, pelo descritor; descritor sem vnode (pipe, socket) da `EINVAL` |
| 92 | chown | completa | `-1` mantem o campo; tira `S_ISUID` (e `S_ISGID` com execucao do grupo) de quem nao e diretorio, como o Linux; dono de 32 bits no ext2; no FAT so aceita dono 0; so root troca o dono; o dono troca o grupo para um grupo dele (4.1) (2.7) |
| 93 | fchown | completa | como o `chown`, pelo descritor |
| 94 | lchown | completa | nao segue o ultimo link |
| 95 | umask | completa | por processo, herdado no `fork`, 022 no boot (2.7) |
| 88 | symlink | completa | ext2 (alvo de ate 59 bytes dentro do inode, maior num bloco) e tmpfs; `EPERM` no FAT; alvo vazio da `ENOENT` (2.7) |
| 89 | readlink | completa | nao segue o ultimo link; `EINVAL` para o que nao e link (2.7) |
| 96 | gettimeofday | completa | fuso sempre zero |
| 97 | getrlimit | completa | `RLIMIT_NOFILE`, `RLIMIT_NPROC`, `RLIMIT_STACK`; os outros sao infinitos |
| 98 | getrusage | parcial | `RUSAGE_SELF`, `RUSAGE_CHILDREN`, `RUSAGE_THREAD`; so `ru_utime` e `ru_stime` |
| 100 | times | completa | em ticks de 100 Hz (o `CLK_TCK` da musl) |
| 102 | getuid | completa | (4.1) |
| 104 | getgid | completa | (4.1) |
| 105 | setuid | completa | regras do Linux; root troca os tres ids (4.1) |
| 106 | setgid | completa | idem (4.1) |
| 107 | geteuid | completa | (4.1) |
| 108 | getegid | completa | (4.1) |
| 109 | setpgid | completa | |
| 110 | getppid | completa | |
| 113 | setreuid | completa | (4.1) |
| 114 | setregid | completa | (4.1) |
| 115 | getgroups | completa | ate 32 grupos (4.1) |
| 116 | setgroups | completa | so root; ate 32 grupos (4.1) |
| 117 | setresuid | completa | (4.1) |
| 118 | getresuid | completa | (4.1) |
| 119 | setresgid | completa | (4.1) |
| 120 | getresgid | completa | (4.1) |
| 121 | getpgid | completa | |
| 127 | rt_sigpending | completa | |
| 128 | rt_sigtimedwait | completa | `siginfo` com a origem (`kill`, `tkill`, `SIGCHLD`, falta de pagina); `si_uid` sempre 0 |
| 130 | rt_sigsuspend | completa | a mascara volta depois do handler |
| 131 | sigaltstack | stub | aceita e informa `SS_DISABLE`; os sinais rodam sempre na pilha normal (o plano pede stub) |
| 132 | utime | completa | segundos; `NULL` usa a hora atual (2.7) |
| 158 | arch_prctl | parcial | `ARCH_SET_FS` e `ARCH_GET_FS` |
| 160 | setrlimit | completa | subir o maximo da `EPERM` |
| 162 | sync | completa | |
| 169 | reboot | completa | so root; `RESTART`, `POWER_OFF` e `HALT` pelo ACPI (uACPI); `CAD_ON`/`CAD_OFF` aceitos sem efeito (5.1) |
| 165 | mount | parcial | tipos `vfat` (`fat`, `msdos`) e `ext2` com dispositivo de bloco, `devfs` (`devtmpfs`), `tmpfs` (opcoes `size=`, `nr_inodes=`, `mode=`, `uid=` e `gid=`) e `proc`; `MS_RDONLY`; `MS_NOSUID`, `MS_NODEV`, `MS_NOEXEC`, `MS_SYNCHRONOUS`, `MS_SILENT` e os de `atime` aceitos sem efeito; `MS_REMOUNT`, `MS_BIND` e `MS_MOVE` dao `EINVAL`; um ponto de montagem por diretorio |
| 166 | umount2 | parcial | `EBUSY` com arquivo aberto, cwd ou montagem dentro; so a flag `UMOUNT_NOFOLLOW` (`MNT_FORCE`, `MNT_DETACH`, `MNT_EXPIRE` dao `EINVAL`) |
| 186 | gettid | completa | tid de cada thread; a principal tem o pid |
| 200 | tkill | completa | |
| 201 | time | completa | |
| 202 | futex | parcial | `FUTEX_WAIT` (timeout relativo), `FUTEX_WAKE`, `FUTEX_REQUEUE`, `FUTEX_CMP_REQUEUE`, com e sem `FUTEX_PRIVATE_FLAG`; sem as operacoes PI, `WAKE_OP` e `*_BITSET` (a musl nao usa) |
| 204 | sched_getaffinity | completa | mascara com todas as CPUs online (a musl usa para contar CPUs) |
| 217 | getdents64 | completa | `d_type` preenchido; `lseek(fd, 0, SEEK_SET)` recomeca |
| 218 | set_tid_address | completa | zera a palavra na saida da thread e acorda um esperador do futex nela |
| 219 | restart_syscall | completa | so o kernel a usa, para retomar `nanosleep` e `poll` com o prazo que sobrava |
| 228 | clock_gettime | completa | `REALTIME`, `MONOTONIC`, `PROCESS_CPUTIME_ID`, `THREAD_CPUTIME_ID`, `MONOTONIC_RAW`, `*_COARSE`, `BOOTTIME`, `TAI` (igual ao `REALTIME`) |
| 229 | clock_getres | completa | 1 us nos relogios de parede e de CPU, 1 ms nos `COARSE` |
| 230 | clock_nanosleep | parcial | `REALTIME`, `MONOTONIC`, `BOOTTIME`, `TAI`, relativo ou `TIMER_ABSTIME`; relogios de CPU dao `EOPNOTSUPP` |
| 231 | exit_group | completa | encerra todas as threads do processo |
| 234 | tgkill | completa | |
| 235 | utimes | completa | microssegundos; `tv_usec` fora de 0 a 999999 da `EINVAL` (2.7) |
| 257 | openat | completa | `AT_FDCWD` ou descritor de diretorio; o caminho e resolvido componente a componente, `..` atravessa montagens (2.1) |
| 258 | mkdirat | completa | idem; modo pedido menos o `umask` |
| 260 | fchownat | completa | `AT_SYMLINK_NOFOLLOW`, `AT_EMPTY_PATH` (2.7) |
| 261 | futimesat | completa | como o `utimes`, relativo ao descritor (2.7) |
| 262 | newfstatat | completa | `AT_EMPTY_PATH`; `AT_SYMLINK_NOFOLLOW` nao segue o ultimo link (2.7) |
| 263 | unlinkat | completa | `AT_REMOVEDIR` |
| 264 | renameat | completa | |
| 266 | symlinkat | completa | como o `symlink` |
| 267 | readlinkat | completa | como o `readlink` |
| 268 | fchmodat | completa | como o `chmod`, relativo ao descritor |
| 269 | faccessat | completa | como o `access` |
| 280 | utimensat | completa | nanossegundos no tmpfs, segundos no ext2 (ate 2038), 2 s no FAT (so `mtime` e o dia do `atime`); `UTIME_NOW`, `UTIME_OMIT`, `AT_SYMLINK_NOFOLLOW`, `AT_EMPTY_PATH`, caminho `NULL` com descritor (o `futimens` da musl); muda o `ctime` para agora (2.7) |
| 288 | accept4 | completa | `SOCK_NONBLOCK`, `SOCK_CLOEXEC` (3.4) |
| 292 | dup3 | completa | `O_CLOEXEC` |
| 293 | pipe2 | completa | `O_CLOEXEC`, `O_NONBLOCK` |
| 302 | prlimit64 | parcial | so para o proprio processo |
| 439 | faccessat2 | completa | `AT_EACCESS` (ids efetivos), `AT_SYMLINK_NOFOLLOW`, `AT_EMPTY_PATH` (4.1) |
| 452 | fchmodat2 | completa | `AT_SYMLINK_NOFOLLOW` (num link da `EOPNOTSUPP`, como o Linux), `AT_EMPTY_PATH` (2.7) |
| 1000 | readdir | propria | uma entrada por chamada; a libc usa o `getdents64` |
| 1001 | openpty | propria | |
| 1002 | kfault | propria | so root; faz o kernel executar (1) ou ler (2) um endereco de usuario, para testar SMEP e SMAP (`crash smep`/`crash smap`); `EOPNOTSUPP` sem a protecao (4.3) |

Chamadas bloqueantes interrompidas por sinal (pipe, terminal, pty, `/dev/input`,
`recvfrom`, `wait4`, `futex` sem prazo) sao reiniciadas sozinhas quando o
handler tem `SA_RESTART`; sem ele, devolvem `EINTR`. `poll`, `nanosleep`,
`pause` e `rt_sigsuspend` devolvem `EINTR` depois de um handler, e sao
retomadas (com o prazo que sobrava) quando o sinal nao chega a rodar handler
nesta thread, como no Linux. 219 (`restart_syscall`) e interna: e o que a
retomada de uma espera com prazo usa.

1002 (`sysinfo`) e 1003 (`proclist`), proprias do Orion, sairam na 2.3: a
mesma informacao esta em `/proc/meminfo`, `/proc/uptime` e `/proc/<pid>/status`.

Qualquer outro numero devolve `ENOSYS` e deixa um aviso no log do kernel.
