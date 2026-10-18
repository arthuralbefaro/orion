param(
    [string[]]$Commands = @(),
    [string[]]$MonitorCommands = @(),
    [string]$Screenshot = "",
    [string[]]$Script = @(),
    [int]$Smp = 4,
    [int]$Memory = 512,
    [double]$BootTimeout = 60,
    [double]$CommandDelay = 1.0,
    [double]$Settle = 2.0,
    [int]$SerialPort = 4444,
    [int]$MonitorPort = 4445,
    [string]$LogPath = "out\test-serial.log",
    [switch]$NoBuild,
    [switch]$NoPrint,
    [switch]$KernelShell,
    [switch]$Disk,
    [string[]]$ExtraDisks = @(),
    [switch]$Gui,
    [switch]$Image,
    [string[]]$HostFwd = @()
)

$ErrorActionPreference = "Stop"
$Root = $PSScriptRoot
$OutDir = Join-Path $Root "out"

function Find-Tool([string]$Name, [string[]]$Fallbacks) {
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    foreach ($f in $Fallbacks) { if (Test-Path $f) { return $f } }
    throw "Ferramenta '$Name' nao encontrada."
}

if (-not $NoBuild) {
    & (Join-Path $Root "build.ps1") -Gui:$Gui -Image:$Image | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "build.ps1 falhou" }
}
if ($Image -and -not (Test-Path (Join-Path $OutDir "orion.img"))) { throw "out\orion.img nao existe: rode .\build.ps1 -Image" }

$Qemu = Find-Tool "qemu-system-x86_64.exe" @("$env:ProgramFiles\qemu\qemu-system-x86_64.exe")
$QemuDir = Split-Path $Qemu -Parent
$ovmfCode = Join-Path $QemuDir "share\edk2-x86_64-code.fd"
$ovmfVarsTemplate = Join-Path $QemuDir "share\edk2-i386-vars.fd"
$ovmfVars = Join-Path $OutDir "ovmf-vars-test.fd"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
if (-not (Test-Path $ovmfVars)) { Copy-Item $ovmfVarsTemplate $ovmfVars }


if ($Disk) {
    $img = Join-Path $OutDir "disk.img"
    if (-not (Test-Path $img)) {
        $fs = [System.IO.File]::Create($img)
        $fs.SetLength(64MB)
        $fs.Close()
        Write-Host "Criado out\disk.img (64 MiB, zerado)." -ForegroundColor Yellow
    }
}
Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

function Wait-PortFree([int]$port, [int]$timeoutSec) {
    $end = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $end) {
        try {
            $l = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, $port)
            $l.Start()
            $l.Stop()
            return $true
        } catch {
            Start-Sleep -Milliseconds 300
        }
    }
    return $false
}

Start-Sleep -Milliseconds 500
if (-not (Wait-PortFree $SerialPort 20)) { Write-Host "AVISO: porta $SerialPort ainda ocupada" -ForegroundColor Yellow }
if (-not (Wait-PortFree $MonitorPort 20)) { Write-Host "AVISO: porta $MonitorPort ainda ocupada" -ForegroundColor Yellow }

$qemuArgs = @(
    "-M", "q35", "-cpu", "qemu64", "-smp", "$Smp", "-m", "${Memory}M",
    "-drive", "if=pflash,format=raw,unit=0,file=$ovmfCode,readonly=on",
    "-drive", "if=pflash,format=raw,unit=1,file=$ovmfVars",
    "-drive", $(if ($Image) { "format=raw,file=out/orion.img" } else { "format=raw,file=fat:rw:out/esp" }),
    "-display", "none",
    "-serial", "tcp:127.0.0.1:$SerialPort,server,nowait",
    "-monitor", "tcp:127.0.0.1:$MonitorPort,server,nowait",
    "-no-reboot", "-no-shutdown"
)

$netdev = "user,id=n0"
foreach ($fwd in $HostFwd) { $netdev += ",hostfwd=$fwd" }
$qemuArgs += @("-netdev", $netdev, "-device", "e1000,netdev=n0",
               "-object", "filter-dump,id=d0,netdev=n0,file=out/net.pcap")
if ($Disk) { $qemuArgs += @("-drive", "format=raw,file=out/disk.img") }
foreach ($extra in $ExtraDisks) { $qemuArgs += @("-drive", "format=raw,file=$extra") }

