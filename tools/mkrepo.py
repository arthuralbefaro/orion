import argparse
import hashlib
import io
import os
import tarfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

PACKAGES = [
    ('lua', '5.4.9', 'Interpretador da linguagem Lua', [('bin/lua', 'bin/lua', 0o755)]),
    ('hello', '1.0', 'Programa de exemplo que mostra os argumentos', [('bin/hello-pkg', 'bin/hello', 0o755)]),
]


def add(tar, name, data, mode):
    info = tarfile.TarInfo(name)
    info.size = len(data)
    info.mode = mode
    info.mtime = 0
    info.uid = info.gid = 0
    info.uname = info.gname = 'root'
    tar.addfile(info, io.BytesIO(data))


def build(esp, out, name, version, desc, files):
    manifest = ['nome %s' % name, 'versao %s' % version, 'descricao %s' % desc]
    payload = []
    for dest, src, mode in files:
        with open(os.path.join(esp, *src.split('/')), 'rb') as fh:
            data = fh.read()
        payload.append((dest, data, mode))
        manifest.append('arquivo %s %o %s' % (dest, mode, hashlib.sha256(data).hexdigest()))
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode='w', format=tarfile.USTAR_FORMAT) as tar:
        add(tar, 'MANIFEST', ('\n'.join(manifest) + '\n').encode(), 0o644)
        for dest, data, mode in payload:
            add(tar, dest, data, mode)
    blob = buf.getvalue()
    fname = '%s-%s.tar' % (name, version)
    with open(os.path.join(out, fname), 'wb') as fh:
        fh.write(blob)
    return fname, blob


def make_repo(esp, out):
    os.makedirs(out, exist_ok=True)
    lines = []
    for name, version, desc, files in PACKAGES:
        fname, blob = build(esp, out, name, version, desc, files)
        lines.append('%s %s %s %d %s %s' % (name, version, fname, len(blob), hashlib.sha256(blob).hexdigest(), desc))
    fname, blob = build(esp, out, 'quebrado', '1.0', 'Pacote com a soma errada (teste)', [('bin/quebrado', 'bin/hello', 0o755)])
    lines.append('quebrado 1.0 %s %d %s %s' % (fname, len(blob), '0' * 64, 'Pacote com a soma errada (teste)'))
    with open(os.path.join(out, 'INDEX'), 'w', newline='\n') as fh:
        fh.write('\n'.join(lines) + '\n')
    return len(lines)


def main():
    ap = argparse.ArgumentParser(description='Gera o repositorio estatico de pacotes do Orion (INDEX e .tar).')
    ap.add_argument('--esp', default=os.path.join(ROOT, 'out', 'esp'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 'http', 'repo'))
    a = ap.parse_args()
    n = make_repo(a.esp, a.out)
    print('mkrepo: %s, %d pacote(s)' % (a.out, n))


if __name__ == '__main__':
    main()
