import argparse
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mkimage import FatDir, Fat16, fat_add


def create(path, size_mib, label, files, texts):
    root = FatDir()
    for spec in files:
        name, _, src = spec.partition('=')
        with open(src, 'rb') as fh:
            fat_add(root, name, fh.read())
    for spec in texts:
        name, _, text = spec.partition('=')
        fat_add(root, name, (text + '\n').encode('latin-1'))
    img = Fat16(size_mib * 1024 * 1024, label, 0, time.time()).build(root)
    with open(path, 'wb') as fh:
        fh.write(img)


def read_file(path, name):
    data = open(path, 'rb').read()
    sector, spc, reserved, nfats, root_entries = struct.unpack_from('<HBHBH', data, 11)
    fatsz = struct.unpack_from('<H', data, 22)[0]
    root_off = (reserved + nfats * fatsz) * sector
    data_off = root_off + root_entries * 32
    cbytes = sector * spc
    fat_off = reserved * sector
    want = name.upper()
    base, _, ext = want.partition('.')
    short = (base.ljust(8) + ext.ljust(3)).encode('ascii')
    for i in range(root_entries):
        e = data[root_off + i * 32:root_off + i * 32 + 32]
        if e[0] == 0:
            break
        if e[0] == 0xE5 or e[11] == 0x0F or e[11] & 0x08:
            continue
        if e[:11] != short:
            continue
        cluster = struct.unpack_from('<H', e, 26)[0]
        size = struct.unpack_from('<I', e, 28)[0]
        out = bytearray()
        seen = 0
        while 2 <= cluster < 0xFFF8 and len(out) < size and seen < 65536:
            off = data_off + (cluster - 2) * cbytes
            out += data[off:off + cbytes]
            cluster = struct.unpack_from('<H', data, fat_off + cluster * 2)[0]
            seen += 1
        return bytes(out[:size])
    return None


def main():
    ap = argparse.ArgumentParser(description='Cria um pendrive FAT16 (sem particoes) ou le um arquivo da raiz dele.')
    sub = ap.add_subparsers(dest='cmd', required=True)
    c = sub.add_parser('cria')
    c.add_argument('imagem')
    c.add_argument('--tamanho', type=int, default=16, help='MiB')
    c.add_argument('--rotulo', default='PENDRIVE')
    c.add_argument('--arquivo', action='append', default=[], help='NOME=origem')
    c.add_argument('--texto', action='append', default=[], help='NOME=conteudo (uma linha)')
    r = sub.add_parser('le')
    r.add_argument('imagem')
    r.add_argument('nome')
    a = ap.parse_args()
    if a.cmd == 'cria':
        create(a.imagem, a.tamanho, a.rotulo, a.arquivo, a.texto)
        print('mkfat: %s, %d MiB, rotulo %s' % (a.imagem, a.tamanho, a.rotulo))
        return 0
    got = read_file(a.imagem, a.nome)
    if got is None:
        print('mkfat: %s nao achado em %s' % (a.nome, a.imagem))
        return 1
    sys.stdout.write(got.decode('latin-1'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
