#!/usr/bin/env python3
import argparse
import os
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import qrun

ROOT = qrun.ROOT
PS1 = ROOT / "regressao.ps1"

STRING = re.compile(r'"((?:[^"`]|`.)*)"|\'((?:[^\']|\'\')*)\'')


def ps_string(m):
    if m.group(1) is not None:
        s = m.group(1)
        return re.sub(r"`(.)", lambda e: {"n": "\n", "r": "\r", "t": "\t", "0": "\0"}.get(e.group(1), e.group(1)), s)
    return m.group(2).replace("''", "'")


def parse_sequences():
    src = PS1.read_text(encoding="utf-8")
    seqs = {}
    blocks = re.split(r"^if \(\$runAll -or \$Only([A-Z])\) \{\s*$", src, flags=re.M)
    for i in range(1, len(blocks), 2):
        name = blocks[i]
        body = blocks[i + 1]
        end = re.search(r"^\}\s*$", body, flags=re.M)
        if end:
            body = body[:end.start()]
        seq = {"name": name, "steps": [], "expect": [], "memdelta": None, "shots": [], "pixels": [], "sizes": [], "disk": False, "gui": False, "image": False, "hostfwd": [],
               "extra": [], "prep": [],
               "delay": 1.0, "boot": 60.0, "remove_disk": False, "log": f"out/reg-{name.lower()}.log",
               "stress": None, "hostweb": None}
        m = re.search(r"\$steps" + name + r"\s*=\s*@\((.*?)^\s*\)\s*$", body, flags=re.S | re.M)
        if m:
            seq["steps"] = [ps_string(s) for s in STRING.finditer(m.group(1))]
        call = re.search(r'test\.ps1"\)(.*?)\|\s*Out-Null', body, flags=re.S)
        if call:
            c = call.group(1)
            seq["disk"] = "-Disk" in c
            ex = re.search(r"-ExtraDisks\s+@\(([^)]*)\)", c)
            if ex:
                seq["extra"] = [ps_string(s) for s in STRING.finditer(ex.group(1))]
            seq["gui"] = "-Gui" in c
            seq["image"] = "-Image" in c
            seq["iso"] = bool(re.search(r"-Iso\b", c))
            seq["iso_disk"] = "-IsoDisk" in c
            seq["bios"] = bool(re.search(r"-Bios\b", c))
            nic = re.search(r"-Nic\s+(\S+)", c)
            seq["nic"] = nic.group(1) if nic else "e1000e"
            bus = re.search(r"-Bus\s+(\S+)", c)
            seq["bus"] = bus.group(1) if bus else "ahci"
            seq["usb"] = bool(re.search(r"-Usb\b", c))
            seq["no_ps2"] = bool(re.search(r"-NoPs2\b", c))
            seq["usb_disk"] = bool(re.search(r"-UsbDisk\b", c))
            seq["usb_fat"] = bool(re.search(r"-UsbFat\b", c))
            seq["no_strict"] = bool(re.search(r"-NoStrict\b", c))
            seq["audio"] = bool(re.search(r"-Audio\b", c))
            vga = re.search(r"-Vga\s+(\S+)", c)
            seq["vga"] = vga.group(1) if vga else ""
            seq["usb_hub"] = bool(re.search(r"-UsbHub\b", c))
            hf = re.search(r"-HostFwd\s+@\(([^)]*)\)", c)
            if hf:
                seq["hostfwd"] = [ps_string(s) for s in STRING.finditer(hf.group(1))]
            d = re.search(r"-CommandDelay\s+([\d.]+)", c)
            if d:
                seq["delay"] = float(d.group(1))
            b = re.search(r"-BootTimeout\s+([\d.]+)", c)
            if b:
                seq["boot"] = float(b.group(1))
            lp = re.search(r'-LogPath\s+"([^"]+)"', c)
            if lp:
                seq["log"] = lp.group(1).replace("\\", "/")
        for mk in re.finditer(r'tools\\mkext2\.py"\)\s+(.+)$', body, flags=re.M):
            seq["prep"].append([ps_string(s) if s.group(0).startswith(('"', "'")) else s.group(0)
                                for s in re.finditer(r'"(?:[^"`]|`.)*"|\'(?:[^\']|\'\')*\'|\S+', mk.group(1))])
        seq["remove_disk"] = bool(re.search(r'Remove-Item \(Join-Path \$Root "out\\disk\.img"\)', body))
        seq["remove"] = re.findall(r'Remove-Item \(Join-Path \$Root "out\\(stick\d*\.img)"\)', body)
        seq["blank"] = [(b.group(1).replace("\\", "/"), int(b.group(2)))
                        for b in re.finditer(r'^\s*New-BlankImage "([^"]+)" (\d+)\s*$', body, flags=re.M)]
        seq["hostfat"] = re.findall(r'tools\\mkfat\.py"\) le (\S+) (\S+)', body)
        seq["hostwav"] = re.findall(r'tools\\wavcheck\.py"\) (\S+) (.+)$', body, flags=re.M)
        seq["remove"] += re.findall(r'Remove-Item \(Join-Path \$Root "out\\(audio\.wav)"\)', body)
        for e in re.finditer(r'^\s*(Expect|ExpectNot) \$\w+ ("(?:[^"`]|`.)*"|\'(?:[^\']|\'\')*\') ("(?:[^"`]|`.)*"|\'(?:[^\']|\'\')*\')\s*$',
                             body, flags=re.M):
            pat = ps_string(STRING.match(e.group(2)))
            desc = ps_string(STRING.match(e.group(3)))
            seq["expect"].append((pat, desc, e.group(1) == "ExpectNot"))
        for px in re.finditer(r'^\s*Test-ShotPixel "([^"]+)" (\d+) (\d+) 0x([0-9a-fA-F]{6}) (\d+) "([^"]+)"\s*$', body, flags=re.M):
            seq["pixels"].append((px.group(1).replace("\\", "/"), int(px.group(2)), int(px.group(3)),
                                  int(px.group(4), 16), int(px.group(5)), px.group(6)))
        for sz in re.finditer(r'^\s*Test-ShotSize "([^"]+)" (\d+) (\d+) "([^"]+)"\s*$', body, flags=re.M):
            seq["sizes"].append((sz.group(1).replace("\\", "/"), int(sz.group(2)), int(sz.group(3)), sz.group(4)))
        for sc in re.finditer(r'^\s*Test-ShotColors "([^"]+)" "([^"]+)" (\d+)\s*$', body, flags=re.M):
            seq["shots"].append((sc.group(1).replace("\\", "/"), sc.group(2), int(sc.group(3))))
        md = re.search(r'^\s*Test-MemDelta \$\w+ "([^"]+)" (\d+)\s*$', body, flags=re.M)
        if md:
            seq["memdelta"] = (md.group(1), int(md.group(2)))
        hw = re.search(r"Start-HostWeb (\d+)", body)
        if hw:
            seq["hostweb"] = int(hw.group(1))
        st = re.search(r"\$stressCmd\s*=\s*\"([^\"]+)\"", body)
        if st:
            seq["stress"] = st.group(1)
        seqs[name] = seq
    return seqs


