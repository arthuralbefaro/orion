param(
    [int]$Smp = 4,
    [switch]$NoBuild,
    [switch]$OnlyA,
    [switch]$OnlyB,
    [switch]$OnlyC,
    [switch]$OnlyD,
    [switch]$OnlyE,
    [switch]$OnlyF,
    [switch]$OnlyV,
    [switch]$OnlyN,
    [switch]$OnlyU,
    [int]$StressRuns = 1,
    [switch]$Verbose
)

$ErrorActionPreference = "Stop"
$Root = $PSScriptRoot
$failures = @()
$checks = 0
$runAll = -not ($OnlyA -or $OnlyB -or $OnlyC -or $OnlyD -or $OnlyE -or $OnlyF -or $OnlyV -or $OnlyN -or $OnlyU)

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

function Start-HostWeb([int]$port) {
    $py = & (Join-Path $Root "tools\python.ps1")
    $pyArgs = @($py | Select-Object -Skip 1) + @((Join-Path $Root "tools\hostweb.py"), "--port", "$port")
    $p = Start-Process -FilePath $py[0] -ArgumentList $pyArgs -PassThru -WindowStyle Hidden `
        -RedirectStandardError (Join-Path $Root "out\hostweb.log")
    Start-Sleep -Seconds 2
    return $p
}

function Test-ShotPixel([string]$ppm, [int]$x, [int]$y, [string]$hex, [int]$tol, [string]$what) {
    $script:checks++
    $path = Join-Path $Root $ppm
    if (-not (Test-Path $path)) {
        Write-Host ("  [FALHA] " + $what + "   (sem a captura " + $ppm + ")") -ForegroundColor Red
        $script:failures += $what
        return
    }
    $bytes = [System.IO.File]::ReadAllBytes($path)
    $fields = @()
    $i = 0
    while ($fields.Count -lt 4 -and $i -lt $bytes.Length) {
        $tok = ""
        while ($i -lt $bytes.Length -and [char]$bytes[$i] -notmatch '\s') { $tok += [char]$bytes[$i]; $i++ }
        if ($tok) { $fields += $tok }
        $i++
    }
    $w = [int]$fields[1]
    $p = $i + ($y * $w + $x) * 3
    $want = [Convert]::ToInt32($hex.Substring(2), 16)
    $got = @([int]$bytes[$p], [int]$bytes[$p + 1], [int]$bytes[$p + 2])
    $exp = @((($want -shr 16) -band 255), (($want -shr 8) -band 255), ($want -band 255))
    $ok = $true
    for ($c = 0; $c -lt 3; $c++) { if ([Math]::Abs($got[$c] - $exp[$c]) -gt $tol) { $ok = $false } }
    $seen = "{0:x2}{1:x2}{2:x2}" -f $got[0], $got[1], $got[2]
    if ($ok) {
        Write-Host ("  [ok]    " + $what + " (pixel $x,$y = $seen)") -ForegroundColor Green
    } else {
        Write-Host ("  [FALHA] " + $what + " (pixel $x,$y = $seen, esperado $hex +-$tol)") -ForegroundColor Red
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
    & (Join-Path $Root "build.ps1") -Image | Out-Null
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
        "cmd:ls -l /boot/efi/boot", "wait:1",
        "cmd:cat /proc/self/maps", "wait:1", "cmd:cat /proc/mounts", "wait:1", "cmd:cat /proc/stat", "wait:1",
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
        "cmd:history", "wait:3", "cmd:cat /proc/interrupts", "wait:1",
        "cmd:forktest", "wait:30",
        "cmd:forktest -s 6", "wait:3", "key:0B", "cmd:mem", "key:0B", "wait:12",
        "cmd:dhcp", "wait:8",
        "cmd:ifconfig", "wait:2",
        "cmd:ping 10.0.2.2 3", "wait:10",
        "tcpprobe:5555",
        "cmd:httpget 10.0.2.2 8088 /index.html", "until:30:crc32",
        "cmd:httpget -o /tmp/big.bin 10.0.2.2 8088 /big.bin", "until:90:crc32",
        "cmd:wc /tmp/big.bin", "cmd:rm /tmp/big.bin", "wait:1",
        "cmd:httpget example.com 80 /", "until:30:httpget:",
        "cmd:httpget -q 10.0.2.2 8088 /big10.bin", "until:120:corpo de",
        "cmd:head -c 1000000 /dev/random > /tmp/up.bin", "wait:3",
        "key:0B", "cmd:tcploss 10", "key:0B",
        "cmd:httpget -u /tmp/up.bin 10.0.2.2 8088 /up", "until:240:corpo de",
        "cmd:httpget -q 10.0.2.2 8088 /big.bin", "until:240:corpo de",
        "key:0B", "cmd:tcploss 0", "cmd:net", "key:0B", "cmd:rm /tmp/up.bin", "wait:1", "cmd:cat /proc/interrupts", "wait:1",
        "cmd:httpd -n 3 -r / &", "wait:2", "hostget:8081:/etc/passwd", "hostget:8081:/nao-existe", "hostget:8081:/etc",
        "wait:2", "cmd:httpd -n 1 &", "wait:2", "hostget:8081:/etc/group", "wait:2",
        "cmd:unixtest /run", "until:60:falha(s))", "cmd:unixtest /", "until:60:falha(s))",
        "cmd:permtest /tmp", "until:90:falha(s))", "cmd:permtest /", "until:90:falha(s))",
        "cmd:ls -l /etc/shadow", "cmd:id", "wait:1", "cmd:su orion -c poweroff", "wait:1",
        "cmd:imginfo /usr/share/imagens/teste.jpg /usr/share/imagens/teste.png /usr/share/wallpapers/orion.png", "wait:2",
        "cmd:shmtest", "until:30:falha(s))", "cmd:aslrtest", "until:30:falha(s))", "cmd:cat /proc/self/maps", "cmd:cat /proc/self/maps", "wait:1",
        "cmd:login", "until:10:login: ", "raw:orion", "key:0D", "until:10:Senha: ", "raw:errada", "key:0D",
        "until:10:Login incorreto", "wait:1", "raw:orion", "key:0D", "wait:1", "raw:orion", "key:0D",
        "until:10:Bem-vindo, orion", "wait:1", "cmd:id", "cmd:pwd", "cmd:cat /etc/shadow", "wait:1",
        "cmd:su -c id", "until:10:Senha: ", "raw:xx", "key:0D", "until:10:senha incorreta",
        "cmd:su -c id", "until:10:Senha: ", "raw:orion", "key:0D", "until:10:uid=0(root)",
        "cmd:passwd", "until:10:Senha atual: ", "raw:orion", "key:0D", "until:10:Senha nova: ", "raw:nova123", "key:0D",
        "until:10:Repita", "raw:nova123", "key:0D", "until:15:trocada",
        "cmd:su orion -c id", "until:10:Senha: ", "raw:orion", "key:0D", "until:10:senha incorreta",
        "cmd:su orion -c id", "until:10:Senha: ", "raw:nova123", "key:0D", "until:10:uid=1000(orion)",
        "cmd:exit", "wait:1", "cmd:grep orion: /etc/shadow", "cmd:ls -l /etc/shadow", "cmd:ls -l /bin/su /bin/passwd",
        "cmd:ls -l /home", "cmd:passwd orion", "until:10:Senha nova: ", "raw:orion", "key:0D", "until:10:Repita",
        "raw:orion", "key:0D", "until:15:trocada", "cmd:su orion -c id", "until:10:uid=1000(orion)", "wait:1",
        "cmd:ping 127.0.0.1 2", "wait:4",
        "cmd:httpd -p 8080 -n 1 &", "wait:2", "cmd:httpget 127.0.0.1 8080 /etc/group", "until:30:crc32", "wait:2",
        "cmd:cd /tmp", "cmd:wget http://example.com", "until:30:salvo", "cmd:grep Example index.html", "wait:1",
        "cmd:rm index.html", "cmd:wget -O /tmp/w.bin http://10.0.2.2:8088/big.bin", "until:30:salvo",
        "cmd:sha256sum /tmp/w.bin", "wait:3", "cmd:rm /tmp/w.bin", "cmd:wget http://10.0.2.2:8088/nada", "wait:3", "cmd:cd /",
        "cmd:nc -l 7777 < /dev/null > /tmp/nc.out &", "wait:1", "cmd:echo transferido-pelo-nc | nc -N 127.0.0.1 7777", "wait:2",
        "cmd:cat /tmp/nc.out", "cmd:nc -z 127.0.0.1 7778", "wait:2",
        "cmd:echo GET /index.html HTTP/1.0 | nc -N 10.0.2.2 8088", "wait:3",
        "cmd:printf \377\373\001ola\n | nc -l 7779 > /tmp/tn.out &", "wait:1", "cmd:echo oi | telnet 127.0.0.1 7779", "wait:3",
        "cmd:od -b /tmp/tn.out", "wait:2", "cmd:rm /tmp/nc.out /tmp/tn.out",
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
        "cmd:dev", "cmd:pty", "cmd:mutex", "cmd:mem", "key:0B",
        "cmd:echo sobrevive-ao-reboot > /reboot.txt", "cmd:sync", "wait:3", "reboot:",
        "cmd:cat /reboot.txt", "cmd:rm /reboot.txt", "cmd:mount", "wait:1"
    )
    $web = Start-HostWeb 8088
    & (Join-Path $Root "test.ps1") -Smp $Smp -Image @buildFlag -HostFwd @("tcp:127.0.0.1:5555-:7", "tcp:127.0.0.1:8081-:80") -LogPath "out\reg-a.log" `
        -Script $stepsA -CommandDelay 1.5 -BootTimeout 60 -NoPrint | Out-Null
    Stop-Process -Id $web.Id -Force -ErrorAction SilentlyContinue
    $a = Get-Log "out\reg-a.log"
    if ($Verbose) { Write-Output $a }

    Expect $a "Orion sh \(pid \d+\)" "shell de usuario subiu"
    Expect $a "\[info\] cpu\s+SMEP ligado, SMAP ligado" "SMEP e SMAP ligados em todas as copias de memoria de usuario"
    Expect $a "\[info\] acpi\s+uACPI pronto em \d+ ms: namespace carregado, botao de energia ligado, _PRT com \d+ rota" "uACPI carrega o namespace, o botao de energia e o _PRT"
    Expect $a "e1000\s+8086:10d3[^\n]*MSI no vetor 129, CPU 2, link up" "e1000e (82574L) liga a MSI e manda a IRQ para a CPU 2"
    Expect $a "(?s)cat /proc/interrupts.*?\n\s*129:\s+0\s+0\s+[1-9]\d*\s+0\s+e1000" "/proc/interrupts: rede por MSI so na CPU 2, depois do trafego"
    Expect $a "su orion -c poweroff\s*poweroff: gravando os discos e desligando\s*poweroff: Operation not permitted" "usuario comum nao desliga a maquina (EPERM do kernel)"
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
    Expect $a "-rwxr-xr-x\s+1\s+root\s+root\s+\d+ \w{3} [ \d]\d( \d\d:\d\d|  \d{4}) /bin/ls" "ls -l na raiz ext2: binario com modo 0755"
    Expect $a "-rwxr-xr-x\s+1\s+root\s+root\s+\d+ \w{3} [ \d]\d( \d\d:\d\d|  \d{4}) orion\.elf" "ls -l no FAT da particao EFI com a data da entrada"
    Expect $a "sda1: GPT, LBA 2048, \d+ setores \(64 MiB\), ESP" "GPT: particao EFI reconhecida"
    Expect $a "sda2: GPT, LBA \d+, \d+ setores \(\d+ MiB\), raiz Linux x86-64" "GPT: particao de raiz reconhecida"
    Expect $a "raiz montada: ext2 em sda2" "raiz em ext2 na particao GPT"
    Expect $a "particao EFI sda1 montada em /boot/efi" "particao EFI montada em /boot/efi"
    Expect $a "/dev/sda2 / ext2 rw 0 0" "raiz ext2 montada com escrita"
    Expect $a "\[tcpprobe\] porta 5555: fechada pelo Orion" "TCP: o hospedeiro ve a porta 7 fechada (RST)"
    Expect $a "tcp: [1-9]\d* segmento\(s\), 0 com erro, \d+ descartado\(s\), [1-9]\d* RST enviado\(s\)" "TCP: segmento recebido sem erro de checksum e RST enviado"
    Expect $a "ola do hospedeiro</body></html>\s*\nhttpget: HTTP/1\.0 200 OK" "TCP: httpget baixou a pagina do servidor do hospedeiro"
    Expect $a "httpget: corpo de 1048576 bytes, crc32 a2dcf263" "TCP: 1 MiB do hospedeiro sem corromper (crc32)"
    Expect $a "wc /tmp/big\.bin\s*\n[^\n]*\s1048576 /tmp/big\.bin" "TCP: arquivo baixado gravado inteiro"
    Expect $a "(?s)httpget example\.com 80 /.*?Example Domain.*?httpget: HTTP/1\.[01] 200" "TCP: httpget baixou example.com (depende da internet do hospedeiro)"
    Expect $a "RST enviado\(s\), 0 conexao\(oes\)" "TCP: conexoes encerradas saem da tabela"
    Expect $a "httpget: corpo de 10485760 bytes, crc32 1a960f7c" "TCP: 10 MiB do hospedeiro sem corromper (crc32)"
    Expect $a "(?s)tcploss: descartando 1 em 10 .*?httpget: enviado (\d+) bytes, crc32 ([0-9a-f]{8})\s*\nrecebido \1 bytes, crc32 \2" "TCP: envio de 1 MB com 10% de perda chega inteiro (crc32 no hospedeiro)"
    Expect $a "(?s)tcploss: descartando 1 em 10 .*?corpo de 1048576 bytes, crc32 a2dcf263.*?tcploss: desligado" "TCP: 1 MiB recebido com 10% de perda sem corromper"
    Expect $a "tcp: [1-9]\d* retransmissao\(oes\)" "TCP: retransmissoes contadas no comando net"
    Expect $a "\[hostget\] porta 8081 /etc/passwd: HTTP/1\.0 200 OK, corpo de \d+ bytes\s*\n\[hostget\] root:x:0:0:" "TCP servidor: httpd do Orion responde ao hospedeiro"
    Expect $a "\[hostget\] porta 8081 /nao-existe: HTTP/1\.0 404 Not Found" "httpd: 404 para o que nao existe"
    Expect $a "\[hostget\] porta 8081 /etc: HTTP/1\.0 200 OK[^\n]*\n(\[hostget\] [^\n]*\n)*?\[hostget\] resolv\.conf" "httpd: lista o diretorio"
    Expect $a "httpd: 10\.0\.2\.2:\d+ GET /etc/passwd -> 200" "httpd: accept4 e getpeername com o endereco do cliente"
    Expect $a "httpd: fim, 3 conexao\(oes\)" "httpd: select com prazo, contagem e saida limpa"
    Expect $a "(?s)httpd: fim, 3 conexao.*?\[hostget\] porta 8081 /etc/group: HTTP/1\.0 200 OK" "SO_REUSEADDR: segundo httpd na mesma porta com conexoes em TIME_WAIT"
    Expect $a "(?s)unixtest: sockets AF_UNIX em /run\s.*?unixtest: TUDO OK" "AF_UNIX no tmpfs: socketpair, bind, listen, connect, accept, poll, nomes"
    Expect $a "(?s)unixtest: sockets AF_UNIX em /\s*\n.*?unixtest: TUDO OK" "AF_UNIX com o no do socket na raiz ext2"
    Expect $a "(?s)permtest: permissoes em /tmp\s.*?permtest: TUDO OK" "permtest no tmpfs: uid/gid, permissoes, sticky, setuid"
    Expect $a "(?s)permtest: permissoes em /\s*\n.*?permtest: TUDO OK" "permtest na raiz ext2"
    Expect $a "usuario comum nao le o /etc/shadow\s+ok" "usuario comum nao le o /etc/shadow"
    Expect $a "binario setuid de root roda com euid 0\s+ok" "binario setuid troca o euid no exec"
    Expect $a "-rw-------\s+1\s+root\s+root\s+\d+ [^\n]*/etc/shadow" "/etc/shadow e 0600 de root na imagem"
    Expect $a "\nuid=0\(root\) gid=0\(root\)" "id mostra root pelo /etc/passwd"
    Expect $a "(?s)aslrtest: pilha.*?aslrtest: TUDO OK \(0 falha\(s\)\)" "aslrtest: pilha e mmap mudam a cada exec, dentro das faixas"
    Expect $a "(?s)shmtest: MAP_SHARED de arquivo.*?shmtest: TUDO OK \(0 falha\(s\)\)" "shmtest: MAP_SHARED com escrita em arquivo do tmpfs entre processos"
    Expect $a "o que o outro processo escreveu aparece aqui\s+ok" "shmtest: escrita de outro processo aparece no mapeamento"
    Expect $a "teste\.jpg: JPEG 96x64, 3 canais, media [0-9a-f]{6}\s*\nteste\.jpg: quadrantes vermelho verde azul amarelo" "stb_image decodifica JPEG"
    Expect $a "teste\.png: PNG 48x32, 3 canais, media [0-9a-f]{6}\s*\nteste\.png: quadrantes vermelho verde azul amarelo" "stb_image decodifica PNG"
    Expect $a "orion\.png: PNG 1280x800, 3 canais" "papel de parede PNG gerado no build esta na imagem"
    Expect $a '(?s)aslrtest: TUDO OK.*?cat /proc/self/maps\s*\n.*?\n([0-9a-f]+)-[0-9a-f]+ rw-p 00000000 00:00 0\s+\[stack\].*?cat /proc/self/maps\s*\n.*?\n(?!\1-)[0-9a-f]+-[0-9a-f]+ rw-p 00000000 00:00 0\s+\[stack\]' "dois cat /proc/self/maps seguidos mostram pilhas em enderecos diferentes"
    Expect $a 'login: root\s*Senha: \s*Bem-vindo, root\.' "boot pede usuario e senha, senha sem eco"
    Expect $a 'login: orion\s*Senha: \s*Login incorreto' "login recusa senha errada"
    Expect $a 'Login incorreto\s*orion login: orion\s*Senha: \s*Bem-vindo, orion\.' "login aceita orion depois do erro"
    Expect $a 'Bem-vindo, orion\.[\s\S]*?uid=1000\(orion\) gid=100\(users\) grupos=100\(users\)\r?\n' "login troca uid, gid e grupos"
    Expect $a '/home/orion \$ pwd\s*/home/orion' "login entra no HOME do usuario"
    Expect $a 'cat: /etc/shadow: Permissao negada' "orion nao le o /etc/shadow"
    Expect $a 'su -c id\s*Senha: \s*su: senha incorreta' "su recusa senha errada"
    Expect $a 'su -c id\s*Senha: \s*uid=0\(root\) gid=0\(root\)' "su com a senha do root vira root"
    Expect $a '\$ passwd\s*Senha atual: \s*Senha nova: \s*Repita a senha nova: \s*passwd: senha de orion trocada' "passwd do usuario pede a senha atual"
    Expect $a '(?s)senha de orion trocada.*?su orion -c id\s*Senha: \s*su: senha incorreta.*?su orion -c id\s*Senha: \s*uid=1000\(orion\)' "senha antiga deixa de valer e a nova vale"
    Expect $a '\norion:\$6\$(?!OrionUser0Salt01)[./0-9A-Za-z]{16}\$[./0-9A-Za-z]{86}:20000:' "passwd grava hash SHA-512 com sal novo"
    Expect $a '(?s)grep orion: /etc/shadow.*?-rw-------\s+1\s+root\s+root[^\n]*/etc/shadow' "/etc/shadow continua 0600 de root depois do passwd"
    Expect $a '-rwsr-xr-x\s+1\s+root\s+root[^\n]*/bin/su' "su e setuid root"
    Expect $a '-rwsr-xr-x\s+1\s+root\s+root[^\n]*/bin/passwd' "passwd e setuid root"
    Expect $a 'drwxr-xr-x\s+\d+\s+orion\s+users[^\n]*orion' "/home/orion e do orion na imagem"
    Expect $a 'passwd orion\s*Senha nova: ' "root troca a senha de outro sem a senha atual"
    Expect $a 'su orion -c id\s*uid=1000\(orion\)' "root vira orion pelo su sem senha"
    ExpectNot $a '(?i)senha[^:\n]*: [^\s]' "senha digitada nunca aparece na tela"
    Expect $a "64 bytes de 127\.0\.0\.1: seq=2" "loopback: ping 127.0.0.1"
    Expect $a "httpd: 127\.0\.0\.1:\d+ GET /etc/group -> 200" "loopback: httpd atende o httpget em 127.0.0.1"
    Expect $a "(?s)httpget 127\.0\.0\.1 8080 /etc/group.*?root:x:0:.*?httpget: HTTP/1\.0 200 OK" "loopback: httpget recebe a resposta"
    Expect $a "wget: index\.html salvo \(\d+ bytes" "wget http://example.com salvou o arquivo (depende da internet do hospedeiro)"
    Expect $a "grep Example index\.html\s*\n[^\n]*Example Domain" "o arquivo salvo e a pagina do example.com"
    Expect $a "1f8647d4cd2f7594c35c413e582e01089c00929dd4239eddd5ded3d40f325d53  /tmp/w\.bin" "wget -O: 1 MiB do hospedeiro com o sha256 certo"
    Expect $a "wget: o servidor respondeu 404" "wget avisa o 404 e sai com erro"
    Expect $a "cat /tmp/nc\.out\s*\n\s*transferido-pelo-nc" "nc -l e nc cliente pelo loopback"
    Expect $a "nc: 127\.0\.0\.1 7778 fechada" "nc -z ve a porta fechada"
    Expect $a "(?s)echo GET /index\.html HTTP/1\.0 \| nc -N 10\.0\.2\.2 8088.*?HTTP/1\.0 200 OK.*?ola do hospedeiro" "nc conversa HTTP com o hospedeiro"
    Expect $a "telnet: conectado a 127\.0\.0\.1 7779\s*\nola\s*\n" "telnet tira a negociacao do que mostra"
    Expect $a "0000000 377 376\s+1 157 151\s+15\s+12" "telnet recusa a opcao (IAC DONT) e manda CR LF"
    Expect $a "(?s)----- reboot -----.*?cat /reboot\.txt\s*\n\s*sobrevive-ao-reboot" "arquivo gravado na raiz sobrevive a reboot"
    Expect $a "(?s)cat /proc/self/maps\s*\n\s*[0-9a-f]{8,}-[0-9a-f]{8,} r--p [0-9a-f]{8} 00:00 \d+\s+/bin/cat" "cat /proc/self/maps mostra as VMAs do proprio cat"
    Expect $a "[0-9a-f]+-7ff[0-9a-f]{9} rw-p 00000000 00:00 0\s+\[stack\]" "maps mostra a pilha"
    Expect $a "proc /proc proc ro 0 0" "/proc/mounts lista o procfs"
    Expect $a "ahci\s+MSI no vetor 128, entregue na CPU 1" "AHCI liga a MSI e manda a IRQ para a CPU 1"
    Expect $a "(?s)cat /proc/interrupts\s*\n\s+CPU0\s+CPU1\s+CPU2\s+CPU3\s*\n.*?\n\s*128:\s+0\s+[1-9]\d*\s+0\s+0\s+ahci" "/proc/interrupts: AHCI por MSI so na CPU 1"
    Expect $a "\n\s*64:\s+[1-9]\d*\s+[1-9]\d*\s+[1-9]\d*\s+[1-9]\d*\s+lapic-timer" "/proc/interrupts: timer local nas 4 CPUs"
    Expect $a "(?s)cat /proc/stat\s*\ncpu  \d+ 0 \d+ [1-9]\d* 0 0 0 0 0 0\s*\ncpu0 \d+ 0 \d+ \d+ [^\n]*\ncpu1 [^\n]*\ncpu2 [^\n]*\ncpu3 \d+ 0 \d+ \d+ [^\n]*\nctxt [1-9]\d*" "/proc/stat tem o total e uma linha por CPU"
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
    Expect $a "e1000\s+8086:10d3.*link up" "placa de rede detectada com link up"
    Expect $a "ip 10\.0\.2\.15  mascara 255\.255\.255\.0" "dhcp pegou endereco, mascara e rotas"
    Expect $a "init: autostart: /bin/netup" "init roda o netup no boot"
    Expect $a "(?s)netup: eth0 por DHCP.*?dhcp: ip 10\.0\.2\.15 mascara 255\.255\.255\.0 gateway 10\.0\.2\.2 dns 10\.0\.2\.3 \(servidor 10\.0\.2\.2" "boot pega IP pelo DHCP de usuario, lendo o /etc/network"
    ExpectNot $a "\[info\] dhcp " "nenhuma linha do DHCP do kernel (ele saiu do kernel)"
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
    Expect $a "\n\s+(\d+)\s+1\s+\1\s+\d+\s+rodando\s+sh" "top listou o sh"
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
    Expect $a "\[1\] rodando\s+sleep 2\s*\n\[2\] rodando\s+sleep 1" "dois trabalhos em segundo plano ao mesmo tempo (numerados como no bash)"
    Expect $a "(?s)\$ fg\s*\nsleep 1" "fg traz o ultimo trabalho para o primeiro plano"
    Expect $a "\[1\] pronto\s+sleep 2" "trabalho terminado e avisado no prompt seguinte"
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
    Test-MemDelta $a "memoria estavel na sequencia A" 32
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
        "type:imgview /usr/share/imagens/teste.jpg &", "mon:sendkey ret", "wait:4",
        "shot:out\shots\reg-app-imgview.ppm", "mon:sendkey right", "wait:1", "mon:sendkey q", "wait:2",
        "type:sleep 100 &", "mon:sendkey ret", "wait:1", "type:monitor &", "mon:sendkey ret", "wait:4",
        "shot:out\shots\reg-app-monitor.ppm", "mon:sendkey pgdn", "mon:sendkey pgdn", "mon:sendkey pgdn", "mon:sendkey pgdn",
        "mon:sendkey up", "wait:0.5", "mon:sendkey tab", "mon:sendkey ret", "wait:2.5", "mon:sendkey q", "wait:2",
        "type:arquivos /usr/share &", "mon:sendkey ret", "wait:4", "mon:sendkey down", "mon:sendkey down", "mon:sendkey down",
        "mon:sendkey ret", "wait:1.5", "shot:out\shots\reg-app-arquivos.ppm", "mon:sendkey down", "mon:sendkey down", "mon:sendkey ret",
        "wait:4", "mon:sendkey q", "wait:2", "mon:sendkey tab", "type:pasta-nova", "mon:sendkey ret", "wait:1",
        "mon:sendkey tab", "mon:sendkey tab", "mon:sendkey tab", "mon:sendkey tab", "mon:sendkey ret", "wait:0.5", "mon:sendkey ret", "wait:1",
        "mon:sendkey q", "wait:2",
        "type:editor /tmp/nota.txt &", "mon:sendkey ret", "wait:4", "type:ola orion", "mon:sendkey ret", "type:segunda linha",
        "mon:sendkey backspace", "mon:sendkey backspace", "mon:sendkey backspace", "mon:sendkey backspace", "mon:sendkey backspace",
        "type:vez", "mon:sendkey up", "mon:sendkey end", "type:.", "mon:sendkey ctrl-s", "wait:1.5",
        "shot:out\shots\reg-app-editor.ppm", "mon:sendkey ctrl-q", "wait:2",
        "type:config &", "mon:sendkey ret", "wait:4", "mon:sendkey up", "mon:sendkey up", "mon:sendkey ret", "wait:2",
        "shot:out\shots\reg-app-config.ppm", "mon:sendkey down", "mon:sendkey ret", "wait:2", "mon:sendkey q", "wait:2",
        "type:terminal &", "mon:sendkey ret", "wait:4", "type:printenv COLUMNS > /tmp/term.txt", "mon:sendkey ret", "wait:1",
        "type:sleep 50", "mon:sendkey ret", "wait:1.5", "mon:sendkey ctrl-c", "wait:1",
        "type:echo depois >> /tmp/term.txt", "mon:sendkey ret", "wait:1", "type:ls /usr/share", "mon:sendkey ret", "wait:1.5",
        "shot:out\shots\reg-app-terminal.ppm", "type:exit", "mon:sendkey ret", "wait:2",
        "mon:sendkey ctrl-alt-x", "wait:5",
        "cmd:echo TTY-DE-VOLTA", "wait:1", "shot:out\shots\reg-wm-6-tty.ppm", "cmd:cat /tmp/nota.txt", "wait:1", "cmd:cat /tmp/term.txt", "wait:1",
        "cmd:cat /etc/wm.conf", "wait:1", "cmd:wm", "wait:4", "mon:sendkey ctrl-alt-x", "wait:5",
        "key:0B", "cmd:mem", "cmd:pty", "cmd:procs", "cmd:irq",
        "key:0B", "cmd:crash smep", "wait:4", "reboot:", "cmd:crash smap", "wait:4",
        "reboot:", "cmd:reboot", "wait:4", "monlog:info status",
        "reboot:", "cmd:poweroff", "wait:4", "monlog:info status",
        "reboot:", "mon:system_powerdown", "wait:8", "monlog:info status"
    )
    & (Join-Path $Root "test.ps1") -Smp $Smp -Image -NoBuild -LogPath "out\reg-b.log" `
        -Script $stepsB -CommandDelay 1.5 -BootTimeout 60 -NoPrint | Out-Null
    $b = Get-Log "out\reg-b.log"
    if ($Verbose) { Write-Output $b }

    Expect $b "adding /doom/freedoom1.wad" "doom: carregou o freedoom1.wad"
    Expect $b "doom: framebuffer \d+x\d+, escala \d" "doom: framebuffer e /dev/input"
    Expect $b "doom: \d{3,} quadros em \d+ ms" "doom rodou a demo (centenas de quadros)"
    Test-ShotColors "out\shots\reg-doom.ppm" "captura do doom tem a cena do jogo" 40
    Expect $b "wm: \d+x\d+, celula" "wm abriu o framebuffer"
    Expect $b "wm: fontes vetoriais: DejaVuSansMono\.ttf" "wm aberto pelo shell tambem carrega a fonte vetorial"
    Expect $b "wm: terminal 0 -> sh pid \d+" "wm abriu o primeiro terminal"
    Expect $b "wm: terminal 3 -> sh pid \d+" "Ctrl\+T abriu um segundo terminal"
    Expect $b "morto por sinal 2" "Ctrl\+C no terminal grafico matou o sleep"
    Expect $b "wm: encerrando" "Ctrl\+Alt\+X encerrou o wm"
    Expect $b "TTY-DE-VOLTA\r?\nTTY-DE-VOLTA|echo TTY-DE-VOLTA[\s\S]{0,40}TTY-DE-VOLTA" "TTY respondeu depois do wm"
    Expect $b "0 pty\(s\) viva\(s\)" "nenhuma pty vazada"
    Expect $b "\n\s+128\s+0\s+[1-9]\d*\s+0\s+0\s+ahci" "irq do shell do kernel mostra a AHCI em outra CPU (MSI)"
    Expect $b "(?s)pid\s+ppid\s+estado[^\n]*\n\s+\d+\s+1\s+running\s+\d+\s+sh\s*\n\s+1\s+0\s+running\s+\d+\s+init" "procs so tem init e sh"
    Test-MemDelta $b "memoria estavel na sequencia B" 16
    Expect $b "wm: app abriu a janela \d+ \(660x490, 'imgview'\)" "imgview abre a janela"
    Expect $b "agora se chama 'imgview: teste\.jpg 96x64'\s*\n(wm: janela \d+ mostrou o primeiro quadro\s*\n)?wm: janela \d+ agora se chama 'imgview: teste\.png 48x32'" "imgview mostra o JPEG e a seta passa para o PNG da mesma pasta"
    Expect $b "wm: app da janela \d+ saiu \(pediu para fechar\) depois de \d+ quadro" "imgview fecha com q"
    Test-ShotPixel "out\shots\reg-app-imgview.ppm" 413 223 0xc82828 40 "captura do imgview: quadrante vermelho do JPEG"
    Test-ShotPixel "out\shots\reg-app-imgview.ppm" 698 413 0xdcc828 40 "captura do imgview: quadrante amarelo do JPEG"
    Expect $b "wm: app abriu a janela \d+ \(640x470, 'monitor'\)" "monitor abre a janela"
    Expect $b "agora se chama 'monitor: [1-9]\d* processos'" "monitor conta os processos pelo /proc"
    Expect $b "(?s)agora se chama 'monitor: TERM (\d+) sleep'.*?wm: janela \d+ agora se chama 'monitor: pid \1 saiu'" "monitor encerra o sleep escolhido na lista e a lista se atualiza sozinha"
    Expect $b "(?s)monitor: TERM \d+ sleep.*?wm: app da janela \d+ saiu \(pediu para fechar\) depois de \d+ quadro\(s\), [1-9]\d* parcial" "monitor fecha com q e redesenha por areas sujas"
    Test-ShotPixel "out\shots\reg-app-monitor.ppm" 883 289 0xe0a040 30 "captura do monitor: grafico de memoria desenhado"
    Test-ShotPixel "out\shots\reg-app-monitor.ppm" 276 245 0x141a26 12 "captura do monitor: fundo do grafico de CPU"
    Expect $b "wm: app abriu a janela \d+ \(640x440, 'arquivos'\)" "arquivos abre a janela"
    Expect $b "(?s)agora se chama 'arquivos: /usr/share'.*?agora se chama 'arquivos: /usr/share/imagens'" "arquivos entra numa pasta pelo teclado"
    Expect $b "(?s)agora se chama 'arquivos: /usr/share/imagens'.*?wm: app abriu a janela \d+ \(660x490, 'imgview'\).*?agora se chama 'imgview: teste\.jpg 96x64'" "arquivos abre a imagem escolhida no imgview"
    Expect $b "agora se chama 'arquivos: criou pasta-nova'" "arquivos cria uma pasta"
    Expect $b "(?s)'arquivos: criou pasta-nova'.*?agora se chama 'arquivos: apagou pasta-nova'" "arquivos apaga a pasta depois da confirmacao"
    Expect $b "(?s)'arquivos: apagou pasta-nova'.*?wm: app da janela \d+ saiu \(pediu para fechar\)" "arquivos fecha com q"
    Test-ShotPixel "out\shots\reg-app-arquivos.ppm" 900 192 0xf4f6fa 12 "captura do arquivos: campo do caminho"
    Test-ShotPixel "out\shots\reg-app-arquivos.ppm" 306 241 0xe8ecf4 24 "captura do arquivos: texto da lista (teste.jpg)"
    Expect $b "wm: app abriu a janela \d+ \(640x470, 'editor'\)" "editor abre a janela"
    Expect $b "(?s)agora se chama 'editor: nota\.txt'\s*\n.*?agora se chama 'editor: nota\.txt \*'.*?agora se chama 'editor: salvou nota\.txt'" "editor marca a mudanca e salva com Ctrl+S"
    Expect $b "cat /tmp/nota\.txt\s*\nola orion\.\s*\nsegunda vez\s*\n" "editor gravou o texto com as edicoes (Backspace, seta, End)"
    Expect $b "(?s)'editor: salvou nota\.txt'.*?wm: app da janela \d+ saiu" "editor fecha com Ctrl+Q"
    Test-ShotPixel "out\shots\reg-app-editor.ppm" 520 245 0xffd060 30 "captura do editor: cursor no fim da primeira linha"
    Test-ShotPixel "out\shots\reg-app-editor.ppm" 700 450 0x141a26 12 "captura do editor: fundo da pagina"
    Expect $b "wm: app abriu a janela \d+ \(640x400, 'config'\)" "config abre a janela"
    Expect $b "(?s)wm: wmctl: fundo /usr/share/imagens/teste\.jpg.*?wm: papel de parede /usr/share/imagens/teste\.jpg \(JPEG 96x64\)" "config troca o papel de parede pelo comando fundo do wm"
    Expect $b "(?s)agora se chama 'config: fundo teste\.jpg'.*?agora se chama 'config: fundo teste\.png'" "config aplica a escolha da lista com Enter"
    Expect $b "(?s)'config: fundo teste\.png'.*?wm: app da janela \d+ saiu \(pediu para fechar\)" "config fecha com q"
    Expect $b "cat /etc/wm\.conf\s*\nfundo=/usr/share/imagens/teste\.png" "config grava a escolha em /etc/wm.conf"
    Expect $b "wm: fundo de /etc/wm\.conf: /usr/share/imagens/teste\.png\s*\n[\s\S]*?wm: papel de parede /usr/share/imagens/teste\.png \(PNG 48x32\)" "wm le o papel de parede de /etc/wm.conf ao iniciar"
    Test-ShotPixel "out\shots\reg-app-config.ppm" 100 200 0xc82828 40 "captura do config: fundo novo, quadrante vermelho do JPEG"
    Test-ShotPixel "out\shots\reg-app-config.ppm" 1100 200 0x28b33c 40 "captura do config: fundo novo, quadrante verde do JPEG"
    Expect $b "wm: app abriu a janela \d+ \(796x496, 'terminal'\)" "terminal abre a janela"
    Expect $b "agora se chama 'terminal: sh pid \d+'" "terminal abre um sh numa pty"
    Expect $b "cat /tmp/term\.txt\s*\n48\s*\ndepois\s*\n" "terminal: comandos e Ctrl+C chegam ao sh do app (COLUMNS=48) pela pty"
    Expect $b "(?s)'terminal: sh pid \d+'.*?wm: app da janela \d+ saiu \(pediu para fechar\)" "terminal fecha quando o sh sai"
    Test-ShotPixel "out\shots\reg-app-terminal.ppm" 345 350 0xc8d4e0 24 "captura do terminal: cursor depois do ls, na linha do prompt"
    Test-ShotPixel "out\shots\reg-app-terminal.ppm" 700 500 0x0a0e17 12 "captura do terminal: fundo da tela"
    Expect $b 'crash: pedindo ao kernel para executar codigo de usuario \(SMEP\)[\s\S]*?EXCECAO NAO TRATADA: #PF Page Fault[\s\S]*?busca de instrucao[\s\S]*?SMEP: o kernel tentou executar codigo de usuario \(endereco 0000[0-9a-f]{12}\); panico controlado' "SMEP: kernel executando pagina de usuario vira panico controlado"
    Expect $b 'crash: pedindo ao kernel para ler memoria de usuario sem copy_from_user \(SMAP\)[\s\S]*?EXCECAO NAO TRATADA: #PF Page Fault[\s\S]*?SMAP: o kernel acessou memoria de usuario fora das copias de uaccess \(endereco 0000[0-9a-f]{12}\); panico controlado' "SMAP: kernel lendo memoria de usuario fora do uaccess vira panico controlado"
    ExpectNot $b 'crash: o kernel voltou' "o kernel nunca volta de um acesso proibido a memoria de usuario"
    Expect $b 'reboot: gravando os discos e reiniciando\s*\[info\] acpi\s+reiniciando: gravando os discos\s*\[info\] acpi\s+reiniciando pelo registrador de reset do FADT(?:(?!----- reboot)[\s\S])*?VM status: paused \(shutdown\)' "reboot pelo registrador de reset do FADT (uACPI)"
    ExpectNot $b 'reiniciando pelo controlador do teclado' "reboot nao precisou do controlador do teclado"
    Expect $b 'poweroff: gravando os discos e desligando\s*\[info\] acpi\s+desligando: gravando os discos\s*\[info\] acpi\s+entrando no estado S5(?:(?!----- reboot)[\s\S])*?VM status: paused \(shutdown\)' "poweroff entra no S5 e o QEMU desliga"
    Expect $b 'botao de energia apertado: desligando\s*\[info\] acpi\s+desligando: gravando os discos\s*\[info\] acpi\s+entrando no estado S5(?:(?!----- reboot)[\s\S])*?VM status: paused \(shutdown\)' "botao de energia (system_powerdown) desliga pelo ACPI"
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
        "cmd:touch /mnt/t1", "cmd:chmod 555 /mnt/t1", "cmd:ls -l /mnt/t1", "cmd:chmod 600 /mnt/t1",
        "cmd:chmod 755 /mnt/t1", "cmd:rm /mnt/t1", "wait:1",
        "cmd:mkdir /mnt/q", "cmd:mount /dev/sdb /mnt/q", "cmd:rm -r /mnt/q", "wait:1",
        "cmd:mkdir /tmp/e4", "cmd:mount -t ext2 /dev/sdc /tmp/e4", "cmd:ext2test /tmp/e4", "until:300:falha(s))",
        "cmd:/tmp/e4/bin/hello ext2", "wait:2",
        "cmd:mkfs.ext2 /dev/sdc", "cmd:fsck.ext2 /dev/sdc", "wait:1",
        "cmd:mkdir /tmp/e1", "cmd:mount -t ext2 /dev/sdd /tmp/e1", "cmd:ext2test /tmp/e1", "until:300:falha(s))",
        "cmd:umount /tmp/e4", "cmd:umount /tmp/e1", "wait:1",
        "cmd:fsck.ext2 /dev/sdc", 'cmd:echo FIM$X-FSCK1', "until:120:FIM-FSCK1",
        "cmd:fsck.ext2 /dev/sdd", 'cmd:echo FIM$X-FSCK2', "until:120:FIM-FSCK2",
        "cmd:mkfs.ext2 -L NOVO /dev/sdd", "until:120:rotulo 'NOVO'", 'cmd:echo FIM$X-MKFS', "until:120:FIM-MKFS",
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
    & (Join-Path $Root "test.ps1") -Smp $Smp -Image -NoBuild -Disk -ExtraDisks @("out/ext2-4k.img", "out/ext2-1k.img") -LogPath "out\reg-c.log" `
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
    Expect $c "-r-xr-xr-x\s+1\s+root\s+root\s+0 \w{3} [ \d]\d \d\d:\d\d /mnt/t1" "FAT: chmod 555 liga o somente leitura"
    Expect $c "chmod /mnt/t1: Operation not permitted" "FAT: chmod que o FAT nao guarda da EPERM"
    Test-MemDelta $c "memoria estavel na sequencia C" 24
}

