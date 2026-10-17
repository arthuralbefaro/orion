#!/usr/bin/env python3
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SBASE = ROOT / "third_party" / "sbase"
EXCLUDE = {"getconf"}

HEADER = """#include <unistd.h>

#include <libgen.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "sbase-box.h"

struct cmd {
\tchar *name;
\tint (*fn)(int, char **);
} cmds[] = {
\t{"[", test_main},
"""

FOOTER = """\t{NULL},
};

int
main(int argc, char *argv[])
{
\tchar *s = basename(argv[0]);
\tstruct cmd *bp;

\tif (!strcmp(s, "sbase-box") && argc > 1) {
\t\targc--; argv++;
\t\ts = basename(argv[0]);
\t}

\tfor (bp = cmds; bp->name; ++bp) {
\t\tif (strcmp(bp->name, s) == 0)
\t\t\treturn (*bp->fn)(argc, argv);
\t}

\tfor (bp = cmds; bp->name; ++bp)
\t\tprintf("%s ", bp->name);
\tputchar('\\n');

\treturn 0;
}
"""


def write_if_changed(path, text):
    data = text.encode("utf-8")
    if path.exists() and path.read_bytes() == data:
        return
    path.write_bytes(data)


def utilities():
    names = []
    for f in sorted(SBASE.glob("*.c")):
        name = f.stem
        if name in EXCLUDE:
            continue
        if re.search(r"^main", f.read_text(encoding="utf-8", errors="replace"), re.M):
            names.append(name)
    return names


def main():
    gen = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "out" / "gen" / "sbase"
    gen.mkdir(parents=True, exist_ok=True)
    table = []
    protos = []
    for name in utilities():
        text = (SBASE / f"{name}.c").read_text(encoding="utf-8", errors="replace")
        lines = text.split("\n")
        fn = name.replace("-", "_") + "_main"
        for i, line in enumerate(lines):
            if line.startswith("main"):
                lines[i] = line.replace("main", fn, 1)
                break
        write_if_changed(gen / f"{name}.c", "\n".join(lines))
        cmd = "install" if name == "xinstall" else name
        table.append(f'\t{{"{cmd}", {fn}}},')
        protos.append(f"int {fn}(int, char **);")
    write_if_changed(gen / "sbase-box.c", HEADER + "\n".join(table) + "\n" + FOOTER)
    write_if_changed(gen / "sbase-box.h", "\n".join(protos) + "\n")
    keep = {f"{name}.c" for name in utilities()} | {"sbase-box.c"}
    for f in gen.glob("*.c"):
        if f.name not in keep:
            f.unlink()


if __name__ == "__main__":
    main()