$qemuErr = Join-Path $OutDir "test-qemu.log"
$quoted = $qemuArgs | Where-Object { $_ } | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }
function Start-Qemu {
    $p = Start-Process -FilePath $Qemu -ArgumentList $quoted -PassThru -WindowStyle Hidden `
        -WorkingDirectory $Root -RedirectStandardError $qemuErr
    Write-Host "QEMU pid $($p.Id), smp $Smp" -ForegroundColor DarkGray
    return $p
}
$proc = Start-Qemu

$log = New-Object System.Text.StringBuilder
$client = $null
$monitor = $null

function Connect-Port([int]$port, [double]$timeout) {
    $end = (Get-Date).AddSeconds($timeout)
    while ((Get-Date) -lt $end) {
        try {
            $c = New-Object System.Net.Sockets.TcpClient
            $c.Connect("127.0.0.1", $port)
            $c.NoDelay = $true
            return $c
        } catch { Start-Sleep -Milliseconds 200 }
        if ($proc.HasExited) { break }
    }
    $why = ""
    if (Test-Path $qemuErr) { $why = (Get-Content $qemuErr -Raw) }
    throw "nao foi possivel conectar na porta $port. QEMU: $why"
}

function Pump-Serial([double]$seconds) {
    $stream = $client.GetStream()
    $buf = New-Object byte[] 65536
    $end = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $end) {
        if ($stream.DataAvailable) {
            $n = $stream.Read($buf, 0, $buf.Length)
            if ($n -gt 0) { [void]$log.Append([System.Text.Encoding]::ASCII.GetString($buf, 0, $n)) }
        } else {
            Start-Sleep -Milliseconds 15
        }
    }
}

function Wait-For([string]$needle, [double]$timeout) {
    $stream = $client.GetStream()
    $buf = New-Object byte[] 65536
    $end = (Get-Date).AddSeconds($timeout)
    while ((Get-Date) -lt $end) {
        if ($stream.DataAvailable) {
            $n = $stream.Read($buf, 0, $buf.Length)
            if ($n -gt 0) {
                [void]$log.Append([System.Text.Encoding]::ASCII.GetString($buf, 0, $n))
                if ($log.ToString().Contains($needle)) { return $true }
            }
        } else {
            Start-Sleep -Milliseconds 15
        }
    }
    return $log.ToString().Contains($needle)
}

function Wait-After([string]$needle, [double]$timeout, [int]$from) {
    $stream = $client.GetStream()
    $buf = New-Object byte[] 65536
    $end = (Get-Date).AddSeconds($timeout)
    if ($log.ToString().IndexOf($needle, $from, [System.StringComparison]::Ordinal) -ge 0) { return $true }
    while ((Get-Date) -lt $end) {
        if ($stream.DataAvailable) {
            $n = $stream.Read($buf, 0, $buf.Length)
            if ($n -gt 0) {
                [void]$log.Append([System.Text.Encoding]::ASCII.GetString($buf, 0, $n))
                if ($log.ToString().IndexOf($needle, $from, [System.StringComparison]::Ordinal) -ge 0) { return $true }
            }
        } else {
            Start-Sleep -Milliseconds 50
        }
    }
    return ($log.ToString().IndexOf($needle, $from) -ge 0)
}

function Send-Serial([string]$text) {
    $bytes = [System.Text.Encoding]::ASCII.GetBytes($text)
    $stream = $client.GetStream()
    $stream.Write($bytes, 0, $bytes.Length)
    $stream.Flush()
}

function Send-Bytes([byte[]]$bytes) {
    $stream = $client.GetStream()
    $stream.Write($bytes, 0, $bytes.Length)
    $stream.Flush()
}

function Send-Monitor([string]$cmd) {
    if (-not $monitor) { $script:monitor = Connect-Port $MonitorPort 10 }
    $stream = $monitor.GetStream()
    $bytes = [System.Text.Encoding]::ASCII.GetBytes($cmd + "`n")
    $stream.Write($bytes, 0, $bytes.Length)
    $stream.Flush()
    Start-Sleep -Milliseconds 200
    $buf = New-Object byte[] 8192
    while ($stream.DataAvailable) { [void]$stream.Read($buf, 0, $buf.Length) }
}

