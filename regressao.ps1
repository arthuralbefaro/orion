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

$buildFlag = @{}
if ($NoBuild) { $buildFlag = @{ NoBuild = $true } }

# ---------------- Sequencia A: texto ----------------
if ($runAll -or $OnlyA) {
    Write-Host "== Sequencia A (texto) ==" -ForegroundColor Cyan
    $stepsA = @(
        "key:0B", "cmd:mem", "key:0B", "wait:1",
        "cmd:ls /bin | wc", "wait:1",
        "cmd:memtest", "wait:2",
        "cmd:crash", "wait:2",
        "cmd:sigtest", "wait:10",
        "cmd:badptr", "until:40:badptr: fim",
        "cmd:fputest", "until:60:fputest: fim", "cmd:auxtest", "until:30:auxtest: fim",
        "cmd:sctest", "until:30:sctest: fim", "cmd:filetest", "until:60:filetest: fim",
        "cmd:sleep 100", "wait:2", "key:03", "wait:2",
        "cmd:hello um dois", "wait:1",
        "cmd:date", "wait:1",
        "cmd:ls /dev", "wait:1",
        "cmd:polltest", "wait:6",
        "cmd:fbtest 2", "wait:1", "shot:out\shots\reg-fbtest.ppm", "wait:2",
        "cmd:evtest 3", "wait:0.8", "mon:sendkey a", "mon:mouse_move 10 5", "wait:4",
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
        "cmd:dev", "cmd:pty", "cmd:mem"
    )
    & (Join-Path $Root "test.ps1") -Smp $Smp @buildFlag -LogPath "out\reg-a.log" `
        -Script $stepsA -CommandDelay 1.5 -BootTimeout 60 -NoPrint | Out-Null
    $a = Get-Log "out\reg-a.log"
    if ($Verbose) { Write-Output $a }

    Expect $a "Orion sh \(pid \d+\)" "shell de usuario subiu"
    Expect $a "ls /bin \| wc[\s\S]{0,80}?\d+\s+\d+\s+\d+" "ls /bin | wc produziu contagem"
    Expect $a "resultado: TUDO OK" "memtest TUDO OK"
    Expect $a "#PF Page Fault" "crash levou page fault"
    Expect $a "morto por sinal 11" "crash morto com SIGSEGV"
    Expect $a "sigtest: TUDO OK" "sigtest TUDO OK"
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
    Expect $a "flag NT ligada dentro do handler\s+ok" "flag NT do usuario nao derruba o iretq do rt_sigreturn"
    Expect $a "Copias de usuario: [1-9]\d* falta\(s\) recuperada\(s\)" "tabela de excecoes recuperou as faltas"
    Expect $a "morto por sinal 2" "Ctrl\+C matou o sleep com SIGINT"
    Expect $a "hello from ring 3" "hello rodou"
    Expect $a "argv\[2\] = dois" "argumentos chegaram ao hello"
    Expect $a "UTC 20\d\d" "date imprimiu a data"
    Expect $a "fb0" "ls /dev listou fb0"
    Expect $a "sda" "ls /dev listou o disco"
    Expect $a "polltest: TUDO OK" "polltest TUDO OK"
    Expect $a "fbtest: gradiente \+ retangulo desenhados" "fbtest desenhou"
    Expect $a "tecla-desce" "evtest recebeu tecla"
    Expect $a "mouse-move" "evtest recebeu movimento do mouse"
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
    Test-MemDelta $a "memoria estavel na sequencia A" 16
}

# ---------------- Sequencia B: grafica ----------------
if ($runAll -or $OnlyB) {
    Write-Host "== Sequencia B (grafica) ==" -ForegroundColor Cyan
    $stepsB = @(
        "key:0B", "cmd:mem", "key:0B", "wait:1",
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
        "key:0B", "cmd:mem", "cmd:lsblk", "cmd:mutex"
    )
    & (Join-Path $Root "test.ps1") -Smp $Smp -NoBuild -Disk -LogPath "out\reg-c.log" `
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
