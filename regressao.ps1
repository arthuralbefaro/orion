param(
    [int]$Smp = 4,
    [switch]$NoBuild,
    [switch]$OnlyA,
    [switch]$OnlyB,
    [switch]$OnlyC,
    [switch]$OnlyD,
    [switch]$OnlyE,
    [int]$StressRuns = 1,
    [switch]$Verbose
)

$ErrorActionPreference = "Stop"
$Root = $PSScriptRoot
$failures = @()
$checks = 0
$runAll = -not ($OnlyA -or $OnlyB -or $OnlyC -or $OnlyD -or $OnlyE)

function Get-Log([string]$path) {
    return (Get-Content (Join-Path $Root $path) -Raw -ErrorAction SilentlyContinue)
}

function Expect([string]$log, [string]$pattern, [string]$what) {
    $script:checks++
    if ($log -match $pattern) {
        Write-Host ("  [ok]    " + $what) -ForegroundColor Green
    } else {
        Write-Host ("  [FALHA] " + $what + "   (padrao: " + $pattern + ")") -ForegroundColor Red
        $script:failures += $what
    }
}

function ExpectNot([string]$log, [string]$pattern, [string]$what) {
    $script:checks++
    if ($log -notmatch $pattern) {
        Write-Host ("  [ok]    " + $what) -ForegroundColor Green
    } else {
        Write-Host ("  [FALHA] " + $what + "   (achou: " + $Matches[0] + ")") -ForegroundColor Red
        $script:failures += $what
    }
}

function Test-MemDelta([string]$log, [string]$what, [int]$max) {
    $script:checks++
    $m = [regex]::Matches($log, "Paginas: \d+ total, (\d+) usadas")
    if ($m.Count -lt 2) {
        Write-Host ("  [FALHA] " + $what + "   (precisa de dois 'mem', achei " + $m.Count + ")") -ForegroundColor Red
        $script:failures += $what
        return
    }
    $a = [int]$m[0].Groups[1].Value
    $b = [int]$m[$m.Count - 1].Groups[1].Value
    $d = [Math]::Abs($b - $a)
    if ($d -le $max) {
        Write-Host ("  [ok]    " + $what + " ($a -> $b, delta $d)") -ForegroundColor Green
    } else {
        Write-Host ("  [FALHA] " + $what + " ($a -> $b, delta $d > $max)") -ForegroundColor Red
        $script:failures += $what
    }
}

function Test-ShotColors([string]$ppm, [string]$what, [int]$min) {
    $script:checks++
    $path = Join-Path $Root $ppm
    if (-not (Test-Path $path)) {
        Write-Host ("  [FALHA] " + $what + "   (sem a captura " + $ppm + ")") -ForegroundColor Red
        $script:failures += $what
        return
    }
    $bytes = [System.IO.File]::ReadAllBytes($path)
    $lines = 0
    $i = 0
    while ($i -lt $bytes.Length -and $lines -lt 3) {
        if ($bytes[$i] -eq 10) { $lines++ }
        $i++
    }
    $seen = @{}
    for ($p = $i; $p + 2 -lt $bytes.Length; $p += 3 * 997) {
        $seen[[int]$bytes[$p] * 65536 + [int]$bytes[$p + 1] * 256 + [int]$bytes[$p + 2]] = $true
    }
    $n = $seen.Count
    if ($n -ge $min) {
        Write-Host ("  [ok]    " + $what + " ($n cores na amostra)") -ForegroundColor Green
    } else {
        Write-Host ("  [FALHA] " + $what + " ($n cores na amostra, minimo $min)") -ForegroundColor Red
        $script:failures += $what
    }
}

$buildFlag = @{}
if ($NoBuild) { $buildFlag = @{ NoBuild = $true } }
if (-not $NoBuild -and -not ($runAll -or $OnlyA)) {
    Write-Host "== Build ==" -ForegroundColor Cyan
    & (Join-Path $Root "build.ps1") | Out-Null
}