class Report:
    def __init__(self):
        self.checks = 0
        self.failures = []

    def expect(self, log, pattern, desc, negate=False):
        self.checks += 1
        if bool(re.search(pattern, log, flags=re.I)) != negate:
            print(f"  [ok]    {desc}")
        else:
            print(f"  [FALHA] {desc}   (padrao: {pattern})")
            self.failures.append(desc)

    def memdelta(self, log, desc, limit):
        self.checks += 1
        m = re.findall(r"Paginas: \d+ total, (\d+) usadas", log)
        if len(m) < 2:
            print(f"  [FALHA] {desc}   (precisa de dois 'mem', achei {len(m)})")
            self.failures.append(desc)
            return
        a, b = int(m[0]), int(m[-1])
        d = abs(b - a)
        if d <= limit:
            print(f"  [ok]    {desc} ({a} -> {b}, delta {d})")
        else:
            print(f"  [FALHA] {desc} ({a} -> {b}, delta {d} > {limit})")
            self.failures.append(desc)


    def shot_pixel(self, ppm, x, y, rgb, tol, desc):
        self.checks += 1
        path = ROOT / ppm
        if not path.exists():
            print(f"  [FALHA] {desc}   (sem a captura {ppm})")
            self.failures.append(desc)
            return
        data = path.read_bytes()
        fields, i = [], 0
        while len(fields) < 4 and i < len(data):
            tok = b""
            while i < len(data) and not data[i:i + 1].isspace():
                tok += data[i:i + 1]
                i += 1
            if tok:
                fields.append(tok)
            i += 1
        p = i + (y * int(fields[1]) + x) * 3
        got = data[p:p + 3]
        exp = ((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255)
        seen = got.hex()
        if all(abs(got[c] - exp[c]) <= tol for c in range(3)):
            print(f"  [ok]    {desc} (pixel {x},{y} = {seen})")
        else:
            print(f"  [FALHA] {desc} (pixel {x},{y} = {seen}, esperado {rgb:06x} +-{tol})")
            self.failures.append(desc)

    def shot_size(self, ppm, w, h, desc):
        self.checks += 1
        path = ROOT / ppm.replace("\\", "/")
        if not path.exists():
            print(f"  [FALHA] {desc}   (sem a captura {ppm})")
            self.failures.append(desc)
            return
        dims = path.read_bytes()[:32].split()
        if len(dims) >= 3 and dims[1] == str(w).encode() and dims[2] == str(h).encode():
            print(f"  [ok]    {desc}")
        else:
            print(f"  [FALHA] {desc} (captura {dims[1:3]})")
            self.failures.append(desc)

    def shot_colors(self, ppm, desc, minimum):
        self.checks += 1
        path = ROOT / ppm.replace("\\", "/")
        if not path.exists():
            print(f"  [FALHA] {desc}   (sem a captura {ppm})")
            self.failures.append(desc)
            return
        data = path.read_bytes()
        i = 0
        lines = 0
        while i < len(data) and lines < 3:
            if data[i] == 10:
                lines += 1
            i += 1
        seen = set()
        for p in range(i, len(data) - 2, 3 * 997):
            seen.add(data[p] * 65536 + data[p + 1] * 256 + data[p + 2])
        if len(seen) >= minimum:
            print(f"  [ok]    {desc} ({len(seen)} cores na amostra)")
        else:
            print(f"  [FALHA] {desc} ({len(seen)} cores na amostra, minimo {minimum})")
            self.failures.append(desc)


def run_stress(seq, rep, runs, smp, image=False):
    cmd = seq["stress"]
    for i in range(1, runs + 1):
        log_path = f"out/reg-e-{i}.log"
        _, log = qrun.run([f"cmd:{cmd}", "cmd:echo FIM$X-ESTRESSE", "until:1200:FIM-ESTRESSE"], smp=smp,
                          no_build=True, log_path=log_path, image=image)
        rep.expect(log, r"\nFIM-ESTRESSE", f"estresse rodada {i}: pipeline terminou")
        rep.expect(log, r"stress: \d+ processos, 0 falha\(s\) ok", f"estresse rodada {i}: 0 falhas")
        rep.checks += 1
        if re.search(r"\*\*\* KERNEL PANIC \*\*\*|\*\*\* EXCECAO NAO TRATADA|deadlock|watchdog|FALHA|fork devolveu 0", log):
            print(f"  [FALHA] estresse rodada {i}: panico, deadlock ou falha no log ({log_path})")
            rep.failures.append(f"estresse rodada {i}: log limpo")
        else:
            print(f"  [ok]    estresse rodada {i}: sem panico, deadlock ou falha")


def main():
    ap = argparse.ArgumentParser(description="Regressao do Orion (le os passos do regressao.ps1)")
    ap.add_argument("which", nargs="?", default="ABCDFIVNUSGWHE", help="sequencias, ex.: ABCDFIVNUSGWHE ou AC")
    ap.add_argument("--smp", type=int, default=4)
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--stress-runs", type=int, default=1)
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--slow-waits", action="store_true", help="esperas fixas antigas, sem esperar o prompt")
    a = ap.parse_args()
    if a.slow_waits:
        os.environ["ORION_SLOW_WAITS"] = "1"

    seqs = parse_sequences()
    image = any(s["image"] for s in seqs.values())
    rep = Report()
    built = a.no_build
    for name in a.which.upper():
        seq = seqs.get(name)
        if not seq:
            print(f"sequencia {name} nao existe no regressao.ps1")
            rep.failures.append(f"sequencia {name}")
            continue
        print(f"== Sequencia {name} ==")
        if seq["stress"]:
            if not built:
                qrun.build(False, image)
                built = True
            run_stress(seq, rep, a.stress_runs, a.smp, seq["image"])
            continue
        if seq["remove_disk"]:
            (ROOT / "out" / "disk.img").unlink(missing_ok=True)
        for name in seq["remove"]:
            (ROOT / "out" / name).unlink(missing_ok=True)
        for rel, mib in seq["blank"]:
            with open(ROOT / rel, "wb") as fh:
                fh.truncate(mib * 1024 * 1024)
        if seq["prep"] and not built:
            qrun.build(False, image)
            built = True
        for args in seq["prep"]:
            subprocess.run([sys.executable, str(ROOT / "tools" / "mkext2.py")] + args, check=True, cwd=ROOT)
        wants_iso = seq.get("iso", False) or seq.get("iso_disk", False)
        no_build = built and not seq["gui"] and not wants_iso
        if (seq["gui"] or wants_iso) and a.no_build:
            no_build = True
        web = None
        if seq["hostweb"]:
            web = subprocess.Popen([sys.executable, str(ROOT / "tools" / "hostweb.py"), "--port", str(seq["hostweb"])],
                                   cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            time.sleep(2)
        try:
            _, log = qrun.run(seq["steps"], smp=a.smp, disk=seq["disk"], extra_disks=seq["extra"], gui=seq["gui"],
                              no_build=no_build, image=seq["image"], hostfwd=seq["hostfwd"],
                              boot_timeout=seq["boot"], command_delay=seq["delay"], log_path=seq["log"],
                              iso=seq.get("iso", False), iso_disk=seq.get("iso_disk", False),
                              bios=seq.get("bios", False), nic=seq.get("nic", "e1000e"),
                              bus=seq.get("bus", "ahci"), usb=seq.get("usb", False),
                              no_ps2=seq.get("no_ps2", False), usb_disk=seq.get("usb_disk", False),
                              usb_fat=seq.get("usb_fat", False), no_strict=seq.get("no_strict", False),
                              audio=seq.get("audio", False), vga=seq.get("vga", ""),
                              usb_hub=seq.get("usb_hub", False))
        finally:
            if web:
                web.kill()
        for image_path, name in seq["hostfat"]:
            got = subprocess.run([sys.executable, str(ROOT / "tools" / "mkfat.py"), "le", image_path, name],
                                 cwd=ROOT, capture_output=True, text=True, encoding="latin-1")
            log += f"\n----- hospedeiro: {name} -----\n" + got.stdout
        for wav_path, wav_args in seq["hostwav"]:
            got = subprocess.run([sys.executable, str(ROOT / "tools" / "wavcheck.py"), wav_path] + wav_args.split(),
                                 cwd=ROOT, capture_output=True, text=True)
            log += "\n----- hospedeiro: wavcheck -----\n" + got.stdout
        built = built or not seq["gui"]
        if seq["gui"]:
            qrun.write_autostart(False)
            if image and qrun.make_image().returncode != 0:
                rep.failures.append("mkimage depois da sequencia grafica")
        if a.verbose:
            print(log)
        for pat, desc, negate in seq["expect"]:
            rep.expect(log, pat, desc, negate)
        for ppm, desc, minimum in seq["shots"]:
            rep.shot_colors(ppm, desc, minimum)
        for ppm, w, h, desc in seq["sizes"]:
            rep.shot_size(ppm, w, h, desc)
        for ppm, x, y, rgb, tol, desc in seq["pixels"]:
            rep.shot_pixel(ppm, x, y, rgb, tol, desc)
        if seq["memdelta"]:
            rep.memdelta(log, *seq["memdelta"])

    print()
    if not rep.failures:
        print(f"REGRESSAO VERDE ({rep.checks} verificacoes)")
        sys.exit(0)
    print(f"REGRESSAO VERMELHA: {len(rep.failures)} de {rep.checks} falharam")
    for f in rep.failures:
        print(f"  - {f}")
    sys.exit(1)


if __name__ == "__main__":
    main()
