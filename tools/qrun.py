#!/usr/bin/env python3
import argparse
import os
import platform
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "out"

OVMF_CODE_CANDIDATES = [
    "/usr/share/OVMF/OVMF_CODE_4M.fd",
    "/usr/share/OVMF/OVMF_CODE.fd",
    "/usr/share/edk2/x64/OVMF_CODE.fd",
    "/usr/share/edk2-ovmf/x64/OVMF_CODE.fd",
    "/usr/share/qemu/edk2-x86_64-code.fd",
]
OVMF_VARS_CANDIDATES = [
    "/usr/share/OVMF/OVMF_VARS_4M.fd",
    "/usr/share/OVMF/OVMF_VARS.fd",
    "/usr/share/edk2/x64/OVMF_VARS.fd",
    "/usr/share/edk2-ovmf/x64/OVMF_VARS.fd",
    "/usr/share/qemu/edk2-i386-vars.fd",
]


def find_qemu():
    exe = shutil.which("qemu-system-x86_64")
    if exe:
        return exe
    win = Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "qemu" / "qemu-system-x86_64.exe"
    if win.exists():
        return str(win)
    raise SystemExit("qemu-system-x86_64 nao encontrado")


def find_firmware(qemu):
    share = Path(qemu).parent / "share"
    code = share / "edk2-x86_64-code.fd"
    vars_ = share / "edk2-i386-vars.fd"
    if code.exists() and vars_.exists():
        return code, vars_
    for c in OVMF_CODE_CANDIDATES:
        if Path(c).exists():
            for v in OVMF_VARS_CANDIDATES:
                if Path(v).exists():
                    return Path(c), Path(v)
    raise SystemExit("firmware UEFI (OVMF) nao encontrado; no Ubuntu: apt install ovmf")


def port_free(port):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.bind(("127.0.0.1", port))
        return True
    except OSError:
        return False
    finally:
        s.close()


