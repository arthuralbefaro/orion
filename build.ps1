param(
    [switch]$Run,
    [switch]$Clean,
    [switch]$Gdb,
    [int]$Memory = 512,
    [int]$Smp = 1,
    [string]$Cpu = "qemu64,+smep,+smap",
    [string]$Accel = "",
    [switch]$Disk,
    [switch]$Gui,
    [switch]$Image,
    [switch]$Iso,
    [switch]$Bios
)

$ErrorActionPreference = "Stop"
$Root      = $PSScriptRoot
$Kernel    = "orion"
$SrcDir    = Join-Path $Root "kernel\src"
$IncDir    = Join-Path $Root "kernel\include"
$LimineDir = Join-Path $Root "third_party\limine"
$UserDir   = Join-Path $Root "user"
$BuildDir  = Join-Path $Root "build"
$OutDir    = Join-Path $Root "out"
$EspDir    = Join-Path $OutDir "esp"
$KernelElf = Join-Path $OutDir "$Kernel.elf"

function Find-Tool([string]$Name, [string[]]$Fallbacks) {
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    foreach ($f in $Fallbacks) { if (Test-Path $f) { return $f } }
    throw "Ferramenta '$Name' nao encontrada. Rode .\setup.ps1 (ou instale e adicione ao PATH)."
}

if ($Clean) {
    Remove-Item -Recurse -Force $BuildDir, $OutDir -ErrorAction SilentlyContinue
    Write-Host "Limpo."
    if (-not $Run) { exit 0 }
}

if (-not (Test-Path (Join-Path $LimineDir "limine.h")) -or -not (Test-Path (Join-Path $LimineDir "BOOTX64.EFI"))) {
    throw "third_party\limine incompleto. Rode .\setup.ps1 primeiro."
}

$Clang = Find-Tool "clang.exe"  @("$env:ProgramFiles\LLVM\bin\clang.exe")
$Lld   = Find-Tool "ld.lld.exe" @("$env:ProgramFiles\LLVM\bin\ld.lld.exe")

$CFlags = @(
    "--target=x86_64-unknown-none-elf", "-std=gnu11", "-O2", "-g", "-Wall", "-Wextra",
    "-ffreestanding", "-nostdlibinc", "-fno-stack-protector", "-fno-stack-check", "-fno-lto",
    "-fno-pic", "-fno-pie", "-ffunction-sections", "-fdata-sections",
    "-m64", "-march=x86-64", "-mabi=sysv", "-mno-80387", "-mno-mmx", "-mno-sse", "-mno-sse2",
    "-mno-red-zone", "-mcmodel=kernel",
    "-I$IncDir", "-I$LimineDir"
)
$LdFlags = @(
    "-m", "elf_x86_64", "-nostdlib", "-static", "-z", "max-page-size=0x1000",
    "-z", "noexecstack", "--gc-sections", "-T", (Join-Path $Root "linker.ld")
)

$sources = Get-ChildItem -Path $SrcDir -Recurse -File | Where-Object { $_.Extension -in @(".c", ".S") }
$headers = Get-ChildItem -Path $IncDir, $LimineDir -Recurse -File -Filter *.h
$newestHeader = ($headers | Sort-Object LastWriteTime -Descending | Select-Object -First 1).LastWriteTime

$objects = @()
$compiled = 0
$staleSet = @{}
foreach ($src in $sources) {
    $rel = $src.FullName.Substring($SrcDir.Length + 1)
    $obj = Join-Path $BuildDir ($rel + ".o")
    $stale = -not (Test-Path $obj)
    if (-not $stale) {
        $objTime = (Get-Item $obj).LastWriteTime
        $stale = ($objTime -lt $src.LastWriteTime) -or ($objTime -lt $newestHeader)
    }
    if ($stale) { $staleSet[$rel] = $true }
}
if ($staleSet.Count -gt 0) { $staleSet["kernel\main.c"] = $true }
foreach ($src in $sources) {
    $rel = $src.FullName.Substring($SrcDir.Length + 1)
    $obj = Join-Path $BuildDir ($rel + ".o")
    $objDir = Split-Path $obj -Parent
    if (-not (Test-Path $objDir)) { New-Item -ItemType Directory -Path $objDir -Force | Out-Null }
    if ($staleSet.ContainsKey($rel)) {
        Write-Host "  CC   $rel"
        & $Clang @CFlags -c $src.FullName -o $obj
        if ($LASTEXITCODE -ne 0) { throw "Erro compilando $rel" }
        $compiled++
    }
    $objects += $obj
}

