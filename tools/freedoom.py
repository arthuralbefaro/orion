#!/usr/bin/env python3
import hashlib
import shutil
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VERSION = "0.13.0"
SHA256 = "3f9b264f3e3ce503b4fb7f6bdcb1f419d93c7b546f4df3e874dd878db9688f59"
WAD_SHA256 = "7323bcc168c5a45ff10749b339960e98314740a734c30d4b9f3337001f9e703d"
NAME = f"freedoom-{VERSION}"
URL = f"https://github.com/freedoom/freedoom/releases/download/v{VERSION}/{NAME}.zip"
DIR = ROOT / "third_party" / "freedoom"
WAD = DIR / "freedoom1.wad"


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fetch():
    if WAD.exists() and sha256(WAD) == WAD_SHA256:
        return WAD
    tmp = Path(tempfile.gettempdir()) / "orion-freedoom"
    tmp.mkdir(parents=True, exist_ok=True)
    zpath = tmp / f"{NAME}.zip"
    if not zpath.exists() or sha256(zpath) != SHA256:
        print(f">> Baixando Freedoom {VERSION} ({URL})")
        with urllib.request.urlopen(URL) as r, open(zpath, "wb") as out:
            shutil.copyfileobj(r, out)
    got = sha256(zpath)
    if got != SHA256:
        raise SystemExit(f"SHA-256 do Freedoom nao confere: {got} (esperado {SHA256})")
    DIR.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(zpath) as z:
        with z.open(f"{NAME}/freedoom1.wad") as src, open(WAD, "wb") as dst:
            shutil.copyfileobj(src, dst)
        with z.open(f"{NAME}/COPYING.txt") as src, open(DIR / "COPYING.txt", "wb") as dst:
            shutil.copyfileobj(src, dst)
    got = sha256(WAD)
    if got != WAD_SHA256:
        raise SystemExit(f"SHA-256 do freedoom1.wad nao confere: {got}")
    print(f"Freedoom {VERSION} em third_party/freedoom")
    return WAD


if __name__ == "__main__":
    fetch()
    sys.exit(0)
