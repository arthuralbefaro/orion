#!/usr/bin/env python3
import glob
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FLAGS = ["cc", "-target", "x86_64-linux-musl", "-static", "-O2", "-s", "-Wall", "-Wextra",
         "-Wno-unused-parameter"]


def find_zig():
    exe = "zig.exe" if platform.system() == "Windows" else "zig"
    local = ROOT / "third_party" / "zig" / exe
    if local.exists():
        return str(local)
    found = shutil.which("zig")
    if found:
        return found
    raise SystemExit("zig nao encontrado: rode setup.ps1 (Windows) ou ponha o Zig 0.16.0 em third_party/zig")


def program_spec(prog):
    srcs, extra, deps = [], [], []
    spec = prog / "build.txt"
    if spec.exists():
        deps.append(spec)
        for line in spec.read_text(encoding="utf-8").splitlines():
            t = line.strip()
            if t.startswith("src "):
                found = sorted(glob.glob(str(prog / t[4:].strip())))
                if not found:
                    raise SystemExit(f"build.txt de {prog.name}: nada em {t[4:].strip()}")
                srcs += [Path(f) for f in found]
            elif t.startswith("flag "):
                f = t[5:].strip()
                if f.startswith("-I"):
                    f = "-I" + str(prog / f[2:])
                extra.append(f)
            elif t.startswith("dep "):
                deps += [Path(f) for f in glob.glob(str(prog / t[4:].strip()), recursive=True)]
    srcs += sorted(prog.glob("*.c"))
    deps += sorted(prog.glob("*.h"))
    return srcs, extra, deps


def build_all(quiet=False):
    musl = ROOT / "user" / "musl"
    if not musl.exists():
        return []
    zig = find_zig()
    out = ROOT / "out"
    bindir = out / "esp" / "bin"
    bindir.mkdir(parents=True, exist_ok=True)
    built = []
    for prog in sorted(p for p in musl.iterdir() if p.is_dir()):
        srcs, extra, deps = program_spec(prog)
        elf = out / f"{prog.name}.elf"
        newest = max((f.stat().st_mtime for f in srcs + deps if f.exists()), default=0)
        if not elf.exists() or elf.stat().st_mtime < newest:
            if not quiet:
                print(f"  ZIG  {prog.name} ({len(srcs)} arquivo(s))")
            r = subprocess.run([zig] + FLAGS + extra + ["-o", str(elf)] + [str(s) for s in srcs], cwd=ROOT)
            if r.returncode != 0:
                raise SystemExit(f"erro compilando {prog.name} contra a musl")
        shutil.copyfile(elf, bindir / prog.name)
        built.append(prog.name)
    return built


if __name__ == "__main__":
    names = build_all()
    print(f"{len(names)} programa(s) da musl em out/esp/bin")
    sys.exit(0)