# ---------------- Sequencia A: texto ----------------
if ($runAll -or $OnlyA) {
    Write-Host "== Sequencia A (texto) ==" -ForegroundColor Cyan
    $stepsA = @(
        "key:0B", "cmd:mem", "key:0B", "wait:1",
        "cmd:ls /bin | wc", "wait:1",
        "cmd:memtest", "wait:2",
        "cmd:crash", "wait:2",
        "cmd:sigtest", "until:180:sigtest: fim",
        "cmd:badptr", "until:40:badptr: fim",
        "cmd:fputest", "until:60:fputest: fim", "cmd:auxtest", "until:30:auxtest: fim",
        "cmd:sctest", "until:30:sctest: fim", "cmd:filetest", "until:60:filetest: fim",
        "cmd:mhello", "until:30:mhello: fim",
        "cmd:mthreads", "until:120:mthreads: fim", "cmd:mfutex", "until:240:mfutex: fim",
        "cmd:dash", "wait:2",
        "cmd:for i in 1 2 3; do echo laco`$i; done", "wait:2",
        "cmd:x=`$(echo oi); echo sub[`$x] conta=`$((6*7))", "wait:2",
        "cmd:f() { echo arg=`$1; }; f um; case abc in a*) echo casou;; esac", "wait:2",
        "cmd:printf 'x\ny\n' | while read l; do echo linha=`$l; done", "wait:2",
        "cmd:lua -e 'print(2^10)'", "wait:3",
        "cmd:lua -e 'print(os.execute([[echo dentro-do-lua]]))'", "wait:3",
        "cmd:seq 1 10 | paste -s -d + -; echo orion | tr a-z A-Z | cut -c2-; find /bin -name 'lu*'", "wait:4",
        "cmd:test -x /bin/dash && echo dash-executavel", "wait:2",
        "cmd:tcc /lib/tcc/hello.c -o /tcchello && /tcchello", "until:120:ola do tcc",
        "cmd:printf '#include <stdlib.h>\nint main(void) { return atoi(`"7`"); }\n' > /tcc7.c; tcc /tcc7.c -o /tcc7; /tcc7; echo tcc-status=`$?; rm /tcc7.c /tcc7 /tcchello", "until:120:tcc-status=7",
        "cmd:exit 7", "wait:2",
        "cmd:sleep 100", "wait:2", "key:03", "wait:2",
        "cmd:hello um dois", "wait:1",
        "cmd:date", "wait:1",
        "cmd:ls /dev", "wait:1",
        "cmd:vfstest -n /", "until:60:falha(s))",
        "cmd:tmptest", "until:400:falha(s))",
        "cmd:vfstest /tmp", "until:200:falha(s))",
        "cmd:linktest /tmp", "until:120:falha(s))",
        "cmd:ln -s alvo-do-ln /tmp/lnx", "cmd:readlink /tmp/lnx", "cmd:rm /tmp/lnx", "wait:1",
        "cmd:attrtest /tmp", "until:120:falha(s))", "cmd:attrtest -k /tmp", "until:60:falha(s))",
        "cmd:touch /tmp/lsx", "cmd:chmod 640 /tmp/lsx", "cmd:chown 12:34 /tmp/lsx", "cmd:ln -s lsx /tmp/lsl",
        "cmd:ls -l /tmp/lsx /tmp/lsl", "wait:2", "cmd:rm /tmp/lsx /tmp/lsl", "cmd:ls -l /bin/ls", "wait:1",
        "cmd:cat /proc/self/maps", "wait:1", "cmd:cat /proc/mounts", "wait:1",
        "cmd:proctest um dois", "until:60:falha(s))",
        "cmd:pctest", "until:120:falha(s))",
        "cmd:polltest", "wait:6",
        "cmd:fbtest 2", "wait:1", "shot:out\shots\reg-fbtest.ppm", "wait:2",
        "cmd:evtest 10", "wait:0.8", "mon:sendkey a", "mon:sendkey ctrl", "mon:sendkey up", "mon:mouse_move 10 5", "until:20:evento(s)",
        "cmd:ptytest", "wait:8",
        "cmd:echo alfa", "wait:1",
        "keys:1B5B41", "raw: beta", "key:0D", "wait:1.5",
        "keys:1B5B41", "keys:1B5B44", "keys:1B5B44", "keys:1B5B44", "keys:1B5B44",
        "raw:X", "key:0D", "wait:1.5",
        "keys:1B5B41", "keys:1B5B48", "keys:1B5B46",
        "key:7F", "key:7F", "key:7F", "key:7F", "key:0D", "wait:1.5",
        "raw:cho homeok", "keys:1B5B48", "raw:e", "key:0D", "wait:1.5",
        "raw:eccho delok", "keys:1B5B48", "keys:1B5B43", "keys:1B5B43", "keys:1B5B337E",
        "key:0D", "wait:1.5",
        "cmd:history", "wait:1",
        "cmd:forktest", "wait:30",
        "cmd:forktest -s 6", "wait:3", "key:0B", "cmd:mem", "key:0B", "wait:12",
        "cmd:dhcp", "wait:8",
        "cmd:ifconfig", "wait:2",
        "cmd:ping 10.0.2.2 3", "wait:10",
        "cmd:nslookup example.com", "wait:8",
        "cmd:nslookup example.com", "wait:8",
        "cmd:GREETING=ola-mundo", "wait:1",
        'cmd:echo eco-$GREETING-fim', "wait:1",
        "cmd:printenv SHELL", "wait:1",
        "cmd:env | grep HOME", "wait:1",
        "cmd:top 1", "wait:3",
        "cmd:ps -l", "wait:1",
        "cmd:uptime", "cmd:free", "wait:1",
        "cmd:time sleep 1", "wait:3",
        "cmd:kill -l", "cmd:kill 9999", "cmd:true", "wait:1",
        "cmd:cat /dev/null | wc", "wait:1",
        "cmd:head -c 1000 /dev/zero | wc", "wait:1",
        "cmd:head -c 64 /dev/random | wc", "wait:1",
        "cmd:stress 300 8", "until:240:stress: 300 processos",
        "cmd:unset GREETING", 'cmd:echo vazio[$GREETING]', "wait:1",
        "cmd:true && echo e-ok", "cmd:false && echo NAO-DEVIA", "cmd:false || echo ou-ok",
        "cmd:echo um ; echo dois", 'cmd:false ; echo status=$?',
        "cmd:XV=abc printenv XV", "cmd:printenv XV || echo XV-sumiu",
        "cmd:sleep 3 &", "cmd:jobs", "cmd:wait", "wait:1",
        "cmd:sleep 2 & sleep 1 & jobs", "cmd:fg", "wait:3", "cmd:jobs", "wait:1",
        "cmd:limtest", "until:60:limtest: fim",
        "cmd:ulimit -a", "cmd:stress 220 220 9000 &", "wait:6", "cmd:ps | wc", "cmd:wait", "until:120:pronto  stress",
        "key:0B", "cmd:heaptest", "wait:1", "cmd:preempt", "wait:2",
        "cmd:arp", "cmd:net",
        "cmd:dev", "cmd:pty", "cmd:mutex", "cmd:mem"
    )
    & (Join-Path $Root "test.ps1") -Smp $Smp @buildFlag -LogPath "out\reg-a.log" `
        -Script $stepsA -CommandDelay 1.5 -BootTimeout 60 -NoPrint | Out-Null
    $a = Get-Log "out\reg-a.log"
    if ($Verbose) { Write-Output $a }

    Expect $a "Orion sh \(pid \d+\)" "shell de usuario subiu"
    Expect $a "ls /bin \| wc[\s\S]{0,80}?\d+\s+\d+\s+\d+" "ls /bin | wc produziu contagem"
    Expect $a "resultado: TUDO OK" "memtest TUDO OK"
    Expect $a "tocar 10 paginas aloca so essas paginas\s+ok" "mmap anonimo aloca sob demanda"
    Expect $a "MAP_FIXED troca so a pagina pedida\s+ok" "MAP_FIXED"
    Expect $a "a pagina do meio virou SIGSEGV\s+ok" "munmap parcial"
    Expect $a "escrever numa pagina so de leitura da SIGSEGV\s+ok" "mprotect PROT_READ"
    Expect $a "PROT_NONE e volta preserva o conteudo\s+ok" "mprotect PROT_NONE e volta"
    Expect $a "MAP_SHARED anonimo e visto pelo pai depois do fork\s+ok" "MAP_SHARED anonimo entre pai e filho"
    Expect $a "MAP_SHARED tocado so depois do fork tambem e compartilhado\s+ok" "MAP_SHARED tocado depois do fork"
    Expect $a "filho que reabre a escrita nao muda a pagina do pai\s+ok" "mprotect no filho nao vaza para o pai"
    Expect $a "escrever no MAP_PRIVATE nao muda o arquivo\s+ok" "mmap MAP_PRIVATE de arquivo"
    Expect $a "pilha cresce alem de 64 KiB" "pilha de 8 MiB sob demanda"
    Expect $a "#PF Page Fault" "crash levou page fault"
    Expect $a "morto por sinal 11" "crash morto com SIGSEGV"
    Expect $a "sigtest: fim, TUDO OK" "sigtest (musl) TUDO OK"
    Expect $a "sigsuspend volta com EINTR depois do handler +ok" "sigsuspend"
    Expect $a "sigsuspend restaura a mascara com SIGUSR1 bloqueado +ok" "sigsuspend restaura a mascara"
    Expect $a "sigtimedwait recebe SIGUSR2 com si_signo +ok" "rt_sigtimedwait"
    Expect $a "sigtimedwait sem sinal expira com EAGAIN +ok" "rt_sigtimedwait com prazo"
    Expect $a "pthread_kill entrega o sinal a thread alvo +ok" "tgkill/pthread_kill"
    Expect $a "read com SA_RESTART continua e le o dado +ok" "SA_RESTART reinicia o read"
    Expect $a "read sem SA_RESTART volta com -1 e EINTR +ok" "sem SA_RESTART o read da EINTR"
    Expect $a "nanosleep devolve em rem o tempo que faltou +ok" "nanosleep interrompido devolve o resto"
    Expect $a "clock_nanosleep TIMER_ABSTIME em CLOCK_MONOTONIC +ok" "clock_nanosleep absoluto"
    Expect $a "clock_gettime aceita os 8 relogios +ok" "todos os relogios do clock_gettime"
    Expect $a "CLOCK_PROCESS_CPUTIME_ID cresce com laco ocupado +ok" "tempo de CPU do processo"
    Expect $a "getrusage\(RUSAGE_CHILDREN\) soma o filho esperado +ok" "getrusage dos filhos"
    Expect $a "wait4 devolve o status e o rusage do filho +ok" "wait4 com rusage"
    Expect $a "ITIMER_REAL de 50 ms dispara SIGALRM 3 vezes +ok" "setitimer com intervalo"
    Expect $a "SA_SIGINFO recebe si_pid do kill e o ucontext +ok" "SA_SIGINFO com siginfo e ucontext"
    Expect $a "sigtimedwait recebe SIGCHLD bloqueado com pid e status +ok" "SIGCHLD bloqueado nao e descartado"
    Expect $a "pthread_cancel cancela a thread presa num read +ok" "pthread_cancel (ucontext no formato do Linux)"
    Expect $a "rcx e r11 sobrevivem a um sinal\s+ok" "rt_sigreturn devolve todos os registradores"
    Expect $a "badptr: fim, (\d+)/\1 TUDO OK" "badptr: ponteiros invalidos devolvem EFAULT sem panico"
    Expect $a "handler de sinal nao canonico\s+SIGSEGV" "handler nao canonico vira SIGSEGV"
    Expect $a "fputest: fim, TUDO OK" "fputest: xmm e mxcsr por processo, em sinal e no fork"
    Expect $a "xmm0\.\.xmm15 zerados na entrada\s+ok" "exec comeca com o estado de SSE limpo"
    Expect $a "AT_PHDR aponta para os cabecalhos de verdade\s+ok" "auxv: AT_PHDR"
    Expect $a "AT_ENTRY = _start\s+ok" "auxv: AT_ENTRY"
    Expect $a "AT_RANDOM muda a cada exec\s+ok" "auxv: AT_RANDOM com 16 bytes novos a cada exec"
    Expect $a "auxtest: fim, TUDO OK" "auxtest: todos os campos do auxv"
    Expect $a "readv espalha em 2 buffers\s+ok" "readv e writev"
    Expect $a "sinal de tempo real 40 entregue ao handler\s+ok" "64 sinais (tempo real)"
    Expect $a "sigaddset usa o bit sig-1 do Linux\s+ok" "mascara de sinais no formato Linux"
    Expect $a "TCGETS no formato do Linux\s+ok" "termios no formato do Linux"
    Expect $a "sctest: fim, TUDO OK" "sctest: syscalls de partida da musl"
    Expect $a "struct stat tem 144 bytes\s+ok" "struct stat no layout do Linux"
    Expect $a "getdents64 lista /bin com sh \(DT_REG\)\s+ok" "getdents64"
    Expect $a "openat relativo a um diretorio\s+ok" "openat relativo a um descritor de diretorio"
    Expect $a "exec fecha FD_CLOEXEC e mantem os outros\s+ok" "O_CLOEXEC/FD_CLOEXEC fechados no exec"
    Expect $a "filetest: fim, TUDO OK" "filetest: arquivos no formato do Linux"
    Expect $a "mhello: programa compilado contra a musl" "programa da musl (zig cc) roda no Orion"
    Expect $a "variavel __thread \(TLS\)\s+ok" "musl: TLS"
    Expect $a "printf com ponto flutuante\s+ok" "musl: printf com ponto flutuante (SSE)"
    Expect $a "mhello: fim, TUDO OK" "mhello: partida da musl completa"
    Expect $a "realloc grande preserva o conteudo \(mremap\) +ok" "mremap pelo realloc da musl"
    Expect $a "mremap move a area e leva as paginas +ok" "mremap com MREMAP_MAYMOVE"
    Expect $a "mremap cresce no lugar sem MAYMOVE +ok" "mremap cresce no lugar"
    Expect $a "ola do tcc dentro do Orion: 42" "tcc compila e roda C dentro do Orion"
    Expect $a "tcc-status=7" "programa do tcc devolve o codigo de saida"
    Expect $a "pthread_create de 8 threads +ok" "mthreads: 8 threads da musl criadas"
    Expect $a "8 threads somando num contador atomico +ok" "mthreads: contador atomico entre threads"
    Expect $a "contador protegido por pthread_mutex +ok" "mthreads: pthread_mutex"
    Expect $a "__thread separado por thread +ok" "mthreads: TLS por thread"
    Expect $a "exit numa thread encerra o processo inteiro +ok" "mthreads: exit encerra todas as threads"
    Expect $a "fork de processo com threads gera filho com uma thread +ok" "mthreads: fork com threads"
    Expect $a "exit com outra thread presa num read +ok" "mthreads: exit acorda thread presa num read"
    Expect $a "execve com outra thread presa num read +ok" "mthreads: execve com thread presa num read"
    Expect $a "execve feito por uma thread que nao e a principal +ok" "mthreads: execve a partir de outra thread"
    Expect $a "mmap/munmap em 4 threads ao mesmo tempo +ok" "mthreads: mmap/munmap concorrente entre threads"
    Expect $a "handler de SIGCHLD colhe todos com WNOHANG +ok" "mthreads: SIGCHLD so chega quando o filho pode ser colhido"
    Expect $a "mthreads: fim, TUDO OK" "mthreads: threads da musl completas"
    Expect $a "pthread_mutex entre 8 threads, 1 milhao de operacoes +ok" "mfutex: 1 milhao de operacoes de mutex entre 8 threads"
    Expect $a "condvar produtor/consumidor, 4\+4 threads, fila de 4 +ok" "mfutex: condvar produtor/consumidor"
    Expect $a "pthread_barrier, 8 threads, 2000 rodadas +ok" "mfutex: pthread_barrier"
    Expect $a "sem_wait/sem_post entre threads +ok" "mfutex: semaforo"
    Expect $a "pthread_cond_timedwait expira com ETIMEDOUT +ok" "mfutex: timedwait expira"
    Expect $a "FUTEX_WAIT com timeout devolve ETIMEDOUT +ok" "mfutex: FUTEX_WAIT com timeout"
    Expect $a "sinal interrompe FUTEX_WAIT com EINTR +ok" "mfutex: sinal interrompe FUTEX_WAIT"
    Expect $a "FUTEX_WAIT com timeout enorme continua esperando +ok" "mfutex: timeout enorme nao vence na hora"
    Expect $a "fork com uma thread em FUTEX_WAIT +ok" "mfutex: fork com thread no futex"
    Expect $a "FUTEX_CMP_REQUEUE acorda 1 e move 3 +ok" "mfutex: FUTEX_CMP_REQUEUE"
    Expect $a "mutex compartilhado entre dois processos \(MAP_SHARED\) +ok" "mfutex: futex compartilhado entre processos"
    Expect $a "mfutex: fim, TUDO OK" "mfutex: futex completo"
    Expect $a "laco1\s+laco2\s+laco3" "dash: for i in 1 2 3"
    Expect $a "sub\[oi\] conta=42" "dash: substituicao de comando e aritmetica"
    Expect $a "arg=um\s+casou" "dash: funcao e case"
    Expect $a "linha=x\s+linha=y" "dash: pipe com while read"
    Expect $a "(?m)^1024\.0" "lua -e 'print(2^10)'"
    Expect $a "dentro-do-lua\s+true\s+exit\s+0" "lua: os.execute pelo /bin/sh -c (posix_spawn)"
    Expect $a "1\+2\+3\+4\+5\+6\+7\+8\+9\+10" "sbase: seq e paste"
    Expect $a "(?m)^RION" "sbase: tr e cut"
    Expect $a "/bin/lua" "sbase: find"
    Expect $a "dash-executavel" "dash: test -x (faccessat2)"
    Expect $a "saiu com 7" "dash: exit 7 volta para o sh com o codigo"
    Expect $a "futex: 0 thread\(s\) esperando" "nenhuma thread esquecida na fila do futex"
    ExpectNot $a "syscall \d+ desconhecida" "nenhuma syscall desconhecida na sequencia A"
    Expect $a "flag NT ligada dentro do handler\s+ok" "flag NT do usuario nao derruba o iretq do rt_sigreturn"
    Expect $a "Copias de usuario: [1-9]\d* falta\(s\) recuperada\(s\)" "tabela de excecoes recuperou as faltas"
    Expect $a "morto por sinal 2" "Ctrl\+C matou o sleep com SIGINT"
    Expect $a "hello from ring 3" "hello rodou"
    Expect $a "argv\[2\] = dois" "argumentos chegaram ao hello"
    Expect $a "UTC 20\d\d" "date imprimiu a data"
    Expect $a "fb0" "ls /dev listou fb0"
    Expect $a "/dev/\.\. e a raiz\s+ok" "vfstest: .. atravessa a montagem do /dev"
    Expect $a "openat\(fd de /dev, `"\.\.`"\) e a raiz\s+ok" "vfstest: openat relativo com .. sai da montagem"
    Expect $a "/dev esta em outro sistema de arquivos\s+ok" "vfstest: st_dev diferente por montagem"
    Expect $a "umount do que nao e montagem da EINVAL\s+ok" "vfstest: umount fora de montagem"
    Expect $a "vfstest: TUDO OK" "vfstest -n: montagens, .. e erros de caminho"
    Expect $a "tmpfs\s+/tmp montado" "tmpfs montado em /tmp no boot"
    Expect $a "tmpfs\s+/run montado" "tmpfs montado em /run no boot"
    Expect $a "1000 arquivos criados e apagados sem vazar pagina\s+ok" "tmpfs: 1000 arquivos sem vazar pagina"
    Expect $a "fechar libera as paginas\s+ok" "tmpfs: arquivo apagado e aberto libera as paginas no close"
    Expect $a "e da ENOSPC\s+ok" "tmpfs: size= limita com ENOSPC"
    Expect $a "tmptest: TUDO OK" "tmptest: tmpfs completo"
    Expect $a "umount com a arvore funda +ok" "tmpfs: umount de arvore com 600 niveis"
    Expect $a "ftruncate de 256 GiB e esparso +ok" "tmpfs: arquivo esparso enorme sem gastar memoria"
    Expect $a "nr_inodes limita com ENOSPC +ok" "tmpfs: nr_inodes"
    Expect $a "rename no mesmo diretorio durante o readdir termina +ok" "tmpfs: rename durante o readdir"
    Expect $a "(?s)vfstest /tmp.*?vfstest: TUDO OK" "vfstest no tmpfs"
    Expect $a "(?s)linktest: links simbolicos em /tmp\s.*?linktest: TUDO OK" "linktest no tmpfs"
    Expect $a "readlink /tmp/lnx\s*\n\s*alvo-do-ln" "ln -s e readlink do sbase"
    Expect $a "(?s)attrtest: modo, dono e datas em /tmp\s.*?attrtest: TUDO OK" "attrtest no tmpfs: chmod, chown, utimensat, umask, nlink"
    Expect $a "(?s)attrtest: conferindo at/keep em /tmp\s.*?attrtest: TUDO OK" "attrtest no tmpfs: atributos guardados"
    Expect $a "-rw-r-----\s+1\s+12\s+34\s+0 \w{3} [ \d]\d \d\d:\d\d /tmp/lsx" "ls -l mostra modo, dono e grupo (chmod e chown do sbase)"
    Expect $a "lrwxrwxrwx\s+1\s+root\s+root\s+3 \w{3} [ \d]\d \d\d:\d\d /tmp/lsl -> lsx" "ls -l mostra o link, o alvo e o nome do dono pelo /etc/passwd"
    Expect $a "-rw-r--r--\s+1\s+root\s+root\s+\d+ \w{3} [ \d]\d( \d\d:\d\d|  \d{4}) /bin/ls" "ls -l no FAT com a data do diretorio"
    Expect $a "(?s)cat /proc/self/maps\s*\n\s*[0-9a-f]{8,}-[0-9a-f]{8,} r--p [0-9a-f]{8} 00:00 \d+\s+/bin/cat" "cat /proc/self/maps mostra as VMAs do proprio cat"
    Expect $a "[0-9a-f]+-7ffffffff000 rw-p 00000000 00:00 0\s+\[stack\]" "maps mostra a pilha"
    Expect $a "proc /proc proc ro 0 0" "/proc/mounts lista o procfs"
    Expect $a "tmpfs /tmp tmpfs rw 0 0" "/proc/mounts lista o tmpfs"
    Expect $a "mprotect divide a area no maps\s+ok" "proctest: maps acompanha o mprotect"
    Expect $a "filho morto aparece como zumbi\s+ok" "proctest: zumbi em /proc/<pid>/status"
    Expect $a "cmdline tem argv separado por NUL\s+ok" "proctest: cmdline"
    Expect $a "e le o caminho dele\s+ok" "proctest: /proc/self/fd"
    Expect $a "proctest: TUDO OK" "proctest: procfs completo"
    Expect $a "leituras do disco sda: (\d+) antes, \1 depois da segunda" "cache de paginas: a segunda leitura nao vai ao disco"
    Expect $a "o mmap mostra o mesmo conteudo do read\s+ok" "cache de paginas: mmap usa as mesmas paginas"
    Expect $a "escrever no mmap nao muda o arquivo\s+ok" "cache de paginas: MAP_PRIVATE copia na escrita"
    Expect $a "pctest: TUDO OK" "pctest: cache de paginas completo"
    Expect $a "sda" "ls /dev listou o disco"
    Expect $a "polltest: TUDO OK" "polltest TUDO OK"
    Expect $a "fbtest: gradiente \+ retangulo desenhados" "fbtest desenhou"
    Expect $a "tecla-desce" "evtest recebeu tecla"
    Expect $a "mouse-move" "evtest recebeu movimento do mouse"
    Expect $a "tecla-desce\s+scancode=1d" "Ctrl chega ao /dev/input"
    Expect $a "tecla-desce\s+scancode=48 ascii=0 mods=1\d" "seta chega ao /dev/input como estendida"
    Expect $a "\nola" "ptytest imprimiu ola"
    Expect $a "ptytest: sh saiu com 0" "ptytest encerrou o sh"
    Expect $a "\nalfa beta" "seta para cima recuperou o comando e o texto foi anexado"
    Expect $a "\nalfa Xbeta" "seta para a esquerda insere no meio da linha"
    Expect $a "\nalfa X\n" "Home, End e backspace cortaram o fim da linha"
    Expect $a "\nhomeok" "Home voltou ao inicio e inseriu ali"
    Expect $a "\ndelok" "seta para a direita e Delete apagaram no meio"
    Expect $a "(?s)\n   1  ls /bin \| wc.{0,900}?\n\s+\d+  echo delok" "history listou os comandos numerados"
    Expect $a "forktest: TUDO OK" "forktest TUDO OK"
    Expect $a "kernel escreve em pagina COW do filho\s+ok" "kernel escreve em pagina COW sem panic"
    Expect $a "8 filho\(s\) dormindo" "8 filhos vivos compartilhando memoria"
    Expect $a "COW: \d{2,} pagina\(s\) compartilhada\(s\)" "paginas compartilhadas enquanto os filhos vivem"
    Expect $a "filhos so de leitura terminaram ok" "filho so de leitura nao copiou nada"
    Expect $a "COW: 0 pagina\(s\) compartilhada\(s\), \d{3,} falta" "compartilhamento volta a zero no fim"
    Expect $a "e1000\s+8086:100e.*link up" "placa de rede detectada com link up"
    Expect $a "ip 10\.0\.2\.15  mascara 255\.255\.255\.0" "dhcp pegou endereco, mascara e rotas"
    Expect $a "eth0  52:54:00:12:34:56  link up" "ifconfig mostrou a interface"
    Expect $a "3 enviado\(s\), 3 recebido\(s\), 0% de perda" "ping ao gateway sem perda"
    Expect $a "servidor: 10\.0\.2\.3" "nslookup usou o servidor DNS que veio do DHCP"
    Expect $a "example\.com tem endereco \d+\.\d+\.\d+\.\d+" "nslookup resolveu um nome (precisa de DNS no hospedeiro)"
    Expect $a "10\.0\.2\.2\s+52:55:0a:00:02:02" "cache ARP tem o gateway"
    Expect $a "pilha: \d+ quadro\(s\), \d+ arp, \d+ ip, [1-9]\d* icmp" "pilha contabilizou icmp"
    Expect $a "eco-ola-mundo-fim" "variavel de ambiente definida e expandida"
    Expect $a "\n/bin/sh" "printenv leu uma variavel"
    Expect $a "HOME=/" "env listou o ambiente"
    Expect $a "Orion top   \d+ processo\(s\)   4 CPU\(s\)" "top mostrou o resumo do sistema"
    Expect $a "\n\s+2\s+1\s+2\s+\d+\s+rodando\s+sh" "top listou o sh"
    Expect $a "vazio\[\]" "unset apagou a variavel"
    Expect $a "\ne-ok" "&& roda o segundo comando quando o primeiro da certo"
    ExpectNot $a "\nNAO-DEVIA" "&& pula o segundo comando quando o primeiro falha"
    Expect $a "\nou-ok" "|| roda o segundo comando quando o primeiro falha"
    Expect $a "\num\r?\ndois" "; roda os dois comandos em sequencia"
    Expect $a "\nstatus=1" "`$? tem o status do ultimo comando"
    Expect $a "XV=abc printenv XV\s*\n\s*abc" "VAR=x cmd passa a variavel so para o comando"
    Expect $a "\nXV-sumiu" "VAR=x cmd nao muda o ambiente do shell"
    Expect $a "\[1\] \d+\s*\n" "cmd & devolve o numero do trabalho e o pid"
    Expect $a "\[1\] rodando\s+sleep 3" "jobs lista o trabalho em segundo plano"
    Expect $a "\[1\] pronto\s+sleep 3" "wait espera o trabalho terminar"
    Expect $a "\[3\] rodando\s+sleep 1" "dois trabalhos em segundo plano ao mesmo tempo"
    Expect $a "(?s)\$ fg\s*\nsleep 1" "fg traz o ultimo trabalho para o primeiro plano"
    Expect $a "\[2\] pronto\s+sleep 2" "trabalho terminado e avisado no prompt seguinte"
    Expect $a "limtest: fim, TUDO OK" "limtest: 256 fds, setrlimit, RLIMIT_NPROC no fork"
    Expect $a "-n arquivos abertos\s+256" "ulimit -a mostra 256 arquivos abertos"
    Expect $a "ps \| wc\s*\n\s+2[0-5]\d\s+" "ps lista mais de 200 processos vivos ao mesmo tempo"
    Expect $a "stress: pico de 220 vivos ao mesmo tempo" "stress manteve 220 filhos vivos"
    Expect $a "stress: 220 processos, 0 falha\(s\) ok" "stress com 220 vivos terminou sem falha"
    Expect $a "PID\s+PPID\s+PGID\s+PAGINAS\s+ESTADO\s+NOME" "ps -l mostrou o cabecalho"
    Expect $a "\n\s+\d+\s+\d+\s+\d+\s+\d+\s+rodando\s+ps" "ps -l listou a si mesmo"
    Expect $a "ligado ha \d\d:\d\d:\d\d, [1-9]\d* processo\(s\), 4 CPU\(s\)" "uptime contou processos e CPUs"
    Expect $a "memoria:\s+\d+\s+\d+\s+\d+" "free mostrou a memoria"
    Expect $a "real 1\.\d{3} s" "time mediu o sleep 1"
    Expect $a "15 TERM" "kill -l listou os sinais"
    Expect $a "kill: 9999: processo nao encontrado" "kill num pid inexistente"
    Expect $a "cat /dev/null \| wc\s*\n\s+0\s+0\s+0" "/dev/null le vazio"
    Expect $a "/dev/zero \| wc\s*\n\s+0\s+\d+\s+1000" "/dev/zero entregou 1000 bytes"
    Expect $a "/dev/random \| wc\s*\n\s+\d+\s+\d+\s+64" "/dev/random entregou 64 bytes"
    Expect $a "stress: 300 processos, 0 falha\(s\) ok" "stress com 300 processos"
    Expect $a ": OK" "heaptest OK"
    Expect $a "OK \(as duas rodaram\)" "preempt OK"
    Expect $a "0 pty\(s\) viva\(s\)" "nenhuma pty vazada"
    Test-MemDelta $a "memoria estavel na sequencia A" 24
}

