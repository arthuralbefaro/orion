#!/usr/bin/env python3
import hashlib
import platform
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TCC = ROOT / "third_party" / "tinycc"
VERSION = "0.9.28rc"
LIBTCC1 = ["libtcc1.c", "stdatomic.c", "builtin.c", "va_list.c", "dsohandle.c", "atomic.S", "alloca.S",
           "alloca-bt.S"]
EMPTY_LIBS = ["m", "pthread", "rt", "dl", "util", "crypt", "resolv", "xnet"]

CONFIG = f"""#define TCC_VERSION "{VERSION}"
#define TCC_TARGET_X86_64 1
#define CONFIG_TCC_MUSL 1
#define CONFIG_TCC_STATIC 1
#define CONFIG_TCC_PREDEFS 0
#define CONFIG_TCC_SEMLOCK 0
#define CONFIG_TCC_BCHECK 0
#define CONFIG_TCC_BACKTRACE 0
#define CONFIG_TCCDIR "/lib/tcc"
#define CONFIG_TCC_SYSINCLUDEPATHS "{{B}}/include:/usr/include"
#define CONFIG_TCC_LIBPATHS "{{B}}:/usr/lib"
#define CONFIG_TCC_CRTPREFIX "/usr/lib"
#define CONFIG_TCC_ELFINTERP "/lib/ld-musl-x86_64.so.1"
#define CONFIG_TCC_SWITCHES "-static"
"""


def write_if_changed(path, data):
    if isinstance(data, str):
        data = data.encode("utf-8")
    if path.exists() and path.read_bytes() == data:
        return
    path.write_bytes(data)


def run(cmd, cwd=None):
    r = subprocess.run([str(c) for c in cmd], cwd=cwd, capture_output=True, text=True, encoding="utf-8",
                       errors="replace")
    if r.returncode != 0:
        raise SystemExit("tcc_gen: falhou: " + " ".join(str(c) for c in cmd) + "\n" + r.stdout + r.stderr)
    return r


def digest(zig):
    h = hashlib.sha256()
    h.update(Path(__file__).read_bytes())
    h.update(run([zig, "version"]).stdout.encode())
    for f in sorted((TCC / "lib").iterdir()) + sorted((TCC / "include").iterdir()):
        h.update(f.name.encode())
        h.update(f.read_bytes())
    return h.hexdigest()


def zig_libc(zig, work):
    (work / "hello.c").write_text('#include <stdio.h>\nint main(void){puts("x");return 0;}\n')
    r = run([zig, "build-exe", "hello.c", "-lc", "-target", "x86_64-linux-musl", "-static", "-OReleaseSmall",
             "--verbose-link", "-femit-bin=" + str(work / "hello")], cwd=work)
    text = r.stdout + r.stderr
    found = {}
    for name in ("crt1.o", "libc.a", "libzigc.a", "libcompiler_rt.a"):
        for m in re.finditer(re.escape(name) + r'(?="|\s|$)', text):
            end = m.end()
            start = m.start()
            while start > 0:
                prev = text.rfind(" ", 0, start - 1)
                cand = text[prev + 1:end].strip().strip('"')
                if Path(cand).is_file():
                    found[name] = Path(cand)
                    break
                if prev < 0 or "\n" in text[prev:start]:
                    break
                start = prev + 1
            if name in found:
                break
    missing = [n for n in ("crt1.o", "libc.a", "libzigc.a", "libcompiler_rt.a") if n not in found]
    if missing:
        raise SystemExit("tcc_gen: o link do zig nao mostrou " + ", ".join(missing) + "\n" + text)
    return found


