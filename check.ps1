$flags = "--target=x86_64-unknown-none-elf","-std=gnu11","-ffreestanding","-nostdlibinc","-fno-stack-protector","-mno-red-zone","-mcmodel=kernel","-mno-sse","-Wall","-Ikernel\include","-Ithird_party\limine","-fsyntax-only"
$uflags = "--target=x86_64-unknown-none-elf","-std=gnu11","-ffreestanding","-nostdlibinc","-mno-sse","-Wall","-Wno-unused-parameter","-Iuser\lib","-fsyntax-only"
$errors = 0
Get-ChildItem -Recurse kernel\src -File -Include *.c,*.S | ForEach-Object {
    $out = clang @flags $_.FullName 2>&1 | Select-String "error|warning"
    if ($out) { $errors++; $out }
}
Get-ChildItem -Recurse user -File -Include *.c | ForEach-Object {
    $out = clang @uflags $_.FullName 2>&1 | Select-String "error|warning"
    if ($out) { $errors++; $out }
}
if ($errors -eq 0) { Write-Host "Nenhum erro ou warning." -ForegroundColor Green }
