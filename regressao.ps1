param(
    [int]$Smp = 4,
    [switch]$NoBuild,
    [switch]$OnlyA,
    [switch]$OnlyB,
    [switch]$Verbose
)

$ErrorActionPreference = "Stop"
$Root = $PSScriptRoot
$failures = @()
$checks = 0

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

function Expect-MemDelta([string]$log, [string]$what, [int]$max) {
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

$buildFlag = @()
if ($NoBuild) { $buildFlag = @("-NoBuild") }

# ---------------- Sequencia A: texto ----------------
if (-not $OnlyB) {
    Write-Host "== Sequencia A (texto) ==" -ForegroundColor Cyan
    $stepsA = @(
        "key:0B", "cmd:mem", "key:0B", "wait:1",
        "cmd:ls /bin | wc", "wait:1",
        "cmd:memtest", "wait:2",
        "cmd:crash", "wait:2",
        "cmd:hello um dois", "wait:1",
        "cmd:date", "wait:1",
        "cmd:ls /dev", "wait:1",
        "cmd:polltest", "wait:6",
        "cmd:fbtest 2", "wait:1", "shot:out\shots\reg-fbtest.ppm", "wait:2",
        "cmd:evtest 3", "wait:0.8", "mon:sendkey a", "mon:mouse_move 10 5", "wait:4",
        "cmd:ptytest", "wait:8",
        "key:0B", "cmd:heaptest", "wait:1", "cmd:preempt", "wait:2",
        "cmd:dev", "cmd:pty", "cmd:mem"
    )
    & (Join-Path $Root "test.ps1") -Smp $Smp @buildFlag -LogPath "out\reg-a.log" `
        -Script $stepsA -CommandDelay 1.5 -BootTimeout 60 -NoPrint | Out-Null
    $a = Get-Log "out\reg-a.log"
    if ($Verbose) { Write-Output $a }

    Expect $a "Orion sh \(pid \d+\)" "shell de usuario subiu"
    Expect $a "ls /bin \| wc[\s\S]{0,80}?\d+\s+\d+\s+\d+" "ls /bin | wc produziu contagem"
    Expect $a "resultado: TUDO OK" "memtest TUDO OK"
    Expect $a "morto: #PF" "crash morto por page fault"
    Expect $a "hello from ring 3" "hello rodou"
    Expect $a "argv\[2\] = dois" "argumentos chegaram ao hello"
    Expect $a "UTC 2026|UTC 20\d\d" "date imprimiu a data"
    Expect $a "fb0" "ls /dev listou fb0"
    Expect $a "input" "ls /dev listou input"
    Expect $a "polltest: TUDO OK" "polltest TUDO OK"
    Expect $a "fbtest: gradiente \+ retangulo desenhados" "fbtest desenhou"
    Expect $a "tecla-desce" "evtest recebeu tecla"
    Expect $a "mouse-move" "evtest recebeu movimento do mouse"
    Expect $a "\nola" "ptytest imprimiu ola"
    Expect $a "ptytest: sh saiu com 0" "ptytest encerrou o sh"
    Expect $a ": OK" "heaptest OK"
    Expect $a "OK \(as duas rodaram\)" "preempt OK"
    Expect $a "0 pty\(s\) viva\(s\)" "nenhuma pty vazada"
    Expect-MemDelta $a "memoria estavel na sequencia A" 16
}

# ---------------- Sequencia B: grafica ----------------
if (-not $OnlyA) {
    Write-Host "== Sequencia B (grafica) ==" -ForegroundColor Cyan
    $stepsB = @(
        "key:0B", "cmd:mem", "key:0B", "wait:1",
        "cmd:wm", "wait:3", "shot:out\shots\reg-wm-1-inicial.ppm",
        "mon:sendkey l", "mon:sendkey s", "mon:sendkey ret", "wait:1.5",
        "shot:out\shots\reg-wm-2-ls.ppm",
        "mon:mouse_move -140 -332", "wait:0.4", "mon:mouse_button 1", "wait:0.3",
        "mon:mouse_move 90 130", "mon:mouse_move 90 130", "wait:0.4", "mon:mouse_button 0", "wait:1",
        "shot:out\shots\reg-wm-3-arrastada.ppm",
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
    Expect $b "wm: encerrando" "Ctrl\+Alt\+X encerrou o wm"
    Expect $b "TTY-DE-VOLTA\r?\nTTY-DE-VOLTA|echo TTY-DE-VOLTA[\s\S]{0,40}TTY-DE-VOLTA" "TTY respondeu depois do wm"
    Expect $b "0 pty\(s\) viva\(s\)" "nenhuma pty vazada"
    Expect $b "(?s)pid\s+ppid\s+estado[^\n]*\n\s+2\s+1\s+running\s+\d+\s+sh\s*\n\s+1\s+0\s+running\s+\d+\s+init" "procs so tem init e sh"
    Expect-MemDelta $b "memoria estavel na sequencia B" 16
}

Write-Host ""
if ($failures.Count -eq 0) {
    Write-Host "REGRESSAO VERDE ($checks verificacoes)" -ForegroundColor Green
    exit 0
}
Write-Host ("REGRESSAO VERMELHA: " + $failures.Count + " de " + $checks + " falharam") -ForegroundColor Red
foreach ($f in $failures) { Write-Host ("  - " + $f) -ForegroundColor Red }
exit 1
