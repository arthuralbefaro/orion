# Codigo de terceiros

| Nome | Versao | Origem | Licenca | Onde fica | Como chega |
|---|---|---|---|---|---|
| Limine | 12.8.0 | https://github.com/Limine-Bootloader/Limine/releases/tag/v12.8.0 | BSD-2-Clause (`third_party/limine/LICENSE`) | `third_party/limine` | versionado no git; o `setup.ps1` baixa de novo se faltar |
| Zig (compilador, inclui a musl 1.2.x e os cabecalhos do Linux) | 0.16.0 | https://ziglang.org/download/0.16.0/ (SHA-256 conferido pelo `tools/zig.ps1` e pelo CI) | MIT (Zig); a musl embutida e MIT | `third_party/zig` | fora do git (`.gitignore`); `tools/zig.ps1` baixa na primeira compilacao |
| dash | 0.5.13.5 | http://gondor.apana.org.au/~herbert/dash/files/dash-0.5.13.5.tar.gz (SHA-256 `40090101a2a491f13e901d3d48e90414f26634628b9bfff35ff540363c227a7d`, o mesmo do `.sha512sum` assinado do autor) | BSD-3-Clause (`third_party/dash/COPYING`); o `signames.c` gerado segue o `mksignames.c`, GPL-2.0-or-later | `third_party/dash` | versionado no git, como veio do tarball |
| Lua | 5.4.9 | https://www.lua.org/ftp/lua-5.4.9.tar.gz (SHA-256 `2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6`, o publicado em lua.org) | MIT (aviso no fim de `third_party/lua/src/lua.h`) | `third_party/lua` | versionado no git, como veio do tarball |
| sbase | commit `c546c3a5724c81cee9a11d816a38ccdf17472129` (arvore `a58d176568a1b33d435a7628740b68289ea5c07a`, 2026-05-25) | https://git.suckless.org/sbase (sem tarball dessa versao: exportado com `git archive` do commit) | MIT (`third_party/sbase/LICENSE`) | `third_party/sbase` | versionado no git, como veio do commit |

Os programas em `user/musl` sao ligados estaticamente contra a musl que vem
com o Zig; a licenca MIT da musl acompanha o Zig em `third_party/zig/lib/libc/musl/COPYRIGHT`.

`dash`, `lua` e `sbase-box` (o multicall do sbase) sao compilados pelo
`build.ps1` a partir de `user/musl/<nome>/build.txt`, sem mudar uma linha das
fontes. Os arquivos que o `make` deles geraria (o `config.h` e as tabelas do
dash, o multicall do sbase) saem de `tools/dash_gen.py` e `tools/sbase_box.py`
em `out/gen`, fora do git.
