$flags = "--target=x86_64-unknown-none-elf","-std=gnu11","-ffreestanding","-nostdlibinc","-fno-stack-protector","-mno-red-zone","-mcmodel=kernel","-mno-sse","-Wall","-Ikernel\include","-Ithird_party\limine","-fsyntax-only"
$errors = 0
Get-ChildItem -Recurse kernel\src -File -Include *.c,*.S | ForEach-Object {
    $out = clang @flags $_.FullName 2>&1 | Select-String "error|warning"
    if ($out) { $errors++; $out }
}
if (Test-Path user\musl) {
    $zig = Join-Path $PSScriptRoot "third_party\zig\zig.exe"
    if (-not (Test-Path $zig)) { $zig = & (Join-Path $PSScriptRoot "tools\zig.ps1") }
    Get-ChildItem user\musl -Directory | ForEach-Object {
        $dir = $_.FullName
        $mflags = @("cc", "-target", "x86_64-linux-musl", "-Wall", "-Wextra", "-Wno-unused-parameter",
                    ("-I" + (Join-Path $PSScriptRoot "user\include")), "-c", "-o",
                    (Join-Path $env:TEMP "orion-check-musl.o"))
        $spec = Join-Path $dir "build.txt"
        if (Test-Path $spec) {
            foreach ($line in Get-Content $spec) {
                $t = $line.Trim()
                if ($t.StartsWith("flag ")) {
                    foreach ($f in ($t.Substring(5).Trim() -split '\s+')) {
                        $f = $f.Replace("@gen", (Join-Path $PSScriptRoot "out\gen\$($_.Name)")).Replace("@root", $PSScriptRoot)
                        if ($f.StartsWith("-I") -and -not [System.IO.Path]::IsPathRooted($f.Substring(2))) {
                            $f = "-I" + (Join-Path $dir $f.Substring(2))
                        }
                        $mflags += $f
                    }
                }
            }
        }
        Get-ChildItem $dir -Filter *.c -File | ForEach-Object {
            $out = & $zig @mflags $_.FullName 2>&1 | Select-String "error|warning"
            if ($out) { $errors++; $out }
        }
    }
}
$hostZig = Join-Path $PSScriptRoot "third_party\zig\zig.exe"
if (-not (Test-Path $hostZig)) { $hostZig = & (Join-Path $PSScriptRoot "tools\zig.ps1") }
Get-ChildItem tools -Filter *.c -File -ErrorAction SilentlyContinue | ForEach-Object {
    $out = & $hostZig cc -std=c99 -Wall -Wextra -c -o (Join-Path $env:TEMP "orion-check-tool.o") $_.FullName 2>&1 |
        Select-String "error|warning"
    if ($out) { $errors++; $out }
}
if (Test-Path kernel\rust) {
    $cargo = Get-Command cargo.exe -ErrorAction SilentlyContinue
    $cargo = if ($cargo) { $cargo.Source } else { Join-Path $env:USERPROFILE ".cargo\bin\cargo.exe" }
    if (-not (Test-Path $cargo)) {
        $errors++
        "cargo nao encontrado (rode .\setup.ps1)"
    } else {
        Get-ChildItem kernel\rust -Directory | ForEach-Object {
            $manifest = Join-Path $_.FullName "Cargo.toml"
            if (-not (Test-Path $manifest)) { return }
            $prevFlags = $env:RUSTFLAGS
            $env:RUSTFLAGS = "-C code-model=kernel -C relocation-model=static -D warnings"
            $out = & $cargo clippy --release --quiet --target x86_64-unknown-none --manifest-path $manifest `
                --target-dir build\rust -- -D warnings 2>&1 | Select-String "error|warning"
            $clippyExit = $LASTEXITCODE
            $env:RUSTFLAGS = $prevFlags
            if ($out -or $clippyExit -ne 0) { $errors++; $out; "clippy saiu com $clippyExit em $($_.Name)" }
            $py = & (Join-Path $PSScriptRoot "tools\python.ps1")
            $pyArgs = @($py | Select-Object -Skip 1)
            $prevPython = $env:PYTHON
            $env:PYTHON = & $py[0] @pyArgs -c "import sys; print(sys.executable)"
            $env:RUSTFLAGS = "-D warnings"
            $out = & $cargo test --quiet --manifest-path $manifest --target-dir build\rust-test 2>&1
            $testExit = $LASTEXITCODE
            $env:RUSTFLAGS = $prevFlags
            $env:PYTHON = $prevPython
            if ($testExit -ne 0) { $errors++; $out | Select-Object -Last 30; "cargo test saiu com $testExit em $($_.Name)" }
        }
    }
    Get-ChildItem -Recurse kernel\rust -File -Include *.rs | ForEach-Object {
        if (Select-String -Path $_.FullName -Pattern '^\s*//|/\*|[^:"]//' -Quiet) {
            $errors++
            "comentario em $($_.FullName)"
        }
    }
}
if ($errors -eq 0) { Write-Host "Nenhum erro ou warning." -ForegroundColor Green }