$RustDir = Join-Path $Root "kernel\rust"
foreach ($crate in Get-ChildItem $RustDir -Directory -ErrorAction SilentlyContinue) {
    $manifest = Join-Path $crate.FullName "Cargo.toml"
    if (-not (Test-Path $manifest)) { continue }
    if (-not (Select-String -Path $manifest -Pattern 'staticlib' -Quiet)) { continue }
    $Cargo = Find-Tool "cargo.exe" @("$env:USERPROFILE\.cargo\bin\cargo.exe")
    $rustTarget = Join-Path $BuildDir "rust"
    $prevFlags = $env:RUSTFLAGS
    $env:RUSTFLAGS = "-C code-model=kernel -C relocation-model=static -D warnings"
    try {
        & $Cargo build --release --quiet --target x86_64-unknown-none --manifest-path $manifest --target-dir $rustTarget
        $cargoExit = $LASTEXITCODE
    } finally {
        $env:RUSTFLAGS = $prevFlags
    }
    if ($cargoExit -ne 0) { throw "Erro compilando o Rust de $($crate.Name)" }
    $libName = "lib" + ((Select-String -Path $manifest -Pattern '^name\s*=\s*"([^"]+)"').Matches[0].Groups[1].Value) + ".a"
    $objects += (Join-Path $rustTarget "x86_64-unknown-none\release\$libName")
}

New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
Write-Host "  LD   out\$Kernel.elf"
& $Lld @LdFlags -o $KernelElf @objects
if ($LASTEXITCODE -ne 0) { throw "Erro no link do kernel" }

$userBins = @{}