class Machine:
    def __init__(self, smp=4, memory=512, disk=False, serial_port=4444, monitor_port=4445,
                 boot_timeout=60.0):
        self.smp = smp
        self.memory = memory
        self.disk = disk
        self.serial_port = serial_port
        self.monitor_port = monitor_port
        self.boot_timeout = boot_timeout
        self.log = bytearray()
        self.proc = None
        self.serial = None
        self.monitor = None
        self.cmd_pos = 0

    def start(self):
        OUT.mkdir(exist_ok=True)
        qemu = find_qemu()
        code, vars_template = find_firmware(qemu)
        vars_copy = OUT / "ovmf-vars-test.fd"
        if not vars_copy.exists():
            shutil.copyfile(vars_template, vars_copy)
        if self.disk:
            img = OUT / "disk.img"
            if not img.exists():
                with open(img, "wb") as f:
                    f.truncate(64 * 1024 * 1024)
        end = time.time() + 20
        while time.time() < end and not (port_free(self.serial_port) and port_free(self.monitor_port)):
            time.sleep(0.3)
        args = [
            qemu, "-M", "q35", "-cpu", "qemu64", "-smp", str(self.smp), "-m", f"{self.memory}M",
            "-drive", f"if=pflash,format=raw,unit=0,file={code},readonly=on",
            "-drive", f"if=pflash,format=raw,unit=1,file={vars_copy}",
            "-drive", "format=raw,file=fat:rw:out/esp",
            "-display", "none",
            "-serial", f"tcp:127.0.0.1:{self.serial_port},server,nowait",
            "-monitor", f"tcp:127.0.0.1:{self.monitor_port},server,nowait",
            "-no-reboot", "-no-shutdown",
            "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
            "-object", "filter-dump,id=d0,netdev=n0,file=out/net.pcap",
        ]
        if self.disk:
            args += ["-drive", "format=raw,file=out/disk.img"]
        self.stderr = open(OUT / "test-qemu.log", "wb")
        self.proc = subprocess.Popen(args, cwd=ROOT, stdout=subprocess.DEVNULL, stderr=self.stderr)
        self.serial = self._connect(self.serial_port, self.boot_timeout)

    def _connect(self, port, timeout):
        end = time.time() + timeout
        while time.time() < end:
            if self.proc.poll() is not None:
                break
            try:
                s = socket.create_connection(("127.0.0.1", port), timeout=1)
                s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                s.setblocking(False)
                return s
            except OSError:
                time.sleep(0.2)
        err = (OUT / "test-qemu.log").read_text(errors="replace")
        raise RuntimeError(f"nao conectou na porta {port}. QEMU: {err}")

    def _read_some(self):
        try:
            data = self.serial.recv(65536)
        except BlockingIOError:
            return False
        except OSError:
            return False
        if data:
            self.log += data
            return True
        return False

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            if not self._read_some():
                time.sleep(0.015)

    def text(self, start=0):
        return self.log[start:].decode("ascii", errors="replace")

    def wait_for(self, needle, timeout, start=0):
        raw = needle.encode()
        end = time.time() + timeout
        while time.time() < end:
            if self.log.find(raw, start) >= 0:
                return True
            if not self._read_some():
                time.sleep(0.05)
        return self.log.find(raw, start) >= 0

    def send(self, data):
        if isinstance(data, str):
            data = data.encode("ascii")
        self.serial.setblocking(True)
        self.serial.sendall(data)
        self.serial.setblocking(False)

    def monitor_cmd(self, cmd):
        if self.monitor is None:
            self.monitor = self._connect(self.monitor_port, 10)
        self.monitor.setblocking(True)
        self.monitor.sendall((cmd + "\n").encode())
        self.monitor.setblocking(False)
        time.sleep(0.2)
        try:
            while self.monitor.recv(8192):
                pass
        except (BlockingIOError, OSError):
            pass

    def screenshot(self, path):
        path = Path(path)
        if not path.is_absolute():
            path = ROOT / path
        path.parent.mkdir(parents=True, exist_ok=True)
        if path.exists():
            path.unlink()
        self.monitor_cmd("screendump " + str(path).replace("\\", "/"))
        end = time.time() + 10
        while time.time() < end and not path.exists():
            time.sleep(0.15)
        if not path.exists():
            print(f"  screenshot FALHOU: {path}")
            return
        time.sleep(0.4)
        try:
            from PIL import Image
            png = path.with_suffix(".png")
            Image.open(path).save(png)
            print(f"  screenshot: {png}")
        except Exception:
            print(f"  screenshot: {path}")

    def stop(self):
        try:
            if self.monitor is not None:
                self.monitor_cmd("quit")
        except OSError:
            pass
        time.sleep(0.3)
        for s in (self.serial, self.monitor):
            try:
                if s is not None:
                    s.close()
            except OSError:
                pass
        if self.proc and self.proc.poll() is None:
            self.proc.kill()
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                pass

    def run_step(self, step, command_delay):
        kind, _, arg = step.partition(":")
        if kind == "cmd":
            print(f"  cmd  > {arg}")
            self.cmd_pos = len(self.log)
            self.send(arg + "\r")
            self.pump(command_delay)
        elif kind == "raw":
            print(f"  raw  > {arg}")
            self.send(arg)
            self.pump(0.3)
        elif kind == "key":
            print(f"  key  > {arg}")
            self.send(bytes([int(arg, 16)]))
            self.pump(0.6)
        elif kind == "keys":
            print(f"  keys > {arg}")
            self.send(bytes.fromhex(arg))
            self.pump(0.6)
        elif kind == "mon":
            print(f"  mon  > {arg}")
            self.monitor_cmd(arg)
            self.pump(0.35)
        elif kind == "wait":
            self.pump(float(arg))
        elif kind == "until":
            secs, _, needle = arg.partition(":")
            print(f"  until> {needle} ({secs} s)")
            if not self.wait_for(needle, float(secs), self.cmd_pos):
                print(f"  AVISO: '{needle}' nao apareceu em {secs} s")
        elif kind == "shot":
            self.pump(0.4)
            self.screenshot(arg)
        else:
            print(f"  passo desconhecido: {step}")


