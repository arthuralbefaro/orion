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


def program_spec(prog, zig):
    srcs, extra, deps, links = [], [], [], []
    gen = ROOT / "out" / "gen" / prog.name
    spec = prog / "build.txt"
    if spec.exists():
        deps.append(spec)
        for line in spec.read_text(encoding="utf-8").splitlines():
            t = line.strip()
            if t.startswith("gen "):
                script = ROOT / t[4:].strip()
                gen.mkdir(parents=True, exist_ok=True)
                r = subprocess.run([sys.executable, str(script), str(gen), zig], cwd=ROOT)
                if r.returncode != 0:
                    raise SystemExit(f"gerador {script} de {prog.name} falhou")
                deps.append(script)
            elif t.startswith("gensrc "):
                found = sorted(glob.glob(str(gen / t[7:].strip())))
                if not found:
                    raise SystemExit(f"build.txt de {prog.name}: nada gerado em {t[7:].strip()}")
                srcs += [Path(f) for f in found]
            elif t.startswith("src "):
                found = sorted(glob.glob(str(prog / t[4:].strip())))
                if not found:
                    raise SystemExit(f"build.txt de {prog.name}: nada em {t[4:].strip()}")
                srcs += [Path(f) for f in found]
            elif t.startswith("flag "):
                for f in t[5:].split():
                    f = f.replace("@gen", str(gen)).replace("@root", str(ROOT))
                    if f.startswith("-I") and not os.path.isabs(f[2:]):
                        f = "-I" + str(prog / f[2:])
                    extra.append(f)
            elif t.startswith("dep "):
                deps += [Path(f) for f in glob.glob(str(prog / t[4:].strip()), recursive=True)]
            elif t.startswith("link "):
                links += t[5:].split()
    srcs += sorted(prog.glob("*.c"))
    deps += sorted(prog.glob("*.h"))
    if gen.exists():
        deps += sorted(gen.glob("*.h"))
    return srcs, extra, deps, links


def build_all(quiet=False):
    musl = ROOT / "user" / "musl"
    if not musl.exists():
        return []
    zig = find_zig()
    out = ROOT / "out"
    bindir = out / "esp" / "bin"
    bindir.mkdir(parents=True, exist_ok=True)
    built = []
    links = {}
    for prog in sorted(p for p in musl.iterdir() if p.is_dir()):
        srcs, extra, deps, plinks = program_spec(prog, zig)
        for n in plinks:
            links[n] = prog.name
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
    progs = ROOT / "user" / "progs"
    taken = {p.name for p in progs.iterdir() if p.is_dir()} if progs.exists() else set()
    taken |= set(built)
    for n, owner in links.items():
        if n not in taken:
            shutil.copyfile(out / f"{owner}.elf", bindir / n)
    wad = None
    if "doom" in built:
        sys.path.insert(0, str(ROOT / "tools"))
        import freedoom
        try:
            wad = freedoom.fetch()
        except (OSError, SystemExit) as e:
            print(f"aviso: sem o freedoom1.wad ({e}); o doom fica sem IWAD")
    if wad and wad.exists():
        (out / "esp" / "doom").mkdir(parents=True, exist_ok=True)
        dest = out / "esp" / "doom" / "freedoom1.wad"
        if not dest.exists() or dest.stat().st_size != wad.stat().st_size:
            shutil.copyfile(wad, dest)
    return built


if __name__ == "__main__":
    names = build_all()
    print(f"{len(names)} programa(s) da musl em out/esp/bin")
    sys.exit(0)
