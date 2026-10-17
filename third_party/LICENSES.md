# Codigo de terceiros

| Nome | Versao | Origem | Licenca | Onde fica | Como chega |
|---|---|---|---|---|---|
| Limine | 12.8.0 | https://github.com/Limine-Bootloader/Limine/releases/tag/v12.8.0 | BSD-2-Clause (`third_party/limine/LICENSE`) | `third_party/limine` | versionado no git; o `setup.ps1` baixa de novo se faltar |
| Zig (compilador, inclui a musl 1.2.x e os cabecalhos do Linux) | 0.16.0 | https://ziglang.org/download/0.16.0/ (SHA-256 conferido pelo `tools/zig.ps1` e pelo CI) | MIT (Zig); a musl embutida e MIT | `third_party/zig` | fora do git (`.gitignore`); `tools/zig.ps1` baixa na primeira compilacao |

Os programas em `user/musl` sao ligados estaticamente contra a musl que vem
com o Zig; a licenca MIT da musl acompanha o Zig em `third_party/zig/lib/libc/musl/COPYRIGHT`.