# ---------------- Sequencia B: grafica ----------------
if ($runAll -or $OnlyB) {
    Write-Host "== Sequencia B (grafica) ==" -ForegroundColor Cyan
    $stepsB = @(
        "key:0B", "cmd:mem", "key:0B", "wait:1",
        "cmd:doom -iwad /doom/freedoom1.wad -nogui -orionquit 30", "wait:22", "shot:out\shots\reg-doom.ppm",
        "until:120:quadros em", "wait:1",
        "cmd:wm", "wait:3", "shot:out\shots\reg-wm-1-inicial.ppm",
        "mon:sendkey l", "mon:sendkey s", "mon:sendkey ret", "wait:1.5",
        "shot:out\shots\reg-wm-2-ls.ppm",
        "mon:mouse_move -140 -332", "wait:0.4", "mon:mouse_button 1", "wait:0.3",
        "mon:mouse_move 90 130", "mon:mouse_move 90 130", "wait:0.4", "mon:mouse_button 0", "wait:1",
        "shot:out\shots\reg-wm-3-arrastada.ppm",
        "mon:sendkey s", "mon:sendkey l", "mon:sendkey e", "mon:sendkey e", "mon:sendkey p",
        "mon:sendkey spc", "mon:sendkey 1", "mon:sendkey 0", "mon:sendkey 0", "mon:sendkey ret",
        "wait:2", "mon:sendkey ctrl-c", "wait:2", "shot:out\shots\reg-wm-3b-ctrlc.ppm",
        "mon:sendkey ctrl-t", "wait:2", "shot:out\shots\reg-wm-4-ctrl-t.ppm",
        "mon:sendkey ctrl-q", "wait:1.5", "shot:out\shots\reg-wm-5-ctrl-q.ppm",
        "mon:sendkey ctrl-alt-x", "wait:5",
        "cmd:echo TTY-DE-VOLTA", "wait:1", "shot:out\shots\reg-wm-6-tty.ppm",
        "key:0B", "cmd:mem", "cmd:pty", "cmd:procs"
    )
    & (Join-Path $Root "test.ps1") -Smp $Smp -NoBuild -LogPath "out\reg-b.log" `
        -Script $stepsB -CommandDelay 1.5 -BootTimeout 60 -NoPrint | Out-Null
    $b = Get-Log "out\reg-b.log"
    if ($Verbose) { Write-Output $b }

    Expect $b "adding /doom/freedoom1.wad" "doom: carregou o freedoom1.wad"
    Expect $b "doom: framebuffer \d+x\d+, escala \d" "doom: framebuffer e /dev/input"
    Expect $b "doom: \d{3,} quadros em \d+ ms" "doom rodou a demo (centenas de quadros)"
    Test-ShotColors "out\shots\reg-doom.ppm" "captura do doom tem a cena do jogo" 40
    Expect $b "wm: \d+x\d+, celula" "wm abriu o framebuffer"
    Expect $b "wm: terminal 0 -> sh pid \d+" "wm abriu o primeiro terminal"
    Expect $b "wm: terminal 3 -> sh pid \d+" "Ctrl\+T abriu um segundo terminal"
    Expect $b "morto por sinal 2" "Ctrl\+C no terminal grafico matou o sleep"
    Expect $b "wm: encerrando" "Ctrl\+Alt\+X encerrou o wm"
    Expect $b "TTY-DE-VOLTA\r?\nTTY-DE-VOLTA|echo TTY-DE-VOLTA[\s\S]{0,40}TTY-DE-VOLTA" "TTY respondeu depois do wm"
    Expect $b "0 pty\(s\) viva\(s\)" "nenhuma pty vazada"
    Expect $b "(?s)pid\s+ppid\s+estado[^\n]*\n\s+2\s+1\s+running\s+\d+\s+sh\s*\n\s+1\s+0\s+running\s+\d+\s+init" "procs so tem init e sh"
    Test-MemDelta $b "memoria estavel na sequencia B" 16
}

# ---------------- Sequencia C: disco e shell ----------------
if ($runAll -or $OnlyC) {
    Write-Host "== Sequencia C (disco cru, escrita e shell) ==" -ForegroundColor Cyan
    Remove-Item (Join-Path $Root "out\disk.img") -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $Root "out\ext2-*.img") -ErrorAction SilentlyContinue
    $py = & (Join-Path $Root "tools\python.ps1")
    $pyArgs = @($py | Select-Object -Skip 1)
    & $py[0] @pyArgs (Join-Path $Root "tools\mkext2.py") out/ext2-4k.img --test --size 32 --block 4096 --add /bin/hello=out/hello.elf
    if ($LASTEXITCODE -ne 0) { throw "mkext2 falhou" }
    & $py[0] @pyArgs (Join-Path $Root "tools\mkext2.py") out/ext2-1k.img --test --size 32 --block 1024 --add /bin/hello=out/hello.elf
    if ($LASTEXITCODE -ne 0) { throw "mkext2 falhou" }
    $stepsC = @(
        "cmd:mkfs /dev/sdb ORION", "wait:10",
        "key:0B", "cmd:mount", "cmd:mem", "key:0B", "wait:1",
        "cmd:fstest -a /mnt", "wait:220",
        "cmd:echo abc > /mnt/a.txt", "cmd:echo def >> /mnt/a.txt", "wait:1",
        "cmd:cat /mnt/a.txt | sort -r | tee /mnt/b.txt | wc", "wait:2",
        "cmd:cat /mnt/b.txt", "cmd:wc < /mnt/a.txt",
        "cmd:cat /nao-existe 2> /mnt/e.txt", "cmd:cat /mnt/e.txt",
        "cmd:mkdir /mnt/d", "cmd:cp /bin/hello /mnt/d/h", "cmd:/mnt/d/h", "wait:1",
        "cmd:rm -r /mnt/d", "cmd:rm /mnt/a.txt /mnt/b.txt /mnt/e.txt",
        "cmd:edit /mnt/nota.txt", "wait:3",
        "raw:linha um", "key:0D", "raw:linha dois", "wait:1",
        "key:13", "wait:3", "key:18", "wait:4",
        "cmd:cat /mnt/nota.txt", "wait:2", "cmd:rm /mnt/nota.txt",
        "cmd:mkdir /mnt/p1", "cmd:mkdir /mnt/p2",
        "cmd:fstest -a /mnt/p1 > /mnt/f1.txt | fstest -a /mnt/p2", 'cmd:echo FIM$X-PAR', "until:600:FIM-PAR",
        "cmd:grep TUDO /mnt/f1.txt", "wait:1",
        "cmd:sync", "cmd:ls /mnt", "wait:1",
        "cmd:vfstest /mnt", "until:120:falha(s))",
        "cmd:cp /bin/tcc /mnt/pc.bin", "wait:3", "cmd:pctest -d sdb -w /mnt /mnt/pc.bin", "until:120:falha(s))",
        "cmd:rm /mnt/pc.bin",
        "cmd:cd /mnt", "cmd:umount /mnt", "cmd:cd /", "cmd:umount /mnt", "cmd:ls /mnt", "wait:1",
        "cmd:mount -r /dev/sdb /mnt", "cmd:cat /mnt/f1.txt | grep TUDO | wc", "cmd:echo x > /mnt/ro.txt",
        "cmd:umount /mnt", "cmd:mount /dev/sdb /mnt",
        "cmd:echo rw-ok > /mnt/rw.txt", "cmd:cat /mnt/rw.txt", "cmd:rm /mnt/rw.txt",
        "cmd:touch /mnt/t1", "cmd:chmod 444 /mnt/t1", "cmd:ls -l /mnt/t1", "cmd:chmod 600 /mnt/t1",
        "cmd:chmod 644 /mnt/t1", "cmd:rm /mnt/t1", "wait:1",
        "cmd:mkdir /mnt/q", "cmd:mount /dev/sdb /mnt/q", "cmd:rm -r /mnt/q", "wait:1",
        "cmd:mkdir /tmp/e4", "cmd:mount -t ext2 /dev/sdc /tmp/e4", "cmd:ext2test /tmp/e4", "until:300:falha(s))",
        "cmd:/tmp/e4/bin/hello ext2", "wait:2",
        "cmd:mkfs.ext2 /dev/sdc", "cmd:fsck.ext2 /dev/sdc", "wait:1",
        "cmd:mkdir /tmp/e1", "cmd:mount -t ext2 /dev/sdd /tmp/e1", "cmd:ext2test /tmp/e1", "until:300:falha(s))",
        "cmd:umount /tmp/e4", "cmd:umount /tmp/e1", "wait:1",
        "cmd:fsck.ext2 /dev/sdc", 'cmd:echo FIM$X-FSCK1', "until:120:FIM-FSCK1",
        "cmd:fsck.ext2 /dev/sdd", 'cmd:echo FIM$X-FSCK2', "until:120:FIM-FSCK2",
        "cmd:mkfs.ext2 -L NOVO /dev/sdd", 'cmd:echo FIM$X-MKFS', "until:120:FIM-MKFS",
        "cmd:fsck.ext2 /dev/sdd", 'cmd:echo FIM$X-FSCK3', "until:120:FIM-FSCK3",
        "cmd:mount -t ext2 /dev/sdd /tmp/e1", "cmd:ls /tmp/e1",
        "cmd:fstest -a /tmp/e1", 'cmd:echo FIM$X-FSTEST', "until:600:FIM-FSTEST",
        "cmd:linktest /tmp/e1", 'cmd:echo FIM$X-LINK', "until:200:FIM-LINK",
        "cmd:attrtest /tmp/e1", 'cmd:echo FIM$X-ATTR', "until:200:FIM-ATTR",
        "cmd:umount /tmp/e1", "cmd:mount -t ext2 /dev/sdd /tmp/e1",
        "cmd:attrtest -k /tmp/e1", 'cmd:echo FIM$X-KEEP', "until:60:FIM-KEEP",
        "cmd:umount /tmp/e1", "wait:1",
        "cmd:fsck.ext2 /dev/sdd", 'cmd:echo FIM$X-FSCK4', "until:120:FIM-FSCK4",
        "key:0B", "cmd:mem", "cmd:lsblk", "cmd:mutex"
    )
    & (Join-Path $Root "test.ps1") -Smp $Smp -NoBuild -Disk -ExtraDisks @("out/ext2-4k.img", "out/ext2-1k.img") -LogPath "out\reg-c.log" `
        -Script $stepsC -CommandDelay 1.5 -BootTimeout 60 -NoPrint | Out-Null
    $c = Get-Log "out\reg-c.log"
    if ($Verbose) { Write-Output $c }

    Expect $c "mkfs: /dev/sdb" "mkfs formatou o disco cru"
    Expect $c "/mnt montado: FAT16 em sdb" "segundo disco montado em /mnt"
    Expect $c "fstest: TUDO OK" "fstest TUDO OK"
    Expect $c "criou 300 arquivos com nome longo\s+ok" "300 arquivos com nome longo"
    Expect $c "readdir devolve todos de volta\s+ok" "readdir devolve os 300"
    Expect $c "escreveu 3 MiB\s+ok" "arquivo de 3 MiB"
    Expect $c "tamanho de 3 MiB\s+ok" "wc do arquivo de 3 MiB"
    Expect $c "(?s)tee /mnt/b\.txt \| wc.{0,60}?2\s+2\s+8" "pipeline de 4 estagios"
    Expect $c "(?s)cat /mnt/b\.txt.{0,20}?def\s+abc" "redirecionamento > e >> com tee"
    Expect $c "cat: /nao-existe: Arquivo ou diretorio inexistente" "redirecionamento 2>"
    Expect $c "hello from ring 3" "binario copiado para /mnt roda"
    Expect $c "/mnt/nota\.txt salvo \(2 linha\(s\)\)" "edit salvou com Ctrl\+S"
    Expect $c "edit: pronto" "edit saiu com Ctrl\+X"
    Expect $c "(?s)cat /mnt/nota\.txt.{0,40}?linha um\s+linha dois" "cat mostra o que o edit gravou"
    Expect $c "mutex\s+pegou\s+contencao\s+espera ms\s+max ms\s+dono" "comando mutex mostra a tabela de contencao"
    Expect $c "\n\s+ahci0\s+[1-9]\d*\s+\d+\s+\d+\s+\d+\s+-" "AHCI usa mutex (pego e livre no fim)"
    Expect $c "grep TUDO /mnt/f1\.txt\s*\n\s*fstest: TUDO OK" "fstest em paralelo: a primeira instancia passou"
    Expect $c "(?s)fstest -a /mnt/p2.*?fstest: TUDO OK.*?\nFIM-PAR" "fstest em paralelo: a segunda instancia passou"
    Expect $c "\n\s+(fat-sdb|bcache)\s+\d+\s+[1-9]\d*\s+" "fstest em paralelo disputou FAT/cache e esperou dormindo"
    Expect $c "devfs montado em /mnt/vt-m \(devfs\)" "vfstest montou devfs num diretorio do disco"
    Expect $c "devfs desmontado de /mnt/vt-m" "vfstest desmontou o devfs"
    Expect $c "o buraco le zeros\s+ok" "vfstest: escrita depois do fim deixa zeros"
    Expect $c "o \.\. do diretorio movido aponta o pai novo\s+ok" "vfstest: rename de diretorio entre pais"
    Expect $c "leitura continua pelo descritor\s+ok" "vfstest: arquivo apagado continua legivel aberto"
    Expect $c "fstat do outro descritor ve o tamanho novo\s+ok" "vfstest: dois descritores veem o mesmo vnode"
    Expect $c "vfstest: TUDO OK" "vfstest no disco: VFS completa"
    Expect $c "rename por cima com a raiz cheia +ok" "vfstest: rename por cima na raiz FAT cheia"
    Expect $c "\.\. a partir de cwd apagado da ENOENT\s+ok" "vfstest: .. de diretorio apagado nao ressuscita o pai"
    Expect $c "getcwd acompanha o rename do cwd\s+ok" "vfstest: getcwd depois de renomear o cwd"
    Expect $c "rename que so muda maiusculas\s+ok" "vfstest: rename so de maiusculas no FAT"
    Expect $c "O_APPEND em dois processos nao perde escrita\s+ok" "vfstest: O_APPEND atomico"
    Expect $c "apagar durante o readdir nao pula entradas\s+ok" "vfstest: getdents64 com posicao estavel"
    Expect $c "leituras do disco sdb: (\d+) antes, \1 depois da segunda" "cache de paginas no disco cru"
    Expect $c "o mmap ja feito ve o pwrite\s+ok" "cache de paginas: escrita aparece no mmap ja feito"
    Expect $c "ftruncate de volta para 9000 le zeros\s+ok" "cache de paginas: truncate coerente"
    Expect $c "pctest: TUDO OK" "pctest no disco cru"
    Expect $c "umount: /mnt: Recurso ocupado" "umount com o cwd dentro da EBUSY"
    Expect $c "/dev/sdb desmontado de /mnt" "umount desmontou o segundo disco"
    Expect $c "/dev/sdb montado em /mnt \(vfat, somente leitura\)" "mount -r monta so leitura"
    Expect $c "grep TUDO \| wc\s*\n\s+1\s+3\s+16" "arquivo legivel depois de remontar"
    Expect $c "/mnt/ro\.txt: Sistema de arquivos somente leitura" "escrita em montagem so de leitura da EROFS"
    Expect $c "(?s)cat /mnt/rw\.txt\s*\n\s*rw-ok" "remontado com escrita"
    Expect $c "mount: /dev/sdb em /mnt/q: Recurso ocupado" "o mesmo disco nao monta duas vezes"
    Expect $c "ext2\s+sdc: rev 1, blocos de 4096 bytes" "ext2 de blocos de 4 KiB montado (leitor em Rust)"
    Expect $c "ext2\s+sdd: rev 1, blocos de 1024 bytes, \d+ blocos \(\d+ livres\), \d+ inodes, 4 grupo\(s\)" "ext2 de blocos de 1 KiB com 4 grupos"
    Expect $c "(?s)ext2test /tmp/e4.*?todos os arquivos batem com o MANIFEST\s+ok.*?ext2test: TUDO OK" "ext2test no ext2 de 4 KiB"
    Expect $c "(?s)ext2test /tmp/e1.*?fim de huge-sparse\.bin\s+ok.*?ext2test: TUDO OK" "ext2test no ext2 de 1 KiB (indireto triplo)"
    Expect $c "link longlink\s+ok" "ext2: link simbolico lento lido pelo readlink"
    Expect $c "argv\[1\] = ext2" "binario do ext2 roda"
    Expect $c "/dev/sdc desmontado de /tmp/e4" "umount do ext2"
    Expect $c "mkfs\.ext2: /dev/sdc: esta montado" "mkfs.ext2 recusa disco montado"
    Expect $c "fsck\.ext2: /dev/sdc: esta montado" "fsck.ext2 recusa disco montado"
    Expect $c "(?s)fsck\.ext2: /dev/sdc: limpo.*?FIM-FSCK1" "ext2 de 4 KiB limpo no fsck depois do ext2test"
    Expect $c "(?s)fsck\.ext2: /dev/sdd: limpo.*?FIM-FSCK2" "ext2 de 1 KiB limpo no fsck depois do ext2test"
    Expect $c "mkfs\.ext2: /dev/sdd pronto: \d+ blocos livres, \d+ inodes livres, rotulo 'NOVO'" "mkfs.ext2 formatou o sdd"
    Expect $c "(?s)FIM-MKFS.*?fsck\.ext2: /dev/sdd: limpo.*?FIM-FSCK3" "ext2 recem-formatado passa no fsck"
    Expect $c "ext2\s+sdd: rev 1, blocos de 1024 bytes, \d+ blocos \(\d+ livres\), \d+ inodes, \d+ grupo\(s\), label 'NOVO', leitura e escrita" "kernel monta o ext2 do mkfs.ext2"
    Expect $c "ls /tmp/e1\s*\n\s*<DIR>\s+lost\+found/" "ext2 novo tem o lost+found"
    Expect $c "(?s)fstest -a /tmp/e1.*?criou 300 arquivos com nome longo\s+ok.*?escreveu 3 MiB\s+ok.*?fstest: TUDO OK.*?FIM-FSTEST" "fstest -a no ext2"
    Expect $c "(?s)linktest: links simbolicos em /tmp/e1.*?linktest: TUDO OK.*?FIM-LINK" "linktest no ext2"
    Expect $c "(?s)attrtest: modo, dono e datas em /tmp/e1.*?attrtest: TUDO OK.*?FIM-ATTR" "attrtest no ext2"
    Expect $c "(?s)attrtest: conferindo at/keep em /tmp/e1.*?modo sobreviveu\s+ok.*?dono e grupo sobreviveram\s+ok.*?datas sobreviveram\s+ok.*?attrtest: TUDO OK.*?FIM-KEEP" "ext2: modo, dono e datas sobrevivem ao umount e mount"
    Expect $c "(?s)FIM-KEEP.*?fsck\.ext2: /dev/sdd: limpo.*?FIM-FSCK4" "ext2 limpo no fsck depois do fstest -a, do linktest e do attrtest"
    Expect $c "-r--r--r--\s+1\s+root\s+root\s+0 \w{3} [ \d]\d \d\d:\d\d /mnt/t1" "FAT: chmod 444 liga o somente leitura"
    Expect $c "chmod /mnt/t1: Operation not permitted" "FAT: chmod que o FAT nao guarda da EPERM"
    Test-MemDelta $c "memoria estavel na sequencia C" 24
}