def build(gui=False):
    if platform.system() == "Windows":
        cmd = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(ROOT / "build.ps1")]
        if gui:
            cmd.append("-Gui")
        r = subprocess.run(cmd, cwd=ROOT, stdout=subprocess.DEVNULL)
    else:
        r = subprocess.run(["make", "-j", str(os.cpu_count() or 2)], cwd=ROOT, stdout=subprocess.DEVNULL)
        if r.returncode == 0:
            write_autostart(gui)
    if r.returncode != 0:
        raise SystemExit("build falhou")


def write_autostart(gui):
    target = OUT / "esp" / "etc" / "autostart"
    target.parent.mkdir(parents=True, exist_ok=True)
    if gui:
        target.write_text("/bin/dhcp\nshell /bin/wm\n")
    else:
        shutil.copyfile(ROOT / "etc" / "autostart", target)


def run(steps, smp=4, memory=512, disk=False, gui=False, no_build=False, boot_timeout=60.0,
        command_delay=1.0, settle=2.0, log_path="out/test-serial.log", kernel_shell=False):
    if not no_build:
        build(gui)
    elif gui:
        write_autostart(True)
    m = Machine(smp=smp, memory=memory, disk=disk, boot_timeout=boot_timeout)
    code = 0
    try:
        m.start()
        all_steps = (["key:0B"] if kernel_shell else []) + list(steps)
        need_prompt = any(s.split(":", 1)[0] in ("cmd", "raw", "key", "keys") for s in all_steps)
        if need_prompt or not all_steps:
            if m.wait_for("$ ", boot_timeout):
                print("prompt do shell de usuario detectado")
            else:
                print(f"AVISO: prompt do shell nao apareceu em {boot_timeout} s")
        else:
            m.pump(min(boot_timeout, 12))
        for s in all_steps:
            m.run_step(s, command_delay)
        m.pump(settle)
    except Exception as e:
        print(f"ERRO: {e}")
        code = 1
    finally:
        m.stop()
    text = m.text().replace("\r\n", "\n").replace("\r", "\n")
    full = ROOT / log_path
    full.parent.mkdir(parents=True, exist_ok=True)
    full.write_text(text, encoding="utf-8")
    return code, text


def main():
    ap = argparse.ArgumentParser(description="Roda o Orion no QEMU e manda passos pela serial")
    ap.add_argument("steps", nargs="*", help="passos: cmd:..., wait:..., until:s:texto, key:hex, ...")
    ap.add_argument("-c", "--command", action="append", default=[], help="atalho para cmd:<linha>")
    ap.add_argument("--smp", type=int, default=4)
    ap.add_argument("--memory", type=int, default=512)
    ap.add_argument("--disk", action="store_true")
    ap.add_argument("--gui", action="store_true")
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--kernel-shell", action="store_true")
    ap.add_argument("--boot-timeout", type=float, default=60.0)
    ap.add_argument("--delay", type=float, default=1.0)
    ap.add_argument("--settle", type=float, default=2.0)
    ap.add_argument("--log", default="out/test-serial.log")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args()
    steps = [f"cmd:{c}" for c in a.command] + a.steps
    code, text = run(steps, smp=a.smp, memory=a.memory, disk=a.disk, gui=a.gui, no_build=a.no_build,
                     boot_timeout=a.boot_timeout, command_delay=a.delay, settle=a.settle,
                     log_path=a.log, kernel_shell=a.kernel_shell)
    if not a.quiet:
        print(f"----- {a.log} -----")
        print(text)
    sys.exit(code)


if __name__ == "__main__":
    main()
