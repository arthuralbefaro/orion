# Codigo de terceiros

| Nome | Versao | Origem | Licenca | Onde fica | Como chega |
|---|---|---|---|---|---|
| Limine | 12.8.0 | https://github.com/Limine-Bootloader/Limine/releases/tag/v12.8.0 | BSD-2-Clause (`third_party/limine/LICENSE`) | `third_party/limine` | versionado no git; o `setup.ps1` baixa de novo se faltar; o `limine.c` (utilitario do `bios-install`) e compilado sem mudanca pelo `build.ps1 -Iso` em `out/tools` |
| Zig (compilador, inclui a musl 1.2.x e os cabecalhos do Linux) | 0.16.0 | https://ziglang.org/download/0.16.0/ (SHA-256 conferido pelo `tools/zig.ps1` e pelo CI) | MIT (Zig); a musl embutida e MIT | `third_party/zig` | fora do git (`.gitignore`); `tools/zig.ps1` baixa na primeira compilacao |
| dash | 0.5.13.5 | http://gondor.apana.org.au/~herbert/dash/files/dash-0.5.13.5.tar.gz (SHA-256 `40090101a2a491f13e901d3d48e90414f26634628b9bfff35ff540363c227a7d`, o mesmo do `.sha512sum` assinado do autor) | BSD-3-Clause (`third_party/dash/COPYING`); o `signames.c` gerado segue o `mksignames.c`, GPL-2.0-or-later | `third_party/dash` | versionado no git, como veio do tarball |
| Lua | 5.4.9 | https://www.lua.org/ftp/lua-5.4.9.tar.gz (SHA-256 `2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6`, o publicado em lua.org) | MIT (aviso no fim de `third_party/lua/src/lua.h`) | `third_party/lua` | versionado no git, como veio do tarball |
| sbase | commit `c546c3a5724c81cee9a11d816a38ccdf17472129` (arvore `a58d176568a1b33d435a7628740b68289ea5c07a`, 2026-05-25) | https://git.suckless.org/sbase (sem tarball dessa versao: exportado com `git archive` do commit) | MIT (`third_party/sbase/LICENSE`) | `third_party/sbase` | versionado no git, como veio do commit |
| doomgeneric | commit `e257a09527062bfb986b6e5e0780839b6cbd5b71` (2026-10-11) | https://github.com/ozkl/doomgeneric/archive/e257a09527062bfb986b6e5e0780839b6cbd5b71.tar.gz (SHA-256 `26f8f60daa6c6e4d3c01e5eac9fb700d592dede803d13ac848154ff356b2a2e7`) | GPL-2.0 (`third_party/doomgeneric/LICENSE`); o binario `doom` resultante e GPL-2.0 | `third_party/doomgeneric` | versionado no git, como veio do arquivo |
| Freedoom (Phase 1) | 0.13.0 | https://github.com/freedoom/freedoom/releases/download/v0.13.0/freedoom-0.13.0.zip (SHA-256 `3f9b264f3e3ce503b4fb7f6bdcb1f419d93c7b546f4df3e874dd878db9688f59`, o mesmo do `freedoom-0.13.0-CHECKSUM` assinado; `freedoom1.wad` `7323bcc168c5a45ff10749b339960e98314740a734c30d4b9f3337001f9e703d`) | BSD-3-Clause (`third_party/freedoom/COPYING.txt`) | `third_party/freedoom` | fora do git (28 MB); `tools/freedoom.ps1` baixa no primeiro build que tem o `doom` e o `build.ps1` copia para `/doom/freedoom1.wad` |
| uACPI | 6.1.1 | https://github.com/uACPI/uACPI/archive/refs/tags/6.1.1.tar.gz (SHA-256 `40f2e278b2080e69f4eaa3384404944874d430e50b72c1066cab2ba476e2b769`) | MIT (`third_party/uacpi/LICENSE`) | `third_party/uacpi` | versionado no git: `include`, `source`, `LICENSE` e `README.md` do tarball, sem mudanca; compilado junto do kernel |
| TinyCC (tcc) | commit `e23dabed154a8495f3ac1ff3cc9d04fa1c207893` (2026-10-16, versao 0.9.28rc) | https://repo.or.cz/tinycc.git/snapshot/e23dabed154a8495f3ac1ff3cc9d04fa1c207893.tar.gz (SHA-256 `e8110b26f8a7c553e1c6ad61afd4ed8cd789f397366e05cd8d6b8c38c5e5e6cc`) | LGPL-2.1 (`third_party/tinycc/COPYING`, com `RELICENSING`); a `libtcc1.a` que vai para `/lib/tcc` vem de `lib/libtcc1.c` (GPL-2.0+ com excecao de ligacao) e `lib/atomic.S` (GPL-3.0+ com a GCC Runtime Library Exception) | `third_party/tinycc` | versionado no git, como veio do snapshot |

Os programas em `user/musl` sao ligados estaticamente contra a musl que vem
com o Zig; a licenca MIT da musl acompanha o Zig em `third_party/zig/lib/libc/musl/COPYRIGHT`.

`dash`, `lua` e `sbase-box` (o multicall do sbase) sao compilados pelo
`build.ps1` a partir de `user/musl/<nome>/build.txt`, sem mudar uma linha das
fontes. Os arquivos que o `make` deles geraria (o `config.h` e as tabelas do
dash, o multicall do sbase) saem de `tools/dash_gen.py` e `tools/sbase_box.py`
em `out/gen`, fora do git.
