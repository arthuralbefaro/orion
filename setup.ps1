$ErrorActionPreference = "Continue"
$ProgressPreference = "SilentlyContinue"
$Root = $PSScriptRoot

if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    Write-Host "winget nao encontrado. Instale o 'App Installer' pela Microsoft Store e rode de novo." -ForegroundColor Red
    exit 1
}

$packages = @(
    @{ Id = "LLVM.LLVM";                       Nome = "LLVM (clang + ld.lld)" },
    @{ Id = "SoftwareFreedomConservancy.QEMU"; Nome = "QEMU" },
    @{ Id = "Git.Git";                         Nome = "Git" }
)
foreach ($p in $packages) {
    Write-Host ">> $($p.Nome)  [$($p.Id)]" -ForegroundColor Cyan
    winget install --id $p.Id -e --silent --accept-source-agreements --accept-package-agreements
}

$dirs = @("$env:ProgramFiles\LLVM\bin", "$env:ProgramFiles\qemu")
$userPath = [Environment]::GetEnvironmentVariable("Path", "User")
$parts = @()
if ($userPath) { $parts = @($userPath -split ";" | Where-Object { $_ }) }
$changed = $false
foreach ($d in $dirs) {
    if (-not (Test-Path $d)) {
        Write-Host "aviso: $d nao existe (a instalacao falhou?)" -ForegroundColor Yellow
        continue
    }
    if ($parts -notcontains $d) { $parts += $d; $changed = $true }
    if (($env:Path -split ";") -notcontains $d) { $env:Path += ";$d" }
}
if ($changed) {
    [Environment]::SetEnvironmentVariable("Path", ($parts -join ";"), "User")
    Write-Host "PATH do usuario atualizado (abra um terminal novo para valer em todo lugar)."
}

$LimineVersion = "12.8.0"
$LimineDir = Join-Path $Root "third_party\limine"
$Tmp = Join-Path $env:TEMP "orion-limine"
New-Item -ItemType Directory -Force -Path $LimineDir, $Tmp | Out-Null

$binZip = Join-Path $Tmp "limine-binary.zip"
$srcTgz = Join-Path $Tmp "limine-src.tar.gz"
$base = "https://github.com/Limine-Bootloader/Limine/releases/download/v$LimineVersion"

Write-Host ">> Baixando Limine v$LimineVersion" -ForegroundColor Cyan
Invoke-WebRequest -Uri "$base/limine-binary.zip" -OutFile $binZip
Expand-Archive -Path $binZip -DestinationPath $Tmp -Force
foreach ($f in "BOOTX64.EFI", "BOOTIA32.EFI", "limine-bios.sys", "limine-bios-cd.bin",
               "limine-uefi-cd.bin", "limine.c", "limine-bios-hdd.h", "LICENSE", "Makefile") {
    Copy-Item (Join-Path $Tmp "limine-binary\$f") $LimineDir -Force
}

Invoke-WebRequest -Uri "$base/limine-$LimineVersion.tar.gz" -OutFile $srcTgz
Push-Location $Tmp
tar -xzf $srcTgz "limine-$LimineVersion/limine-protocol/include/limine.h"
Pop-Location
$hdr = Join-Path $Tmp "limine-$LimineVersion\limine-protocol\include\limine.h"
if (Test-Path $hdr) {
    Copy-Item $hdr $LimineDir -Force
} else {
    Write-Host "tar falhou; baixando limine.h direto do repositorio limine-protocol" -ForegroundColor Yellow
    Invoke-WebRequest -Uri "https://raw.githubusercontent.com/limine-bootloader/limine-protocol/trunk/include/limine.h" `
                      -OutFile (Join-Path $LimineDir "limine.h")
}
Set-Content -Path (Join-Path $LimineDir "VERSION") -Value $LimineVersion

Write-Host ""
foreach ($t in "clang", "ld.lld", "qemu-system-x86_64") {
    $c = Get-Command $t -ErrorAction SilentlyContinue
    if ($c) { Write-Host ("ok    {0,-22} {1}" -f $t, $c.Source) -ForegroundColor Green }
    else    { Write-Host ("FALTA {0}" -f $t) -ForegroundColor Red }
}
foreach ($f in "BOOTX64.EFI", "limine.h") {
    if (Test-Path (Join-Path $LimineDir $f)) { Write-Host ("ok    {0}" -f $f) -ForegroundColor Green }
    else { Write-Host ("FALTA third_party\limine\{0}" -f $f) -ForegroundColor Red }
}
Write-Host "`nPronto. Agora: .\build.ps1 -Run"