# ---------------- Sequencia D: boot direto na interface grafica ----------------
if ($runAll -or $OnlyD) {
    Write-Host "== Sequencia D (boot direto na GUI) ==" -ForegroundColor Cyan
    $stepsD = @(
        "until:60:wm: apps em /run/wmapp.sock", "wait:3", "shot:out\shots\reg-gui-1-boot.ppm",
        "type:cpuuso wm 10 > /dev/tty", "mon:sendkey ret", "until:40:cpuuso: wm (pid", "wait:1",
        "type:wmctl ping", "mon:sendkey ret", "wait:2", "type:wmctl term", "mon:sendkey ret", "wait:3",
        "type:wmctl list", "mon:sendkey ret", "wait:2", "shot:out\shots\reg-gui-5-wmctl.ppm",
        "type:hello-gui &", "mon:sendkey ret", "wait:4", "mon:sendkey o", "wait:1", "mon:sendkey i", "wait:1",
        "mon:mouse_move -340 -200", "wait:0.5", "mon:mouse_button 1", "wait:0.4", "mon:mouse_button 0", "wait:1",
        "shot:out\shots\reg-gui-6-hello-gui.ppm", "mon:sendkey q", "wait:2",
        "type:guidemo &", "mon:sendkey ret", "wait:4", "type:ana", "mon:sendkey ret", "wait:1",
        "mon:sendkey tab", "mon:sendkey ret", "mon:sendkey ret", "wait:1", "mon:sendkey tab", "mon:sendkey spc", "wait:1",
        "mon:sendkey tab", "mon:sendkey down", "mon:sendkey down", "mon:sendkey down", "wait:1", "mon:sendkey pgdn", "wait:1",
        "mon:sendkey tab", "mon:sendkey ret", "wait:1", "shot:out\shots\reg-gui-7-guidemo.ppm",
        "mon:mouse_move 138 -44", "wait:0.5", "mon:mouse_button 1", "wait:0.3", "mon:mouse_button 0", "wait:1",
        "shot:out\shots\reg-gui-8-menu-ajuda.ppm",
        "mon:mouse_move 0 26", "wait:0.5", "mon:mouse_button 1", "wait:0.3", "mon:mouse_button 0", "wait:1",
        "mon:sendkey f10", "wait:0.5", "mon:sendkey right", "mon:sendkey left", "mon:sendkey down", "mon:sendkey down",
        "wait:0.5", "shot:out\shots\reg-gui-9-menu-arquivo.ppm", "mon:sendkey ret", "wait:2",
        "mon:sendkey ctrl-p", "wait:4", "shot:out\shots\reg-gui-2-monitor.ppm",
        "mon:sendkey ctrl-f", "wait:4", "shot:out\shots\reg-gui-3-arquivos.ppm",
        "mon:sendkey ctrl-t", "wait:2", "mon:sendkey alt-f10", "wait:1.5", "shot:out\shots\reg-gui-10-maximizado.ppm",
        "mon:sendkey alt-f10", "wait:1", "mon:sendkey alt-f9", "wait:1", "shot:out\shots\reg-gui-11-minimizado.ppm",
        "mon:sendkey alt-tab", "wait:0.5", "mon:sendkey alt-tab", "wait:0.5", "mon:sendkey alt-tab", "wait:0.5",
        "mon:sendkey alt-tab", "wait:0.5", "mon:sendkey alt-tab", "wait:0.5", "mon:sendkey alt-tab", "wait:1",
        "mon:sendkey alt-f1", "wait:0.5", "mon:sendkey down", "mon:sendkey down", "mon:sendkey down", "mon:sendkey down",
        "mon:sendkey down", "mon:sendkey down", "wait:0.5", "shot:out\shots\reg-gui-4-menu.ppm", "mon:sendkey ret",
        "wait:4", "shot:out\shots\reg-gui-12-tres-apps.ppm",
        "mon:sendkey ctrl-alt-x", "wait:6"
    )
    & (Join-Path $Root "test.ps1") -Smp $Smp -Image -Gui -Nic e1000 -LogPath "out\reg-d.log" `
        -Script $stepsD -CommandDelay 1.5 -BootTimeout 60 -NoPrint | Out-Null
    $d = Get-Log "out\reg-d.log"
    if ($Verbose) { Write-Output $d }
    & (Join-Path $Root "build.ps1") -Image | Out-Null

    Expect $d "init: autostart: /bin/netup" "init executou o autostart"
    Expect $d "dhcp: ip 10\.0\.2\.15" "netup do autostart configurou a rede pelo dhcp de usuario"
    Expect $d "init: iniciando /bin/wm" "init subiu direto na interface grafica"
    Expect $d "e1000\s+8086:100e[^\n]*GSI (1[6-9]|2[0-3]) pelo _PRT, link up" "e1000 antiga (sem MSI) segue pela GSI do _PRT"
    Expect $d "wm: \d+x\d+, celula" "wm abriu o framebuffer sem passar pelo sh"
    Expect $d "wm: terminal 0 -> sh pid \d+" "wm abriu um terminal com o sh dentro"
    Expect $d "wm: lancou monitor \(pid \d+\)[\s\S]*?wm: app abriu a janela \d+ \(640x470, 'monitor'\)" "Ctrl\+P abre o monitor (app)"
    Expect $d "wm: lancou arquivos \(pid \d+\)[\s\S]*?wm: app abriu a janela \d+ \(640x440, 'arquivos'\)" "Ctrl\+F abre o arquivos (app)"
    Expect $d "wm: menu: Configuracoes\s*\nwm: lancou config \(pid \d+\)[\s\S]*?wm: app abriu a janela \d+ \(640x400, 'config'\)" "menu do wm (Alt+F1, setas e Enter) lanca o config"
    Test-ShotPixel "out\shots\reg-gui-4-menu.ppm" 240 148 0x2c5e9e 12 "captura do menu aberto pelo teclado com Configuracoes destacado"
    Test-ShotPixel "out\shots\reg-gui-12-tres-apps.ppm" 600 600 0x1b2230 12 "captura com 3 apps: janela do config na frente"
    Test-ShotPixel "out\shots\reg-gui-12-tres-apps.ppm" 1250 785 0x2c5e9e 12 "captura com 3 apps: config em foco na barra de tarefas"
    Test-ShotPixel "out\shots\reg-gui-12-tres-apps.ppm" 700 785 0x2a3040 12 "captura com 3 apps: monitor na barra de tarefas"
    Test-ShotPixel "out\shots\reg-gui-12-tres-apps.ppm" 900 785 0x2a3040 12 "captura com 3 apps: arquivos na barra de tarefas"
    Expect $d "wm: terminal \d+ maximizado: 126x33" "Alt\+F10 maximiza o terminal na area de trabalho (126x33)"
    Expect $d "wm: terminal \d+ volta ao tamanho normal: 60x20" "Alt\+F10 de novo volta o terminal a 60x20"
    Expect $d "(wm: alt\+tab: janela \d+ em foco[^\n]*\s*\n){6}" "Alt\+Tab passa por seis janelas seguidas"
    Expect $d "(?s)wm: janela (\d+) minimizada \(Terminal \d+\).*?wm: alt\+tab: janela \1 em foco" "Alt\+Tab traz de volta a janela minimizada com Alt\+F9"
    Test-ShotPixel "out\shots\reg-gui-10-maximizado.ppm" 1200 600 0x0a0e17 12 "captura: terminal maximizado cobre a area de trabalho"
    Test-ShotPixel "out\shots\reg-gui-10-maximizado.ppm" 3 785 0x161c2b 12 "captura: barra de tarefas embaixo"
    Test-ShotPixel "out\shots\reg-gui-11-minimizado.ppm" 3 785 0x161c2b 12 "captura: barra de tarefas com a janela minimizada"
    Expect $d "wm: encerrando" "Ctrl\+Alt\+X encerrou o wm"
    Expect $d "init: /bin/wm terminou" "init percebeu a saida do wm"
    Expect $d "wm: comandos em /run/wm\.sock" "wm escuta comandos num socket Unix"
    Expect $d "wm: wmctl: ping" "wmctl conversou com o wm pelo socket Unix"
    Expect $d "wm: wmctl: term\s*\nwm: terminal \d+ -> sh pid \d+" "wmctl term: o wm abriu um terminal a pedido de outro programa"
    Expect $d "wm: wmctl: list" "wmctl list pelo socket Unix"
    Expect $d "wm: apps em /run/wmapp\.sock" "wm escuta apps num socket Unix proprio"
    Expect $d "wm: fontes vetoriais: DejaVuSansMono\.ttf 20 px no terminal, DejaVuSans\.ttf 15 px na barra e nos titulos" "terminal e barra com fonte vetorial (stb_truetype e DejaVu)"
    Expect $d "wm: 1280x800, celula 10x21" "celula do terminal vem da metrica da fonte"
    Expect $d "wm: papel de parede /usr/share/wallpapers/orion\.png \(PNG 1280x800\)" "wm carrega o papel de parede PNG"
    Test-ShotColors "out\shots\reg-gui-1-boot.ppm" "captura do boot grafico mostra o papel de parede" 250
    Expect $d "wm: glifo 'A' do terminal: \d+x\d+ pixels, ([1-9]\d|\d{3}) niveis de alfa \(antialias\)" "glifos com antialias (10 ou mais niveis de alfa)"
    Expect $d "wm: ([5-9]\d|\d{3}) glifo\(s\) vetoriais no cache" "cache de glifos usado pelo terminal e pela barra"
    Expect $d "wm: app abriu a janela (\d+) \(360x200, 'Ola do hello-gui'\) com o buffer em /run/wm/app-\d+\.buf\s*\nwm: janela \1 mostrou o primeiro quadro" "hello-gui abre uma janela propria e desenha no buffer compartilhado"
    Expect $d "wm: janela \d+ agora se chama 'hello-gui: tecla o'\s*\nwm: janela \d+ agora se chama 'hello-gui: tecla i'" "teclas vao do wm ao hello-gui e a resposta volta pelo protocolo"
    Expect $d "wm: janela \d+ agora se chama 'hello-gui: clique \d+,\d+'" "clique na janela chega ao hello-gui em coordenadas da janela"
    Expect $d "wm: app da janela \d+ saiu \(pediu para fechar\) depois de [4-9] quadro\(s\)" "hello-gui fecha com q e o wm tira a janela"
    Test-ShotColors "out\shots\reg-gui-6-hello-gui.ppm" "captura com o hello-gui tem o gradiente da janela" 100
    Expect $d "wm: app abriu a janela \d+ \(600x400, 'guidemo'\)" "guidemo (libgui) abre a janela"
    Expect $d "agora se chama 'guidemo: ola ana'" "libgui: campo de texto recebe o texto e o Enter"
    Expect $d "'guidemo: somar 1'\s*\nwm: janela \d+ agora se chama 'guidemo: somar 2'" "libgui: Tab leva o foco ao botao e o Enter o aciona"
    Expect $d "agora se chama 'guidemo: caixa ligada'" "libgui: caixa de opcao com espaco"
    Expect $d "'guidemo: item Item 1'\s*\n[^\n]*'guidemo: item Item 2'\s*\n[^\n]*'guidemo: item Item 3'\s*\n[^\n]*'guidemo: item Item 15'" "libgui: lista com setas e Page Down (rolagem)"
    Expect $d "agora se chama 'guidemo: lista com 41'" "libgui: botao acrescenta item na lista"
    Expect $d "agora se chama 'guidemo: sobre'" "libgui: menu Ajuda/Sobre pelo mouse"
    Expect $d "'guidemo: saindo'\s*\nwm: app da janela \d+ saiu \(pediu para fechar\)" "libgui: menu Arquivo/Sair pelo teclado (F10, setas, Enter) fecha o app"
    Expect $d "'guidemo: saindo'\s*\nwm: app da janela \d+ saiu \(pediu para fechar\) depois de \d+ quadro\(s\), [1-9]\d* parcial\(is\)" "libgui manda so a area que mudou (quadros parciais)"
    Expect $d "cpuuso: wm \(pid \d+\) usou [0-4]\.\d% de CPU em 10 s" "wm parado gasta menos de 5% de CPU"
}

# ---------------- Sequencia F: ISO hibrida ----------------
if ($runAll -or $OnlyF) {
    Write-Host "== Sequencia F (ISO hibrida: CD e pendrive, BIOS e UEFI) ==" -ForegroundColor Cyan
    if (-not $NoBuild -or -not (Test-Path (Join-Path $Root "out\orion.iso"))) {
        & (Join-Path $Root "build.ps1") -Iso | Out-Null
    }
    $stepsF = @(
        "cmd:mount", "cmd:id", "cmd:ls /bin | wc", "cmd:ls -l /etc/shadow /bin/su", "cmd:ls -l /home",
        "cmd:echo vivo-na-ram > /home/orion/nota.txt", "cmd:cat /home/orion/nota.txt", "cmd:hello",
        "cmd:lua -e print(6*7)", "cmd:su orion -c id", "cmd:free", "wait:1",
        "reboot:uefi,cd", "cmd:mount", "cmd:cat /home/orion/nota.txt", "wait:1",
        "reboot:bios,isodisk", "cmd:mount", "cmd:poweroff", "wait:4", "monlog:info status",
        "reboot:uefi,isodisk", "cmd:mount", "cmd:ls /doom", "cmd:reboot", "wait:4", "monlog:info status"
    )
    & (Join-Path $Root "test.ps1") -Smp $Smp -Iso -Bios -NoBuild -LogPath "out\reg-f.log" `
        -Script $stepsF -CommandDelay 1.5 -BootTimeout 90 -NoPrint | Out-Null
    $f = Get-Log "out\reg-f.log"
    if ($Verbose) { Write-Output $f }

    Expect $f '^(?:(?!----- reboot)[\s\S])*?Limine [\d.]+ via BIOS[^\n]*\n[^\n]*kernel lido de /boot/orion\.elf no CD(?:(?!----- reboot)[\s\S])*?raiz montada: tmpfs do initramfs \(\d{3,} entradas, \d{5,} KiB, 0 falha\(s\)\); \d{5,} KiB do modulo devolvidos(?:(?!----- reboot)[\s\S])*?login: root\s*Senha: \s*Bem-vindo, root\.(?:(?!----- reboot)[\s\S])*?\ninitramfs / tmpfs rw' "ISO como CD no BIOS: El Torito, Limine, raiz em RAM e login"
    Expect $f '----- reboot uefi,cd -----(?:(?!----- reboot)[\s\S])*?Limine [\d.]+ via UEFI 64[^\n]*\n[^\n]*kernel lido de /boot/orion\.elf no CD(?:(?!----- reboot)[\s\S])*?raiz montada: tmpfs do initramfs \(\d{3,} entradas, \d{5,} KiB, 0 falha\(s\)\); \d{5,} KiB do modulo devolvidos(?:(?!----- reboot)[\s\S])*?login: root\s*Senha: \s*Bem-vindo, root\.(?:(?!----- reboot)[\s\S])*?\ninitramfs / tmpfs rw' "ISO como CD no UEFI: imagem EFI do El Torito, raiz em RAM e login"
    Expect $f '----- reboot bios,isodisk -----(?:(?!----- reboot)[\s\S])*?Limine [\d.]+ via BIOS[^\n]*\n[^\n]*kernel lido de /boot/orion\.elf no disco(?:(?!----- reboot)[\s\S])*?raiz montada: tmpfs do initramfs \(\d{3,} entradas, \d{5,} KiB, 0 falha\(s\)\); \d{5,} KiB do modulo devolvidos(?:(?!----- reboot)[\s\S])*?login: root\s*Senha: \s*Bem-vindo, root\.(?:(?!----- reboot)[\s\S])*?\ninitramfs / tmpfs rw' "ISO gravada em pendrive no BIOS: MBR do Limine, raiz em RAM e login"
    Expect $f '----- reboot uefi,isodisk -----(?:(?!----- reboot)[\s\S])*?Limine [\d.]+ via UEFI 64[^\n]*\n[^\n]*kernel lido de /boot/orion\.elf no disco(?:(?!----- reboot)[\s\S])*?raiz montada: tmpfs do initramfs \(\d{3,} entradas, \d{5,} KiB, 0 falha\(s\)\); \d{5,} KiB do modulo devolvidos(?:(?!----- reboot)[\s\S])*?login: root\s*Senha: \s*Bem-vindo, root\.(?:(?!----- reboot)[\s\S])*?\ninitramfs / tmpfs rw' "ISO gravada em pendrive no UEFI: particao EFI do MBR, raiz em RAM e login"
    Expect $f "ls /bin \| wc\s*\n\s*1\d\d\s" "live: /bin inteiro no initramfs"
    Expect $f "-rw-------\s+1\s+root\s+root[^\n]*/etc/shadow" "live: /etc/shadow 0600 de root"
    Expect $f "-rwsr-xr-x\s+1\s+root\s+root[^\n]*/bin/su" "live: su setuid root"
    Expect $f "drwxr-xr-x\s+\d+\s+orion\s+users[^\n]*orion" "live: /home/orion do orion"
    Expect $f "cat /home/orion/nota\.txt\s*\nvivo-na-ram" "live: grava e le arquivo na raiz em RAM"
    Expect $f "hello from ring 3! pid \d+" "live: hello roda"
    Expect $f "lua -e print\(6\*7\)\s*\n42" "live: lua roda"
    Expect $f "su orion -c id\s*\nuid=1000\(orion\)" "live: su para orion"
    Expect $f "----- reboot uefi,cd -----[\s\S]*?cat: /home/orion/nota\.txt: Arquivo ou diretorio inexistente" "live: nada sobrevive ao reboot (raiz so em RAM)"
    Expect $f "ls /doom\s*\n[^\n]*freedoom1\.wad" "live: o wad do Doom esta no initramfs"
    Expect $f '----- reboot bios,isodisk -----(?:(?!----- reboot)[\s\S])*?entrando no estado S5(?:(?!----- reboot)[\s\S])*?VM status: paused \(shutdown\)' "poweroff pelo ACPI no BIOS"
    Expect $f '----- reboot uefi,isodisk -----(?:(?!----- reboot)[\s\S])*?reiniciando pelo registrador de reset do FADT(?:(?!----- reboot)[\s\S])*?VM status: paused \(shutdown\)' "reboot pelo ACPI no UEFI a partir do live"
    ExpectNot $f "PANIC|EXCECAO" "nenhum panico nos quatro boots da ISO"
}