function Convert-Ppm([string]$ppm, [string]$png) {
    $py = Get-Command python -ErrorAction SilentlyContinue
    if ($py) {
        $code = "from PIL import Image; Image.open(r'$ppm').save(r'$png')"
        & $py.Source -c $code 2>&1 | Out-Null
        if (Test-Path $png) { return $true }
    }
    Add-Type -AssemblyName System.Drawing
    $bytes = [System.IO.File]::ReadAllBytes($ppm)
    $pos = 0
    $fields = @()
    while ($fields.Count -lt 4) {
        while ($pos -lt $bytes.Length -and ($bytes[$pos] -eq 32 -or $bytes[$pos] -eq 10 -or $bytes[$pos] -eq 13 -or $bytes[$pos] -eq 9)) { $pos++ }
        if ($bytes[$pos] -eq 35) { while ($pos -lt $bytes.Length -and $bytes[$pos] -ne 10) { $pos++ }; continue }
        $start = $pos
        while ($pos -lt $bytes.Length -and -not ($bytes[$pos] -eq 32 -or $bytes[$pos] -eq 10 -or $bytes[$pos] -eq 13 -or $bytes[$pos] -eq 9)) { $pos++ }
        $fields += [System.Text.Encoding]::ASCII.GetString($bytes, $start, $pos - $start)
    }
    $pos++
    $w = [int]$fields[1]
    $h = [int]$fields[2]
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $rect = New-Object System.Drawing.Rectangle(0, 0, $w, $h)
    $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::WriteOnly, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $row = New-Object byte[] ($w * 3)
    for ($y = 0; $y -lt $h; $y++) {
        for ($x = 0; $x -lt $w; $x++) {
            $src = $pos + ($y * $w + $x) * 3
            $row[$x * 3 + 0] = $bytes[$src + 2]
            $row[$x * 3 + 1] = $bytes[$src + 1]
            $row[$x * 3 + 2] = $bytes[$src + 0]
        }
        [System.Runtime.InteropServices.Marshal]::Copy($row, 0, [IntPtr]($data.Scan0.ToInt64() + $y * $data.Stride), $row.Length)
    }
    $bmp.UnlockBits($data)
    $bmp.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    return (Test-Path $png)
}

