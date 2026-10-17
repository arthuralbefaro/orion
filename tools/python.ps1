$candidates = @()
$py = Get-Command "py" -ErrorAction SilentlyContinue
if ($py) { $candidates += , @($py.Source, "-3") }
foreach ($name in "python3", "python") {
    foreach ($c in @(Get-Command $name -All -ErrorAction SilentlyContinue)) {
        if ($c.Source -notlike "*\WindowsApps\*") { $candidates += , @($c.Source) }
    }
}
foreach ($c in $candidates) {
    $exe = $c[0]
    $pre = @($c | Select-Object -Skip 1)
    $out = & $exe @pre -c "import sys; print(sys.version_info[0])" 2>$null
    if ($LASTEXITCODE -eq 0 -and "$out".Trim() -eq "3") { return , $c }
}
throw "Python 3 nao encontrado (o build do dash e do sbase gera arquivos com ele). Rode .\setup.ps1 ou instale o Python 3 e ponha no PATH."
