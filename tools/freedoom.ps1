$ErrorActionPreference = "Stop"
$Root = Split-Path $PSScriptRoot -Parent
$Version = "0.13.0"
$Sha256 = "3f9b264f3e3ce503b4fb7f6bdcb1f419d93c7b546f4df3e874dd878db9688f59"
$WadSha256 = "7323bcc168c5a45ff10749b339960e98314740a734c30d4b9f3337001f9e703d"
$Name = "freedoom-$Version"
$Dir = Join-Path $Root "third_party\freedoom"
$Wad = Join-Path $Dir "freedoom1.wad"

if ((Test-Path $Wad) -and ((Get-FileHash $Wad -Algorithm SHA256).Hash.ToLower() -eq $WadSha256)) { return $Wad }

$Url = "https://github.com/freedoom/freedoom/releases/download/v$Version/$Name.zip"
$Tmp = Join-Path $env:TEMP "orion-freedoom"
New-Item -ItemType Directory -Force -Path $Tmp | Out-Null
$Zip = Join-Path $Tmp "$Name.zip"
$ok = (Test-Path $Zip) -and ((Get-FileHash $Zip -Algorithm SHA256).Hash.ToLower() -eq $Sha256)
if (-not $ok) {
    Write-Host ">> Baixando Freedoom $Version ($Url)" -ForegroundColor Cyan
    $old = $ProgressPreference
    $ProgressPreference = "SilentlyContinue"
    Invoke-WebRequest -Uri $Url -OutFile $Zip
    $ProgressPreference = $old
}
$hash = (Get-FileHash $Zip -Algorithm SHA256).Hash.ToLower()
if ($hash -ne $Sha256) { throw "SHA-256 do Freedoom nao confere: $hash (esperado $Sha256)" }

$Extract = Join-Path $Tmp "x"
if (Test-Path $Extract) { Remove-Item -Recurse -Force $Extract }
New-Item -ItemType Directory -Force -Path $Extract | Out-Null
& tar.exe -xf $Zip -C $Extract "$Name/freedoom1.wad" "$Name/COPYING.txt"
if ($LASTEXITCODE -ne 0) { throw "falha ao extrair $Zip" }
New-Item -ItemType Directory -Force -Path $Dir | Out-Null
Copy-Item (Join-Path $Extract "$Name\freedoom1.wad") $Wad -Force
Copy-Item (Join-Path $Extract "$Name\COPYING.txt") (Join-Path $Dir "COPYING.txt") -Force
$whash = (Get-FileHash $Wad -Algorithm SHA256).Hash.ToLower()
if ($whash -ne $WadSha256) { throw "SHA-256 do freedoom1.wad nao confere: $whash" }
Write-Host "Freedoom $Version em third_party\freedoom" -ForegroundColor Green
return $Wad