def build_libc(zig, found, usrlib, work):
    shutil.copyfile(found["crt1.o"], usrlib / "crt1.o")
    libc = usrlib / "libc.a"
    shutil.copyfile(found["libc.a"], libc)
    for extra in ("libzigc.a", "libcompiler_rt.a"):
        d = work / extra.replace(".", "_")
        d.mkdir()
        run([zig, "ar", "x", found[extra]], cwd=d)
        objs = sorted(d.iterdir())
        if not objs:
            raise SystemExit(f"tcc_gen: {extra} vazio")
        run([zig, "ar", "q", libc] + objs)
    run([zig, "ar", "s", libc])
    (work / "empty.c").write_text("\n")
    run([zig, "cc", "-target", "x86_64-linux-musl", "-c", "-o", usrlib / "crti.o", work / "empty.c"])
    shutil.copyfile(usrlib / "crti.o", usrlib / "crtn.o")
    for name in EMPTY_LIBS:
        (usrlib / f"lib{name}.a").write_bytes(b"!<arch>\n")


def build_libtcc1(zig, tccdir, work):
    objs = []
    for src in LIBTCC1:
        obj = work / (src.replace(".", "_") + ".o")
        run([zig, "cc", "-target", "x86_64-linux-musl", "-O2", "-fno-pic", "-fno-stack-protector", "-w",
             "-I", TCC, "-c", "-o", obj, TCC / "lib" / src])
        objs.append(obj)
    out = tccdir / "libtcc1.a"
    if out.exists():
        out.unlink()
    run([zig, "ar", "rcs", out] + objs)


def zig_lib_dir(zig):
    text = run([zig, "env"], cwd=ROOT).stdout
    m = re.search(r'lib_dir"?\s*[:=]\s*"((?:[^"\\]|\\.)*)"', text)
    if not m:
        raise SystemExit("tcc_gen: zig env nao mostrou lib_dir\n" + text)
    path = Path(m.group(1).replace("\\\\", "\\"))
    if not path.is_absolute():
        path = ROOT / path
    return path


def copy_tree(src, dst):
    for f in src.rglob("*"):
        if f.is_file():
            t = dst / f.relative_to(src)
            t.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(f, t)


def main():
    gen = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "out" / "gen" / "tcc"
    zig = sys.argv[2] if len(sys.argv) > 2 else str(ROOT / "third_party" / "zig" /
                                                      ("zig.exe" if platform.system() == "Windows" else "zig"))
    zig = str(Path(zig).resolve()) if Path(zig).exists() else zig
    gen.mkdir(parents=True, exist_ok=True)
    write_if_changed(gen / "config.h", CONFIG)
    want = digest(zig)
    stamp = gen / "stamp"
    sysroot = gen / "sysroot"
    if stamp.exists() and stamp.read_text().strip() == want and (sysroot / "usr" / "lib" / "libc.a").exists():
        return
    if stamp.exists():
        stamp.unlink()
    if sysroot.exists():
        shutil.rmtree(sysroot)
    tccdir = sysroot / "lib" / "tcc"
    usrlib = sysroot / "usr" / "lib"
    usrinc = sysroot / "usr" / "include"
    for d in (tccdir / "include", usrlib, usrinc):
        d.mkdir(parents=True)
    copy_tree(TCC / "include", tccdir / "include")
    zinc = zig_lib_dir(zig) / "libc" / "include"
    for sub_dir in ("generic-musl", "x86_64-linux-musl"):
        if not (zinc / sub_dir).is_dir():
            raise SystemExit(f"tcc_gen: cabecalhos da musl nao encontrados em {zinc / sub_dir}")
    copy_tree(zinc / "generic-musl", usrinc)
    copy_tree(zinc / "x86_64-linux-musl", usrinc)
    if not (usrinc / "stdio.h").is_file() or not (usrinc / "bits" / "alltypes.h").is_file():
        raise SystemExit("tcc_gen: /usr/include ficou incompleto")
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        build_libtcc1(zig, tccdir, work)
        build_libc(zig, zig_libc(zig, work), usrlib, work)
    write_if_changed(sysroot / "lib" / "tcc" / "hello.c",
                     '#include <stdio.h>\n\nint main(void)\n{\n    printf("ola do tcc dentro do Orion: %d\\n", 6 * 7);\n'
                     '    return 0;\n}\n')
    stamp.write_text(want)


if __name__ == "__main__":
    main()
