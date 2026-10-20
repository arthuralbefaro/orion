import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mkext2
import mkimage

S_IFDIR = 0o040000
S_IFREG = 0o100000
S_IFLNK = 0o120000
SKIP = {'lost+found'}


def pad4(buf):
    buf.extend(b'\0' * (-len(buf) % 4))


def entry(out, ino, name, mode, uid, gid, nlink, data):
    raw = name.encode() + b'\0'
    fields = (ino, mode, uid, gid, nlink, mkext2.EPOCH, len(data), 0, 0, 0, 0, len(raw), 0)
    out += b'070701' + b''.join(b'%08X' % f for f in fields)
    out += raw
    pad4(out)
    out += data
    pad4(out)


def walk(node, path, out, counter):
    for name in sorted(node.children):
        if not path and name in SKIP:
            continue
        child = node.children[name]
        full = path + '/' + name if path else name
        counter[0] += 1
        if child.kind == 'dir':
            entry(out, counter[0], full, S_IFDIR | child.mode, child.uid, child.gid, 2, b'')
            walk(child, full, out, counter)
        elif child.kind == 'link':
            entry(out, counter[0], full, S_IFLNK | 0o777, child.uid, child.gid, 1, child.target)
        else:
            entry(out, counter[0], full, S_IFREG | child.mode, child.uid, child.gid, 1, child.data)


def main():
    ap = argparse.ArgumentParser(description='Gera o initramfs (cpio newc) do sistema live a partir do esp')
    ap.add_argument('output')
    ap.add_argument('--esp', required=True)
    a = ap.parse_args()
    root = mkimage.root_tree(a.esp)
    for rel in ('EFI/BOOT/BOOTX64.EFI', 'boot/orion.elf', 'boot/limine/limine.conf'):
        with open(os.path.join(a.esp, *rel.split('/')), 'rb') as fh:
            mkext2.add_path(root, '/usr/lib/instalar/esp/' + rel, mkext2.Node('file', 0o644, fh.read()))
    out = bytearray()
    counter = [1]
    entry(out, 1, '.', S_IFDIR | 0o755, 0, 0, 2, b'')
    walk(root, '', out, counter)
    entry(out, 0, 'TRAILER!!!', 0, 0, 0, 1, b'')
    out.extend(b'\0' * (-len(out) % 512))
    with open(a.output, 'wb') as fh:
        fh.write(out)
    print('mkinitrd: %s, %d entradas, %.1f MiB' % (a.output, counter[0], len(out) / (1024 * 1024)))


if __name__ == '__main__':
    main()