function Take-Screenshot([string]$path) {
    if (-not [System.IO.Path]::IsPathRooted($path)) { $path = Join-Path $Root $path }
    $dir = Split-Path $path -Parent
    if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    if (Test-Path $path) { Remove-Item $path -Force }
    Send-Monitor ("screendump " + $path.Replace('\', '/'))
    $end = (Get-Date).AddSeconds(10)
    while ((Get-Date) -lt $end -and -not (Test-Path $path)) { Start-Sleep -Milliseconds 150 }
    if (-not (Test-Path $path)) { Write-Host "  screenshot FALHOU: $path" -ForegroundColor Red; return }
    Start-Sleep -Milliseconds 400
    $png = [System.IO.Path]::ChangeExtension($path, ".png")
    if (Convert-Ppm $path $png) {
        Write-Host "  screenshot: $png" -ForegroundColor DarkCyan
    } else {
        Write-Host "  conversao PPM->PNG falhou ($path)" -ForegroundColor Red
    }
}

$exitCode = 0
$cmdPos = 0
try {
    $client = Connect-Port $SerialPort $BootTimeout

    $steps = @()
    if ($KernelShell) { $steps += "key:0B" }
    foreach ($c in $Commands) { $steps += "cmd:$c" }
    foreach ($m in $MonitorCommands) { $steps += "mon:$m" }
    foreach ($s in $Script) { $steps += $s }
    if ($Screenshot) { $steps += "wait:$Settle"; $steps += "shot:$Screenshot" }

    $needPrompt = $false
    foreach ($s in $steps) { if ($s -like "cmd:*" -or $s -like "raw:*" -or $s -like "key:*" -or $s -like "keys:*") { $needPrompt = $true } }

    if ($needPrompt -or $steps.Count -eq 0) {
        if (Wait-For '$ ' $BootTimeout) {
            Write-Host "prompt do shell de usuario detectado" -ForegroundColor DarkGray
        } else {
            Write-Host "AVISO: prompt do shell nao apareceu em $BootTimeout s" -ForegroundColor Yellow
        }
    } else {
        Pump-Serial ([Math]::Min($BootTimeout, 12))
    }

    foreach ($step in $steps) {
        $kind = $step
        $arg = ""
        $i = $step.IndexOf(':')
        if ($i -ge 0) { $kind = $step.Substring(0, $i); $arg = $step.Substring($i + 1) }
        switch ($kind) {
            "cmd"  { Write-Host "  cmd  > $arg" -ForegroundColor DarkGray; $cmdPos = $log.Length; Send-Serial ($arg + "`r"); Pump-Serial $CommandDelay }
            "raw"  { Write-Host "  raw  > $arg" -ForegroundColor DarkGray; Send-Serial $arg; Pump-Serial 0.3 }
            "key"  { Write-Host "  key  > $arg" -ForegroundColor DarkGray; Send-Bytes @([byte][Convert]::ToInt32($arg, 16)); Pump-Serial 0.6 }
            "keys" {
                Write-Host "  keys > $arg" -ForegroundColor DarkGray
                $bytes = @()
                for ($k = 0; $k + 1 -lt $arg.Length; $k += 2) { $bytes += [byte][Convert]::ToInt32($arg.Substring($k, 2), 16) }
                Send-Bytes $bytes
                Pump-Serial 0.6
            }
            "mon"  { Write-Host "  mon  > $arg" -ForegroundColor DarkGray; Send-Monitor $arg; Pump-Serial 0.35 }
            "wait" { Pump-Serial ([double]$arg) }
            "until" {
                $j = $arg.IndexOf(':')
                $secs = [double]$arg.Substring(0, $j)
                $needle = $arg.Substring($j + 1)
                Write-Host "  until> $needle ($secs s)" -ForegroundColor DarkGray
                if (-not (Wait-After $needle $secs $cmdPos)) {
                    Write-Host "  AVISO: '$needle' nao apareceu em $secs s" -ForegroundColor Yellow
                }
            }
            "shot" { Pump-Serial 0.4; Take-Screenshot $arg }
            "tcpprobe" {
                $port = [int]$arg
                $tc = New-Object System.Net.Sockets.TcpClient
                $res = "recusada"
                try {
                    $iar = $tc.BeginConnect("127.0.0.1", $port, $null, $null)
                    if (-not $iar.AsyncWaitHandle.WaitOne(3000)) {
                        $res = "sem resposta ao connect"
                    } else {
                        $tc.EndConnect($iar)
                        $st = $tc.GetStream()
                        $st.ReadTimeout = 4000
                        $buf = New-Object byte[] 64
                        try {
                            $n = $st.Read($buf, 0, $buf.Length)
                            $res = if ($n -eq 0) { "fechada pelo Orion" } else { "aberta, $n byte(s)" }
                        } catch {
                            $se = $_.Exception.InnerException
                            if ($se -is [System.Net.Sockets.SocketException] -and $se.SocketErrorCode -eq [System.Net.Sockets.SocketError]::TimedOut) {
                                $res = "aberta, sem dados"
                            } else {
                                $res = "fechada pelo Orion"
                            }
                        }
                    }
                } catch {
                    $res = "recusada"
                } finally {
                    $tc.Close()
                }
                Write-Host "  tcpprobe $port`: $res" -ForegroundColor DarkGray
                [void]$log.Append("`n[tcpprobe] porta $port`: $res`n")
                Pump-Serial 0.5
            }
            "reboot" {
                Write-Host "  reboot" -ForegroundColor DarkGray
                Pump-Serial 0.5
                try { Send-Monitor "quit" } catch {}
                try { $client.Close() } catch {}
                try { if ($monitor) { $monitor.Close() } } catch {}
                $monitor = $null
                if (-not $proc.WaitForExit(10000)) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
                [void](Wait-PortFree $SerialPort 20)
                [void](Wait-PortFree $MonitorPort 20)
                [void]$log.Append("`n----- reboot -----`n")
                $from = $log.Length
                $proc = Start-Qemu
                $client = Connect-Port $SerialPort $BootTimeout
                if (Wait-After '$ ' $BootTimeout $from) {
                    Write-Host "prompt do shell de usuario detectado" -ForegroundColor DarkGray
                } else {
                    Write-Host "AVISO: prompt do shell nao apareceu em $BootTimeout s depois do reboot" -ForegroundColor Yellow
                }
                $cmdPos = $log.Length
            }
            default { Write-Host "  passo desconhecido: $step" -ForegroundColor Yellow }
        }
    }

    Pump-Serial $Settle
} catch {
    Write-Host "ERRO: $_" -ForegroundColor Red
    $exitCode = 1
} finally {
    try { if ($monitor) { Send-Monitor "quit" } } catch {}
    Start-Sleep -Milliseconds 300
    try { if ($client) { $client.Close() } } catch {}
    try { if ($monitor) { $monitor.Close() } } catch {}
    if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
    Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}

$logFull = Join-Path $Root $LogPath
$logDir = Split-Path $logFull -Parent
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Force -Path $logDir | Out-Null }
$text = $log.ToString() -replace "`r`n", "`n" -replace "`r", "`n"
Set-Content -Path $logFull -Value $text -Encoding utf8
if (-not $NoPrint) {
    Write-Host "----- $LogPath -----" -ForegroundColor DarkGray
    Write-Output $text
}
exit $exitCode
