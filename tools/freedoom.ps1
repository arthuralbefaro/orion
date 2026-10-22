$ErrorActionPreference = "Stop"
$Root = Split-Path $PSScriptRoot -Parent
$Version = "0.13.0"
$Sha256 = "3f9b264f3e3ce503b4fb7f6bdcb1f419d93c7b546f4df3e874dd878db9688f59"
$Wads = [ordered]@{
    "freedoom1.wad" = "7323bcc168c5a45ff10749b339960e98314740a734c30d4b9f3337001f9e703d"
    "freedoom2.wad" = "a8772e088847032510d97ba2312406a6998f21cbab44d4ff10696faa9c0ecd4b"
}
$Name = "freedoom-$Version"
$Dir = Join-Path $Root "third_party\freedoom"

function Test-Wads {
    foreach ($w in $Wads.Keys) {
        $p = Join-Path $Dir $w
        if (-not (Test-Path $p) -or (Get-FileHash $p -Algorithm SHA256).Hash.ToLower() -ne $Wads[$w]) { return $false }
    }
    return $true
}

$paths = @($Wads.Keys | ForEach-Object { Join-Path $Dir $_ })
if (Test-Wads) { return $paths }

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
$members = @($Wads.Keys | ForEach-Object { "$Name/$_" }) + @("$Name/COPYING.txt")
& tar.exe -xf $Zip -C $Extract @members
if ($LASTEXITCODE -ne 0) { throw "falha ao extrair $Zip" }
New-Item -ItemType Directory -Force -Path $Dir | Out-Null
foreach ($w in $Wads.Keys) {
    Copy-Item (Join-Path $Extract "$Name\$w") (Join-Path $Dir $w) -Force
    $whash = (Get-FileHash (Join-Path $Dir $w) -Algorithm SHA256).Hash.ToLower()
    if ($whash -ne $Wads[$w]) { throw "SHA-256 do $w nao confere: $whash" }
}
Copy-Item (Join-Path $Extract "$Name\COPYING.txt") (Join-Path $Dir "COPYING.txt") -Force
Write-Host "Freedoom $Version em third_party\freedoom" -ForegroundColor Green
return $paths
