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
WADS = {
    "freedoom1.wad": "7323bcc168c5a45ff10749b339960e98314740a734c30d4b9f3337001f9e703d",
    "freedoom2.wad": "a8772e088847032510d97ba2312406a6998f21cbab44d4ff10696faa9c0ecd4b",
}
NAME = f"freedoom-{VERSION}"
URL = f"https://github.com/freedoom/freedoom/releases/download/v{VERSION}/{NAME}.zip"
DIR = ROOT / "third_party" / "freedoom"


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def present():
    return all((DIR / w).exists() and sha256(DIR / w) == h for w, h in WADS.items())


def fetch():
    paths = [DIR / w for w in WADS]
    if present():
        return paths
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
        for w in WADS:
            with z.open(f"{NAME}/{w}") as src, open(DIR / w, "wb") as dst:
                shutil.copyfileobj(src, dst)
        with z.open(f"{NAME}/COPYING.txt") as src, open(DIR / "COPYING.txt", "wb") as dst:
            shutil.copyfileobj(src, dst)
    for w, h in WADS.items():
        got = sha256(DIR / w)
        if got != h:
            raise SystemExit(f"SHA-256 do {w} nao confere: {got}")
    print(f"Freedoom {VERSION} em third_party/freedoom")
    return paths


if __name__ == "__main__":
    fetch()
    sys.exit(0)