# ---------------- Sequencia V: virtio ----------------
if ($runAll -or $OnlyV) {
    Write-Host "== Sequencia V (virtio-blk e virtio-net) ==" -ForegroundColor Cyan
    $stepsV = @(
        "cmd:cat /proc/mounts", "wait:1",
        "cmd:mkdir /root/vt", "cmd:fstest -a /root/vt", 'cmd:echo FIM$X-VFSTEST', "until:600:FIM-VFSTEST",
        "cmd:rmdir /root/vt", "cmd:echo persiste-no-virtio > /root/persiste.txt", "cmd:sync", "wait:2",
        "cmd:ping 10.0.2.2 3", "wait:8", "cmd:httpget -q 10.0.2.2 8088 /big10.bin", "until:120:corpo de",
        "cmd:ifconfig", "wait:1", "cmd:cat /proc/interrupts", "wait:1",
        "reboot:", "cmd:cat /root/persiste.txt", "cmd:rm /root/persiste.txt", "cmd:sync", "wait:1"
    )
    $web = Start-HostWeb 8088
    & (Join-Path $Root "test.ps1") -Smp $Smp -Image -NoBuild -Bus virtio -Nic virtio-net-pci -LogPath "out\reg-v.log" `
        -Script $stepsV -CommandDelay 1.5 -BootTimeout 60 -NoPrint | Out-Null
    Stop-Process -Id $web.Id -Force -ErrorAction SilentlyContinue
    $v = Get-Log "out\reg-v.log"
    if ($Verbose) { Write-Output $v }

    Expect $v "virtio\s+vda: virtio-blk 1af4:(1001|1042) [^\n]*MSI-X no vetor \d+, CPU [1-9]" "virtio-blk liga a MSI-X fora da CPU 0"
    Expect $v "vda2: GPT, LBA \d+, \d+ setores \(256 MiB\), raiz Linux x86-64" "particoes GPT do disco virtio"
    Expect $v "vfs\s+raiz montada: ext2 em vda2" "raiz ext2 no disco virtio"
    Expect $v "/dev/vda2 / ext2 rw 0 0" "/proc/mounts mostra a raiz em vda2"
    Expect $v "(?s)fstest -a /root/vt.*?fstest: TUDO OK.*?FIM-VFSTEST" "fstest -a na raiz pelo virtio-blk"
    Expect $v "(?s)cat /proc/interrupts.*?\n\s*\d+:(\s+\d+){4}\s+virtio-blk" "/proc/interrupts conta a MSI-X do virtio-blk"
    ExpectNot $v "(?s)cat /proc/interrupts.*?\n\s*\d+:\s+0\s+0\s+0\s+0\s+virtio-blk" "a MSI-X do virtio-blk chegou de fato"
    Expect $v "(?s)----- reboot -----.*?cat /root/persiste\.txt\s*\npersiste-no-virtio" "o que foi gravado pelo virtio-blk sobrevive ao reboot"
    Expect $v "virtio\s+virtio-net 1af4:(1000|1041) [^\n]*MSI-X no vetor \d+, CPU [1-9], link up" "virtio-net liga a MSI-X fora da CPU 0"
    ExpectNot $v "placa 1af4:1000 nao suportada" "o driver da e1000 nao reclama da placa virtio"
    Expect $v "dhcp: ip 10\.0\.2\.15" "DHCP pela virtio-net"
    Expect $v "3 enviado\(s\), 3 recebido\(s\), 0% de perda" "ping pela virtio-net sem perda"
    Expect $v "httpget: corpo de 10485760 bytes, crc32 1a960f7c" "TCP: 10 MiB do hospedeiro pela virtio-net sem corromper"
    Expect $v "(?s)cat /proc/interrupts.*?\n\s*\d+:(\s+\d+){4}\s+virtio-net" "/proc/interrupts conta a MSI-X da virtio-net"
    ExpectNot $v "(?s)cat /proc/interrupts.*?\n\s*\d+:\s+0\s+0\s+0\s+0\s+virtio-net" "a MSI-X da virtio-net chegou de fato"
    ExpectNot $v "PANIC|EXCECAO" "nenhum panico na sequencia V"
}

# ---------------- Sequencia N: NVMe ----------------
if ($runAll -or $OnlyN) {
    Write-Host "== Sequencia N (raiz no NVMe) ==" -ForegroundColor Cyan
    $stepsN = @(
        "cmd:cat /proc/mounts", "wait:1",
        "cmd:mkdir /root/nt", "cmd:fstest -a /root/nt", 'cmd:echo FIM$X-NFSTEST', "until:600:FIM-NFSTEST",
        "cmd:rmdir /root/nt", "cmd:echo persiste-no-nvme > /root/persiste.txt", "cmd:sync", "wait:2",
        "cmd:cat /proc/interrupts", "wait:1",
        "reboot:", "cmd:cat /root/persiste.txt", "cmd:rm /root/persiste.txt", "cmd:sync", "wait:1"
    )
    & (Join-Path $Root "test.ps1") -Smp $Smp -Image -NoBuild -Bus nvme -LogPath "out\reg-n.log" `
        -Script $stepsN -CommandDelay 1.5 -BootTimeout 60 -NoPrint | Out-Null
    $n = Get-Log "out\reg-n.log"
    if ($Verbose) { Write-Output $n }

    Expect $n "nvme\s+nvme0n1: 1b36:0010 [^\n]*versao 1\.\d[^\n]*MSI-X no vetor \d+, CPU [1-9]" "NVMe: controlador, Identify e MSI-X fora da CPU 0"
    Expect $n "nvme0n1p2: GPT, LBA \d+, \d+ setores \(256 MiB\), raiz Linux x86-64" "particoes GPT do NVMe com nomes nvme0n1pN"
    Expect $n "vfs\s+raiz montada: ext2 em nvme0n1p2" "boot com a raiz no NVMe"
    Expect $n "/dev/nvme0n1p2 / ext2 rw 0 0" "/proc/mounts mostra a raiz no NVMe"
    Expect $n "(?s)fstest -a /root/nt.*?fstest: TUDO OK.*?FIM-NFSTEST" "fstest -a na raiz pelo NVMe"
    Expect $n "(?s)cat /proc/interrupts.*?\n\s*\d+:(\s+\d+){4}\s+nvme" "/proc/interrupts conta a MSI-X do NVMe"
    ExpectNot $n "(?s)cat /proc/interrupts.*?\n\s*\d+:\s+0\s+0\s+0\s+0\s+nvme" "a MSI-X do NVMe chegou de fato"
    Expect $n "(?s)----- reboot -----.*?cat /root/persiste\.txt\s*\npersiste-no-nvme" "o que foi gravado no NVMe sobrevive ao reboot"
    ExpectNot $n "PANIC|EXCECAO" "nenhum panico na sequencia N"
}

