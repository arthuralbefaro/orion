$flags = "--target=x86_64-unknown-none-elf","-std=gnu11","-ffreestanding","-nostdlibinc","-fno-stack-protector","-mno-red-zone","-mcmodel=kernel","-mno-sse","-Wall","-Ikernel\include","-Ithird_party\limine","-fsyntax-only"
$uflags = "--target=x86_64-unknown-none-elf","-std=gnu11","-ffreestanding","-nostdlibinc","-mno-sse","-Wall","-Wno-unused-parameter","-Iuser\lib","-fsyntax-only"
$errors = 0
Get-ChildItem -Recurse kernel\src -File -Include *.c,*.S | ForEach-Object {
    $out = clang @flags $_.FullName 2>&1 | Select-String "error|warning"
    if ($out) { $errors++; $out }
}
Get-ChildItem -Recurse user -File -Include *.c | Where-Object { $_.FullName -notlike "*\user\musl\*" } | ForEach-Object {
    $out = clang @uflags $_.FullName 2>&1 | Select-String "error|warning"
    if ($out) { $errors++; $out }
}
if (Test-Path user\musl) {
    $zig = Join-Path $PSScriptRoot "third_party\zig\zig.exe"
    if (-not (Test-Path $zig)) { $zig = & (Join-Path $PSScriptRoot "tools\zig.ps1") }
    Get-ChildItem user\musl -Directory | ForEach-Object {
        $dir = $_.FullName
        $mflags = @("cc", "-target", "x86_64-linux-musl", "-Wall", "-Wextra", "-Wno-unused-parameter", "-c", "-o",
                    (Join-Path $env:TEMP "orion-check-musl.o"))
        $spec = Join-Path $dir "build.txt"
        if (Test-Path $spec) {
            foreach ($line in Get-Content $spec) {
                $t = $line.Trim()
                if ($t.StartsWith("flag ")) {
                    $f = $t.Substring(5).Trim()
                    if ($f.StartsWith("-I")) { $f = "-I" + (Join-Path $dir $f.Substring(2)) }
                    $mflags += $f
                }
            }
        }
        Get-ChildItem $dir -Filter *.c -File | ForEach-Object {
            $out = & $zig @mflags $_.FullName 2>&1 | Select-String "error|warning"
            if ($out) { $errors++; $out }
        }
    }
}
if ($errors -eq 0) { Write-Host "Nenhum erro ou warning." -ForegroundColor Green }
