# Orion

Kernel x86_64 escrito do zero em C (gnu11), bootado pelo Limine.

## Requisitos (Windows)
`.\setup.ps1` instala tudo via winget: LLVM (clang + ld.lld), QEMU e Git,
e baixa o Limine 12.8.0 para `third_party\limine`.

## Uso
    .\build.ps1          # compila (incremental)
    .\build.ps1 -Run     # compila e roda no QEMU (UEFI)
    .\build.ps1 -Clean   # limpa

No QEMU a serial aparece no terminal; o shell aceita comandos pelo teclado
da janela ou digitando no terminal. `help` lista os comandos.

## Layout
    kernel/include/   headers (kernel/, lib/, drivers/, arch/x86_64/, mm/, boot/)
    kernel/src/       fontes, mesma divisao
    third_party/      Limine (bootloader) - baixado pelo setup.ps1
    linker.ld         layout do ELF (higher half, 0xffffffff80000000)
    limine.conf       configuracao do bootloader
    build/ out/       gerados (out/esp = pasta que o QEMU boota como disco FAT)

## Toolchain
clang/lld como cross-compiler (`--target=x86_64-unknown-none-elf`), sem libc,
sem SSE/x87 no kernel, red zone desligada, modelo de codigo "kernel".