# ---------------- Sequencia U: USB ----------------
if ($runAll -or $OnlyU) {
    Write-Host "== Sequencia U (USB pelo xHCI, sem PS/2) ==" -ForegroundColor Cyan
    $stepsU = @(
        "cmd:lsusb", "wait:2", "cmd:cat /proc/usb", "wait:1",
        "cmd:evtest 10", "wait:0.8", "mon:sendkey a", "mon:sendkey ctrl", "mon:sendkey up", "mon:mouse_move 10 5",
        "mon:mouse_button 1", "mon:mouse_button 0", "until:20:evento(s)"
    )
    & (Join-Path $Root "test.ps1") -Smp $Smp -Image -NoBuild -Usb -NoPs2 -LogPath "out\reg-u.log" `
        -Script $stepsU -CommandDelay 1.5 -BootTimeout 60 -NoPrint | Out-Null
    $u = Get-Log "out\reg-u.log"
    if ($Verbose) { Write-Output $u }

    Expect $u "usb\s+controlador xHCI 1b36:000d em " "xHCI achado no PCI"
    Expect $u "usb\s+xHCI 1\.\d\d, 8 porta\(s\)" "xHCI reiniciado e ligado, com a versao e as portas"
    Expect $u "usb\s+2 dispositivo\(s\) USB" "dois dispositivos enumerados (Enable Slot e Address Device)"
    Expect $u "(?s)\$ lsusb\s*\n[^\n]*ID 0627:0001 QEMU QEMU USB Keyboard \(USB 2\.00, [a-z ]+speed, classe hid\)" "lsusb mostra o teclado USB com as strings do dispositivo"
    Expect $u "(?s)\$ lsusb.*?ID 0627:0001 QEMU QEMU USB Mouse \(USB 2\.00, [a-z ]+speed, classe hid\)" "lsusb mostra o mouse USB"
    Expect $u "kbd\s+sem controlador PS/2" "a maquina sobe sem o controlador PS/2"
    Expect $u "usb\s+porta \d+: teclado HID pronto, protocolo de boot" "teclado USB configurado (endpoint de interrupcao)"
    Expect $u "usb\s+porta \d+: mouse HID pronto, protocolo de boot" "mouse USB configurado"
    Expect $u "tecla-desce\s+scancode=1e ascii=97 mods=0\s*\n\s*tecla-sobe\s+scancode=1e" "tecla do teclado USB chega ao /dev/input sem PS/2"
    Expect $u "tecla-desce\s+scancode=1d ascii=0 mods=2" "Ctrl do teclado USB vira modificador"
    Expect $u "tecla-desce\s+scancode=48 ascii=0 mods=10" "seta do teclado USB chega como tecla estendida"
    Expect $u "mouse-move\s+a=10 b=5" "movimento do mouse USB chega ao /dev/input"
    Expect $u "mouse-botao\s+a=1 b=0 c=0\s*\n\s*mouse-botao\s+a=0" "botao do mouse USB desce e sobe"
    ExpectNot $u "usb\s+porta \d+: (Enable|Address|descritor)" "nenhuma porta falhou na enumeracao"
    ExpectNot $u "PANIC|EXCECAO|rust: " "nenhum panico na sequencia U"
}

# ---------------- Sequencia E: estresse de processos ----------------
if ($runAll -or $OnlyE) {
    Write-Host "== Sequencia E (estresse) ==" -ForegroundColor Cyan
    $stressCmd = "stress 3000 16 | stress 3000 16 | stress 3000 16 | stress 3000 16"
    for ($i = 1; $i -le $StressRuns; $i++) {
        $stepsE = @("cmd:$stressCmd", 'cmd:echo FIM$X-ESTRESSE', "until:1200:FIM-ESTRESSE",
                    "key:0B", "cmd:procs", "cmd:ps", "wait:1")
        & (Join-Path $Root "test.ps1") -Smp $Smp -Image -NoBuild -LogPath "out\reg-e-$i.log" `
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
