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
| 0 | read | completa | buffer intermediario no kernel; arquivos regulares em pedacos de ate 64 KiB |
| 1 | write | completa | idem |
| 2 | open | parcial | `O_CREAT`, `O_TRUNC`, `O_APPEND`, `O_NONBLOCK`; sem modo de permissao |
| 3 | close | completa | |
| 4 | stat | parcial | `struct stat` ainda no formato proprio (o do Linux chega na 1.3) |
| 5 | fstat | parcial | idem |
| 7 | poll | completa | ate 32 descritores; `EINTR` com sinal |
| 8 | lseek | completa | `SEEK_SET`, `SEEK_CUR`, `SEEK_END` |
| 9 | mmap | parcial | anonimo e de dispositivo (`/dev/fb0`); sem `MAP_FIXED` nem arquivo (1.4) |
| 11 | munmap | parcial | so na faixa do `mmap` |
| 12 | brk | completa | ate 64 MiB |
| 13 | rt_sigaction | completa | `sa_restorer` obrigatorio; `SA_RESTART` aceito e ignorado; `sigsetsize` = 8 |
| 14 | rt_sigprocmask | completa | mascara no formato Linux (bit `sig-1`), 64 sinais; `sigsetsize` = 8 |
| 15 | rt_sigreturn | completa | volta por `iretq`, restaura registradores e SSE |
| 16 | ioctl | parcial | `TCGETS`, `TCSETS`/`W`/`F`, `TIOCGWINSZ`, `TIOCSWINSZ`, `TIOCGPGRP`, `TIOCSPGRP`, `TIOCINQ`, framebuffer, input e rede |
| 19 | readv | completa | ate 1024 pedacos |
| 20 | writev | completa | ate 1024 pedacos |
| 22 | pipe | completa | |
| 24 | sched_yield | completa | |
| 32 | dup | completa | |
| 33 | dup2 | completa | ate o `RLIMIT_NOFILE` |
| 35 | nanosleep | parcial | resolucao de 1 ms; nao devolve o tempo restante |
| 37 | alarm | completa | |
| 39 | getpid | completa | |
| 41 | socket | parcial | `AF_INET` com `SOCK_DGRAM` e `SOCK_RAW` (TCP na fase 3) |
| 42 | connect | parcial | so UDP |
| 44 | sendto | parcial | so UDP e ICMP |
| 45 | recvfrom | parcial | idem |
| 49 | bind | parcial | idem |
| 57 | fork | completa | copy-on-write; respeita `RLIMIT_NPROC` |
| 59 | execve | completa | pilha inicial do Linux com `auxv` (1.1); so `ET_EXEC` |
| 60 | exit | completa | |
| 61 | wait4 | parcial | `WNOHANG`; sem `rusage` |
| 62 | kill | completa | pid, grupo (`-pgid`) e 0 |
| 63 | uname | completa | |
| 72 | fcntl | parcial | `F_GETFL`/`F_SETFL` (`O_NONBLOCK`); o resto chega na 1.3 |
| 74 | fsync | parcial | descarrega tudo (`sync`) |
| 77 | ftruncate | completa | |
| 79 | getcwd | completa | |
| 80 | chdir | completa | |
| 82 | rename | completa | |
| 83 | mkdir | parcial | sem modo de permissao |
| 84 | rmdir | completa | |
| 87 | unlink | completa | |
| 97 | getrlimit | completa | `RLIMIT_NOFILE`, `RLIMIT_NPROC`, `RLIMIT_STACK`; os outros sao infinitos |
| 102 | getuid | stub | sempre 0 ate a 4.1 (plano: "todos 0 por enquanto") |
| 104 | getgid | stub | idem |
| 107 | geteuid | stub | idem |
| 108 | getegid | stub | idem |
| 109 | setpgid | completa | |
| 110 | getppid | completa | |
| 121 | getpgid | completa | |
| 131 | sigaltstack | stub | aceita e informa `SS_DISABLE`; os sinais rodam sempre na pilha normal (o plano pede stub) |
| 158 | arch_prctl | parcial | `ARCH_SET_FS` e `ARCH_GET_FS` |
| 160 | setrlimit | completa | subir o maximo da `EPERM` |
| 162 | sync | completa | |
| 186 | gettid | parcial | igual ao pid enquanto cada processo tem uma thread so (1.6) |
| 218 | set_tid_address | parcial | guarda o endereco e zera a palavra na saida; o `futex` wake chega na 1.7 |
| 228 | clock_gettime | parcial | `CLOCK_REALTIME` e `CLOCK_MONOTONIC` (o resto na 1.8) |
| 231 | exit_group | completa | igual ao `exit` enquanto ha uma thread por processo |
| 302 | prlimit64 | parcial | so para o proprio processo |
| 1000 | readdir | propria | uma entrada por chamada (o `getdents64` chega na 1.3) |
| 1001 | openpty | propria | |
| 1002 | sysinfo | propria | sai na 2.3 (`/proc`) |
| 1003 | proclist | propria | sai na 2.3 (`/proc`) |

Qualquer outro numero devolve `ENOSYS` e deixa um aviso no log do kernel.