# ---------------- Sequencia D: boot direto na interface grafica ----------------
if ($runAll -or $OnlyD) {
    Write-Host "== Sequencia D (boot direto na GUI) ==" -ForegroundColor Cyan
    $stepsD = @(
        "wait:16", "shot:out\shots\reg-gui-1-boot.ppm",
        "mon:sendkey ctrl-p", "wait:2", "shot:out\shots\reg-gui-2-processos.ppm",
        "mon:sendkey ctrl-f", "wait:2", "shot:out\shots\reg-gui-3-arquivos.ppm",
        "mon:mouse_move -640 -400", "wait:0.5", "mon:mouse_move 40 12", "wait:0.5",
        "mon:mouse_button 1", "wait:0.4", "mon:mouse_button 0", "wait:1",
        "shot:out\shots\reg-gui-4-menu.ppm",
        "mon:sendkey ctrl-alt-x", "wait:6"
    )
    & (Join-Path $Root "test.ps1") -Smp $Smp -Gui -LogPath "out\reg-d.log" `
        -Script $stepsD -CommandDelay 1.5 -BootTimeout 60 -NoPrint | Out-Null
    $d = Get-Log "out\reg-d.log"
    if ($Verbose) { Write-Output $d }
    & (Join-Path $Root "build.ps1") | Out-Null

    Expect $d "init: autostart: /bin/dhcp" "init executou o autostart"
    Expect $d "ip 10\.0\.2\.15" "dhcp do autostart configurou a rede"
    Expect $d "init: iniciando /bin/wm" "init subiu direto na interface grafica"
    Expect $d "wm: \d+x\d+, celula" "wm abriu o framebuffer sem passar pelo sh"
    Expect $d "wm: terminal 0 -> sh pid \d+" "wm abriu um terminal com o sh dentro"
    Expect $d "wm: janela de processos aberta" "Ctrl\+P abriu a janela de processos"
    Expect $d "wm: janela de arquivos em /" "Ctrl\+F abriu a janela de arquivos"
    Expect $d "wm: encerrando" "Ctrl\+Alt\+X encerrou o wm"
    Expect $d "init: /bin/wm terminou" "init percebeu a saida do wm"
}

# ---------------- Sequencia E: estresse de processos ----------------
if ($runAll -or $OnlyE) {
    Write-Host "== Sequencia E (estresse) ==" -ForegroundColor Cyan
    $stressCmd = "stress 3000 16 | stress 3000 16 | stress 3000 16 | stress 3000 16"
    for ($i = 1; $i -le $StressRuns; $i++) {
        $stepsE = @("cmd:$stressCmd", 'cmd:echo FIM$X-ESTRESSE', "until:1200:FIM-ESTRESSE",
                    "key:0B", "cmd:procs", "cmd:ps", "wait:1")
        & (Join-Path $Root "test.ps1") -Smp $Smp -NoBuild -LogPath "out\reg-e-$i.log" `
            -Script $stepsE -CommandDelay 1.0 -BootTimeout 60 -NoPrint | Out-Null
        $e = Get-Log "out\reg-e-$i.log"
        if ($Verbose) { Write-Output $e }
        Expect $e "\nFIM-ESTRESSE" "estresse $i`: o pipeline terminou"
        Expect $e "stress: \d+ processos, 0 falha\(s\) ok" "estresse $i`: 0 falhas"
        ExpectNot $e "PANIC|EXCECAO|deadlock|watchdog|stress \d+: |fork devolveu 0" "estresse $i`: sem panico, deadlock ou erro do stress"
    }
}

Write-Host ""
if ($failures.Count -eq 0) {
    Write-Host "REGRESSAO VERDE ($checks verificacoes)" -ForegroundColor Green
    exit 0
}
Write-Host ("REGRESSAO VERMELHA: " + $failures.Count + " de " + $checks + " falharam") -ForegroundColor Red
foreach ($f in $failures) { Write-Host ("  - " + $f) -ForegroundColor Red }
exit 1
