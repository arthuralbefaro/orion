$ErrorActionPreference = "Stop"
$Root = Split-Path $PSScriptRoot -Parent
$Version = "0.16.0"
$Sha256 = "68659eb5f1e4eb1437a722f1dd889c5a322c9954607f5edcf337bc3684a75a7e"
$Name = "zig-x86_64-windows-$Version"
$Dir = Join-Path $Root "third_party\zig"
$Exe = Join-Path $Dir "zig.exe"

if (Test-Path $Exe) {
    $have = (& $Exe version).Trim()
    if ($have -eq $Version) { return $Exe }
}

$Url = "https://ziglang.org/download/$Version/$Name.zip"
$Tmp = Join-Path $env:TEMP "orion-zig"
New-Item -ItemType Directory -Force -Path $Tmp | Out-Null
$Zip = Join-Path $Tmp "$Name.zip"
$ok = (Test-Path $Zip) -and ((Get-FileHash $Zip -Algorithm SHA256).Hash.ToLower() -eq $Sha256)
if (-not $ok) {
    Write-Host ">> Baixando Zig $Version ($Url)" -ForegroundColor Cyan
    $old = $ProgressPreference
    $ProgressPreference = "SilentlyContinue"
    Invoke-WebRequest -Uri $Url -OutFile $Zip
    $ProgressPreference = $old
}
$hash = (Get-FileHash $Zip -Algorithm SHA256).Hash.ToLower()
if ($hash -ne $Sha256) { throw "SHA-256 do Zig nao confere: $hash (esperado $Sha256)" }

$Extract = Join-Path $Tmp "x"
if (Test-Path $Extract) { Remove-Item -Recurse -Force $Extract }
New-Item -ItemType Directory -Force -Path $Extract | Out-Null
& tar.exe -xf $Zip -C $Extract
if ($LASTEXITCODE -ne 0) { throw "falha ao extrair $Zip" }
if (Test-Path $Dir) { Remove-Item -Recurse -Force $Dir }
Move-Item (Join-Path $Extract $Name) $Dir
Write-Host "Zig $Version em third_party\zig" -ForegroundColor Green
return $Exe
