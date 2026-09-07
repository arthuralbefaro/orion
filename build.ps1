param(
    [switch]$Run,
    [switch]$Clean,
    [switch]$Gdb,
    [int]$Memory = 512,
    [string]$Cpu = "qemu64",
    [string]$Accel = ""
)

$ErrorActionPreference = "Stop"
$Root      = $PSScriptRoot
$Kernel    = "orion"
$SrcDir    = Join-Path $Root "kernel\src"
$IncDir    = Join-Path $Root "kernel\include"
$LimineDir = Join-Path $Root "third_party\limine"
$BuildDir  = Join-Path $Root "build"
$OutDir    = Join-Path $Root "out"
$EspDir    = Join-Path $OutDir "esp"
$Elf       = Join-Path $OutDir "$Kernel.elf"

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
foreach ($src in $sources) {
    $rel = $src.FullName.Substring($SrcDir.Length + 1)
    $obj = Join-Path $BuildDir ($rel + ".o")
    $objDir = Split-Path $obj -Parent
    if (-not (Test-Path $objDir)) { New-Item -ItemType Directory -Path $objDir -Force | Out-Null }

    $stale = -not (Test-Path $obj)
    if (-not $stale) {
        $objTime = (Get-Item $obj).LastWriteTime
        $stale = ($objTime -lt $src.LastWriteTime) -or ($objTime -lt $newestHeader)
    }
    if ($stale) {
        Write-Host "  CC   $rel"
        & $Clang @CFlags -c $src.FullName -o $obj
        if ($LASTEXITCODE -ne 0) { throw "Erro compilando $rel" }
        $compiled++
    }
    $objects += $obj
}

New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
Write-Host "  LD   out\$Kernel.elf"
& $Lld @LdFlags -o $Elf @objects
if ($LASTEXITCODE -ne 0) { throw "Erro no link" }

New-Item -ItemType Directory -Force -Path (Join-Path $EspDir "EFI\BOOT"), (Join-Path $EspDir "boot\limine") | Out-Null
Copy-Item (Join-Path $LimineDir "BOOTX64.EFI") (Join-Path $EspDir "EFI\BOOT\BOOTX64.EFI") -Force
Copy-Item $Elf (Join-Path $EspDir "boot\$Kernel.elf") -Force
Copy-Item (Join-Path $Root "limine.conf") (Join-Path $EspDir "boot\limine\limine.conf") -Force
Write-Host "OK: out\$Kernel.elf ($compiled arquivo(s) recompilado(s))" -ForegroundColor Green

if ($Run) {
    $Qemu = Find-Tool "qemu-system-x86_64.exe" @("$env:ProgramFiles\qemu\qemu-system-x86_64.exe")
    $QemuDir = Split-Path $Qemu -Parent
    $ovmfCode = Join-Path $QemuDir "share\edk2-x86_64-code.fd"
    $ovmfVarsTemplate = Join-Path $QemuDir "share\edk2-i386-vars.fd"
    if (-not (Test-Path $ovmfCode)) { throw "Firmware UEFI nao encontrado: $ovmfCode" }
    $ovmfVars = Join-Path $OutDir "ovmf-vars.fd"
    if (-not (Test-Path $ovmfVars)) { Copy-Item $ovmfVarsTemplate $ovmfVars }

    $qemuArgs = @(
        "-M", "q35",
        "-cpu", $Cpu,
        "-m", "${Memory}M",
        "-drive", "if=pflash,format=raw,unit=0,file=$ovmfCode,readonly=on",
        "-drive", "if=pflash,format=raw,unit=1,file=$ovmfVars",
        "-drive", "format=raw,file=fat:rw:out/esp",
        "-serial", "stdio",
        "-no-reboot", "-no-shutdown"
    )
    if ($Accel) { $qemuArgs += @("-accel", $Accel) }
    if ($Gdb)   { $qemuArgs += @("-s", "-S"); Write-Host "QEMU parado; conecte o debugger em localhost:1234" -ForegroundColor Yellow }

    Write-Host "Rodando QEMU (a saida da serial aparece aqui; feche a janela do QEMU para sair)..."
    Push-Location $Root
    try { & $Qemu @qemuArgs } finally { Pop-Location }
}