$MuslDir = Join-Path $UserDir "musl"
if (Test-Path $MuslDir) {
    $Zig = & (Join-Path $Root "tools\zig.ps1")
    $MuslFlags = @("cc", "-target", "x86_64-linux-musl", "-static", "-O2", "-s", "-Wall", "-Wextra",
                   "-Wno-unused-parameter", ("-I" + (Join-Path $UserDir "include")))
    $muslLinks = @{}
    $muslInstalls = @()
    $PyCmd = $null
    foreach ($prog in Get-ChildItem $MuslDir -Directory) {
        $mout = Join-Path $OutDir "$($prog.Name).elf"
        $spec = Join-Path $prog.FullName "build.txt"
        $genDir = Join-Path $OutDir "gen\$($prog.Name)"
        $msrcs = @()
        $mextra = @()
        $mdeps = @()
        if (Test-Path $spec) {
            foreach ($line in Get-Content $spec) {
                $t = $line.Trim()
                if (-not $t) { continue }
                if ($t.StartsWith("gen ")) {
                    $script = Join-Path $Root $t.Substring(4).Trim()
                    if (-not $PyCmd) { $PyCmd = & (Join-Path $Root "tools\python.ps1") }
                    $pyArgs = @($PyCmd | Select-Object -Skip 1)
                    New-Item -ItemType Directory -Force -Path $genDir | Out-Null
                    & $PyCmd[0] @pyArgs $script $genDir $Zig
                    if ($LASTEXITCODE -ne 0) { throw "Gerador $script de $($prog.Name) falhou" }
                    $mdeps += Get-Item $script
                } elseif ($t.StartsWith("gensrc ")) {
                    $pattern = Join-Path $genDir $t.Substring(7).Trim()
                    $found = @(Get-ChildItem -Path $pattern -File -ErrorAction SilentlyContinue)
                    if ($found.Count -eq 0) { throw "build.txt de $($prog.Name): nada gerado em $pattern" }
                    $msrcs += $found
                } elseif ($t.StartsWith("src ")) {
                    $pattern = Join-Path $prog.FullName $t.Substring(4).Trim()
                    $found = @(Get-ChildItem -Path $pattern -File -ErrorAction SilentlyContinue)
                    if ($found.Count -eq 0) { throw "build.txt de $($prog.Name): nada em $pattern" }
                    $msrcs += $found
                } elseif ($t.StartsWith("flag ")) {
                    foreach ($f in ($t.Substring(5).Trim() -split '\s+')) {
                        $f = $f.Replace("@gen", $genDir).Replace("@root", $Root)
                        if ($f.StartsWith("-I") -and -not [System.IO.Path]::IsPathRooted($f.Substring(2))) {
                            $f = "-I" + (Join-Path $prog.FullName $f.Substring(2))
                        }
                        $mextra += $f
                    }
                } elseif ($t.StartsWith("dep ")) {
                    $mdeps += @(Get-ChildItem -Path (Join-Path $prog.FullName $t.Substring(4).Trim()) -File -Recurse -ErrorAction SilentlyContinue)
                } elseif ($t.StartsWith("link ")) {
                    foreach ($n in ($t.Substring(5).Trim() -split '\s+')) { $muslLinks[$n] = $prog.Name }
                } elseif ($t.StartsWith("install ")) {
                    $parts = $t.Substring(8).Trim() -split '\s+'
                    $muslInstalls += , @((Join-Path $genDir $parts[0]), $parts[1])
                }
            }
        }
        $msrcs += @(Get-ChildItem $prog.FullName -Filter *.c -File)
        $mdeps += @(Get-ChildItem $prog.FullName -Filter *.h -File)
        $mdeps += @(Get-ChildItem (Join-Path $UserDir "include") -Filter *.h -File -ErrorAction SilentlyContinue)
        if (Test-Path $genDir) { $mdeps += @(Get-ChildItem $genDir -Filter *.h -File) }
        $mdeps += Get-Item $spec -ErrorAction SilentlyContinue
        $newest = ($msrcs + $mdeps | Where-Object { $_ } | Sort-Object LastWriteTime -Descending | Select-Object -First 1).LastWriteTime
        if (-not (Test-Path $mout) -or (Get-Item $mout).LastWriteTime -lt $newest) {
            Write-Host "  ZIG  $($prog.Name) ($($msrcs.Count) arquivo(s))"
            & $Zig @MuslFlags @mextra -o $mout @($msrcs | ForEach-Object { $_.FullName })
            if ($LASTEXITCODE -ne 0) { throw "Erro compilando $($prog.Name) contra a musl" }
        }
        $userBins[$prog.Name] = $mout
    }
    foreach ($n in $muslLinks.Keys) {
        if (-not $userBins.ContainsKey($n)) { $userBins[$n] = $userBins[$muslLinks[$n]] }
    }
}

New-Item -ItemType Directory -Force -Path (Join-Path $EspDir "EFI\BOOT"), (Join-Path $EspDir "boot\limine"), (Join-Path $EspDir "bin"), (Join-Path $EspDir "dev"), (Join-Path $EspDir "mnt"), (Join-Path $EspDir "tmp"), (Join-Path $EspDir "run"), (Join-Path $EspDir "proc"), (Join-Path $EspDir "home\orion") | Out-Null
Copy-Item (Join-Path $LimineDir "BOOTX64.EFI") (Join-Path $EspDir "EFI\BOOT\BOOTX64.EFI") -Force
Copy-Item $KernelElf (Join-Path $EspDir "boot\$Kernel.elf") -Force
foreach ($name in $userBins.Keys) { Copy-Item $userBins[$name] (Join-Path $EspDir "bin\$name") -Force }
foreach ($old in Get-ChildItem (Join-Path $EspDir "bin") -File) {
    if (-not $userBins.ContainsKey($old.Name)) { Remove-Item $old.FullName -Force }
}
foreach ($inst in $muslInstalls) {
    $from = (Resolve-Path $inst[0]).Path
    $to = if ($inst[1] -eq ".") { $EspDir } else { Join-Path $EspDir $inst[1] }
    $manifest = $from.TrimEnd('\') + ".installed"
    $now = @{}
    foreach ($f in Get-ChildItem $from -Recurse -File) {
        $rel = $f.FullName.Substring($from.Length).TrimStart('\')
        $now[$rel] = $true
        $dest = Join-Path $to $rel
        if ((Test-Path $dest) -and (Get-Item $dest).Length -eq $f.Length -and (Get-Item $dest).LastWriteTime -ge $f.LastWriteTime) { continue }
        New-Item -ItemType Directory -Force -Path (Split-Path $dest -Parent) | Out-Null
        Copy-Item $f.FullName $dest -Force
    }
    if (Test-Path $manifest) {
        foreach ($rel in Get-Content $manifest) {
            if ($rel -and -not $now.ContainsKey($rel)) { Remove-Item (Join-Path $to $rel) -Force -ErrorAction SilentlyContinue }
        }
    }
    Set-Content -Path $manifest -Value ($now.Keys | Sort-Object) -Encoding ascii
}
if ($userBins.ContainsKey("doom")) {
    $wad = $null
    try { $wad = & (Join-Path $Root "tools\freedoom.ps1") } catch { Write-Warning "sem o freedoom1.wad ($_); o doom fica sem IWAD" }
    if ($wad) {
        $wadDir = Join-Path $EspDir "doom"
        $wadOut = Join-Path $wadDir "freedoom1.wad"
        New-Item -ItemType Directory -Force -Path $wadDir | Out-Null
        if (-not (Test-Path $wadOut) -or (Get-Item $wadOut).Length -ne (Get-Item $wad).Length) { Copy-Item $wad $wadOut -Force }
    }
}
Copy-Item (Join-Path $Root "limine.conf") (Join-Path $EspDir "boot\limine\limine.conf") -Force

New-Item -ItemType Directory -Force -Path (Join-Path $EspDir "etc") | Out-Null
$autostart = if ($Gui) { "/bin/netup`nshell /bin/wm" } else { (Get-Content (Join-Path $Root "etc\autostart") -Raw) }
Set-Content -Path (Join-Path $EspDir "etc\autostart") -Value $autostart -Encoding ascii -NoNewline:$false
foreach ($name in @("passwd", "group", "shadow", "resolv.conf", "network")) { Copy-Item (Join-Path $Root "etc\$name") (Join-Path $EspDir "etc\$name") -Force }
Write-Host "OK: out\$Kernel.elf ($compiled arquivo(s) do kernel recompilado(s)), $($userBins.Count) programa(s) em esp\bin" -ForegroundColor Green

if ($Image) {
    if (-not $PyCmd) { $PyCmd = & (Join-Path $Root "tools\python.ps1") }
    $pyArgs = @($PyCmd | Select-Object -Skip 1)
    & $PyCmd[0] @pyArgs (Join-Path $Root "tools\mkimage.py") (Join-Path $OutDir "orion.img") --esp $EspDir
    if ($LASTEXITCODE -ne 0) { throw "mkimage falhou" }
}

if ($Iso) {
    $ToolDir = Join-Path $OutDir "tools"
    New-Item -ItemType Directory -Force -Path $ToolDir | Out-Null
    if (-not $Zig) { $Zig = & (Join-Path $Root "tools\zig.ps1") }
    $hostTools = @(
        @{ Exe = "mkiso.exe"; Src = (Join-Path $Root "tools\mkiso.c"); Flags = @("-Wall", "-Wextra", "-Werror") },
        @{ Exe = "limine.exe"; Src = (Join-Path $LimineDir "limine.c"); Flags = @("-D_FILE_OFFSET_BITS=64") }
    )
    foreach ($t in $hostTools) {
        $exe = Join-Path $ToolDir $t.Exe
        if (-not (Test-Path $exe) -or (Get-Item $exe).LastWriteTime -lt (Get-Item $t.Src).LastWriteTime) {
            Write-Host "  ZIG  $($t.Exe) (ferramenta do hospedeiro)"
            $flags = $t.Flags
            & $Zig cc -O2 -std=c99 @flags -o $exe $t.Src
            if ($LASTEXITCODE -ne 0) { throw "Erro compilando $($t.Exe)" }
        }
    }
    $IsoDir = Join-Path $OutDir "iso"
    Remove-Item -Recurse -Force $IsoDir -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path (Join-Path $IsoDir "boot\limine") | Out-Null
    Copy-Item $KernelElf (Join-Path $IsoDir "boot\$Kernel.elf")
    if (-not $PyCmd) { $PyCmd = & (Join-Path $Root "tools\python.ps1") }
    $pyArgs = @($PyCmd | Select-Object -Skip 1)
    & $PyCmd[0] @pyArgs (Join-Path $Root "tools\mkinitrd.py") (Join-Path $IsoDir "boot\initramfs.cpio") --esp $EspDir
    if ($LASTEXITCODE -ne 0) { throw "mkinitrd falhou" }
    $conf = (Get-Content (Join-Path $Root "limine.conf") -Raw).TrimEnd() + "`n    module_path: boot():/boot/initramfs.cpio`n"
    Set-Content -Path (Join-Path $IsoDir "boot\limine\limine.conf") -Value $conf -Encoding ascii -NoNewline
    foreach ($f in "limine-bios.sys", "limine-bios-cd.bin", "limine-uefi-cd.bin") {
        Copy-Item (Join-Path $LimineDir $f) (Join-Path $IsoDir "boot\limine\$f")
    }
    $IsoFile = Join-Path $OutDir "orion.iso"
    & (Join-Path $ToolDir "mkiso.exe") $IsoDir $IsoFile --bios boot/limine/limine-bios-cd.bin --efi boot/limine/limine-uefi-cd.bin --label ORION
    if ($LASTEXITCODE -ne 0) { throw "mkiso falhou" }
    & (Join-Path $ToolDir "limine.exe") bios-install --quiet $IsoFile
    if ($LASTEXITCODE -ne 0) { throw "limine bios-install falhou" }
}

if ($Run) {

if ($Disk) {
    $img = Join-Path $OutDir "disk.img"
    if (-not (Test-Path $img)) {
        New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
        $fs = [System.IO.File]::Create($img)
        $fs.SetLength(64MB)
        $fs.Close()
        Write-Host "Criado out\disk.img (64 MiB, zerado). Formate com 'mkfs /dev/sdb' no Orion." -ForegroundColor Yellow
    }
}
    $Qemu = Find-Tool "qemu-system-x86_64.exe" @("$env:ProgramFiles\qemu\qemu-system-x86_64.exe")
    $QemuDir = Split-Path $Qemu -Parent
    $ovmfCode = Join-Path $QemuDir "share\edk2-x86_64-code.fd"
    $ovmfVarsTemplate = Join-Path $QemuDir "share\edk2-i386-vars.fd"
    if (-not (Test-Path $ovmfCode)) { throw "Firmware UEFI nao encontrado: $ovmfCode" }
    $ovmfVars = Join-Path $OutDir "ovmf-vars.fd"
    if (-not (Test-Path $ovmfVars)) { Copy-Item $ovmfVarsTemplate $ovmfVars }
    if ($Iso) {
        $ovmfVars = Join-Path $OutDir "ovmf-vars-iso.fd"
        Copy-Item $ovmfVarsTemplate $ovmfVars -Force
    }

    $qemuArgs = @(
        "-M", "q35",
        "-cpu", $Cpu,
        "-smp", "$Smp",
        "-m", "${Memory}M",
        "-serial", "stdio",
        "-no-reboot", "-no-shutdown"
    )
    if (-not $Bios) {
        $qemuArgs += @("-drive", "if=pflash,format=raw,unit=0,file=$ovmfCode,readonly=on",
                       "-drive", "if=pflash,format=raw,unit=1,file=$ovmfVars")
    }
    if ($Iso) { $qemuArgs += @("-cdrom", "out/orion.iso") }
    elseif ($Image) { $qemuArgs += @("-drive", "format=raw,file=out/orion.img") }
    else { $qemuArgs += @("-drive", "format=raw,file=fat:rw:out/esp") }
    $qemuArgs += @("-netdev", "user,id=n0", "-device", "e1000,netdev=n0")
    if ($Disk) { $qemuArgs += @("-drive", "format=raw,file=out/disk.img") }
    if ($Accel) { $qemuArgs += @("-accel", $Accel) }
    if ($Gdb)   { $qemuArgs += @("-s", "-S"); Write-Host "QEMU parado; conecte o debugger em localhost:1234" -ForegroundColor Yellow }

    Write-Host "Rodando QEMU (a saida da serial aparece aqui; feche a janela do QEMU para sair)..."
    Push-Location $Root
    try { & $Qemu @qemuArgs } finally { Pop-Location }
}
