import argparse
import os
import struct
import sys
import time
import uuid
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mkext2

MIB = 1024 * 1024
SECTOR = 512
ESP_TYPE = uuid.UUID('C12A7328-F81F-11D2-BA4B-00A0C93EC93B')
ROOT_TYPE = uuid.UUID('4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709')
GPT_ENTRIES = 128
GPT_ENTRY_SIZE = 128
GPT_ENTRY_SECTORS = GPT_ENTRIES * GPT_ENTRY_SIZE // SECTOR
SHORT_OK = set('ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789$%\'-_@~`!(){}^#&')
ROOT_SKIP = {'boot', 'EFI', 'efi'}
SPECIAL_MODES = {'etc/shadow': 0o600, 'bin/su': 0o4755, 'bin/passwd': 0o4755}
SPECIAL_OWNERS = {'home/orion': (1000, 100)}
MOUNT_POINTS = {'boot': 0o755, 'dev': 0o755, 'proc': 0o555, 'tmp': 0o1777, 'run': 0o755, 'mnt': 0o755,
                'root': 0o700}


class FatDir:
    def __init__(self):
        self.children = {}
        self.cluster = 0


def fat_add(root, path, data):
    parts = [p for p in path.split('/') if p]
    node = root
    for p in parts[:-1]:
        node = node.children.setdefault(p, FatDir())
    node.children[parts[-1]] = data


def is_short(name):
    if name in ('.', '..'):
        return True
    base, dot, ext = name.partition('.')
    if not base or len(base) > 8 or len(ext) > 3 or '.' in ext or (dot and not ext):
        return False
    return all(c in SHORT_OK for c in base + ext)


def pack_short(name):
    base, _, ext = name.partition('.')
    return (base.ljust(8) + ext.ljust(3)).encode('ascii')


def short_alias(name, used):
    if is_short(name.upper()) and pack_short(name.upper()).decode('ascii') not in used:
        return pack_short(name.upper())
    base, _, ext = name.upper().rpartition('.') if '.' in name.strip('.') else (name.upper(), '', '')
    base = ''.join(c for c in base if c in SHORT_OK) or 'X'
    ext = ''.join(c for c in ext if c in SHORT_OK)[:3]
    for n in range(1, 1000000):
        tail = '~%d' % n
        cand = (base[:8 - len(tail)] + tail).ljust(8) + ext.ljust(3)
        if cand not in used:
            return cand.encode('ascii')
    raise SystemExit('mkimage: nomes curtos esgotados')


def lfn_checksum(short):
    s = 0
    for b in short:
        s = (((s & 1) << 7) + (s >> 1) + b) & 0xFF
    return s


def lfn_entries(name, short):
    units = list(name.encode('utf-16-le'))
    chars = [units[i] | units[i + 1] << 8 for i in range(0, len(units), 2)]
    chars.append(0)
    while len(chars) % 13:
        chars.append(0xFFFF)
    if chars[-13:] == [0] + [0xFFFF] * 12 and len(name) % 13 == 0:
        chars = chars[:-13]
    pieces = [chars[i:i + 13] for i in range(0, len(chars), 13)]
    csum = lfn_checksum(short)
    out = []
    for i in range(len(pieces), 0, -1):
        c = pieces[i - 1]
        seq = i | (0x40 if i == len(pieces) else 0)
        e = struct.pack('<B5HBBB6HH2H', seq, *c[0:5], 0x0F, 0, csum, *c[5:11], 0, *c[11:13])
        out.append(e)
    return out


def fat_time(t):
    tm = time.gmtime(t)
    year = max(tm.tm_year, 1980)
    date = ((year - 1980) << 9) | (tm.tm_mon << 5) | tm.tm_mday
    clock = (tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec // 2)
    return date, clock


class Fat16:
    def __init__(self, size, label, hidden, now):
        self.total = size // SECTOR
        self.spc = 4
        self.reserved = 1
        self.root_entries = 512
        self.root_sectors = self.root_entries * 32 // SECTOR
        fatsz = 1
        while True:
            data = self.total - self.reserved - self.root_sectors - 2 * fatsz
            clusters = data // self.spc
            need = ((clusters + 2) * 2 + SECTOR - 1) // SECTOR
            if need <= fatsz:
                break
            fatsz = need
        self.fatsz = fatsz
        self.clusters = clusters
        if not 4085 <= clusters <= 65524:
            raise SystemExit('mkimage: tamanho de FAT16 invalido (%d clusters)' % clusters)
        self.cbytes = self.spc * SECTOR
        self.label = label
        self.hidden = hidden
        self.date, self.clock = fat_time(now)
        self.fat = [0] * (clusters + 2)
        self.fat[0] = 0xFFF8
        self.fat[1] = 0xFFFF
        self.next = 2
        self.img = bytearray(self.total * SECTOR)
        self.root_off = (self.reserved + 2 * fatsz) * SECTOR
        self.data_off = self.root_off + self.root_sectors * SECTOR

    def alloc(self, nbytes):
        n = (nbytes + self.cbytes - 1) // self.cbytes
        if n == 0:
            return 0
        if self.next + n > self.clusters + 2:
            raise SystemExit('mkimage: a particao EFI encheu')
        first = self.next
        for c in range(first, first + n):
            self.fat[c] = c + 1
        self.fat[first + n - 1] = 0xFFFF
        self.next += n
        return first

    def write_chain(self, first, data):
        off = self.data_off + (first - 2) * self.cbytes
        self.img[off:off + len(data)] = data

    def entry(self, short, attr, cluster, size):
        return struct.pack('<11sBBBHHHHHHHI', short, attr, 0, 0, self.clock, self.date, self.date, cluster >> 16,
                           self.clock, self.date, cluster & 0xFFFF, size)

    def dir_bytes(self, node, me, parent):
        out = []
        if me is not None:
            out.append(self.entry(b'.          ', 0x10, me, 0))
            out.append(self.entry(b'..         ', 0x10, parent, 0))
        else:
            out.append(struct.pack('<11sB20x', self.label.upper().ljust(11)[:11].encode('ascii'), 0x08))
        used = set()
        for name in sorted(node.children):
            if is_short(name):
                used.add(pack_short(name).decode('ascii'))
        for name in sorted(node.children):
            child = node.children[name]
            if is_short(name):
                short = pack_short(name)
            else:
                short = short_alias(name, used)
                used.add(short.decode('ascii'))
                out.extend(lfn_entries(name, short))
            if isinstance(child, FatDir):
                out.append(self.entry(short, 0x10, child.cluster, 0))
            else:
                out.append(self.entry(short, 0x20, child[0], len(child[1])))
        return b''.join(out)

    def count_entries(self, node, is_root):
        n = 1 if is_root else 2
        for name in node.children:
            n += 1 if is_short(name) else 1 + (len(name) + 12) // 13
        return n

    def layout(self, node, is_root):
        for name in sorted(node.children):
            child = node.children[name]
            if isinstance(child, FatDir):
                child.cluster = self.alloc(self.count_entries(child, False) * 32)
            else:
                node.children[name] = (self.alloc(len(child)), child)
        for name in sorted(node.children):
            child = node.children[name]
            if isinstance(child, FatDir):
                self.layout(child, False)

    def emit(self, node, me, parent):
        data = self.dir_bytes(node, me, parent)
        if me is None:
            if len(data) > self.root_sectors * SECTOR:
                raise SystemExit('mkimage: raiz da FAT cheia')
            self.img[self.root_off:self.root_off + len(data)] = data
        else:
            self.write_chain(me, data)
        for name in sorted(node.children):
            child = node.children[name]
            if isinstance(child, FatDir):
                self.emit(child, child.cluster, me or 0)
            elif child[0]:
                self.write_chain(child[0], child[1])

    def build(self, root):
        self.layout(root, True)
        self.emit(root, None, 0)
        bs = bytearray(SECTOR)
        bs[0:3] = b'\xEB\x3C\x90'
        bs[3:11] = b'ORION   '
        total16 = self.total if self.total < 65536 else 0
        total32 = self.total if self.total >= 65536 else 0
        struct.pack_into('<HBHBHHBHHHII', bs, 11, SECTOR, self.spc, self.reserved, 2, self.root_entries, total16,
                         0xF8, self.fatsz, 63, 255, self.hidden, total32)
        bs[36] = 0x80
        bs[38] = 0x29
        bs[39:43] = struct.pack('<I', zlib.crc32(self.label.encode()) & 0xFFFFFFFF)
        bs[43:54] = self.label.upper().ljust(11)[:11].encode('ascii')
        bs[54:62] = b'FAT16   '
        bs[510:512] = b'\x55\xAA'
        self.img[0:SECTOR] = bs
        fat = struct.pack('<%dH' % len(self.fat), *self.fat)
        for k in range(2):
            off = (self.reserved + k * self.fatsz) * SECTOR
            self.img[off:off + len(fat)] = fat
        return self.img


def esp_tree(esp):
    root = FatDir()
    for rel in ('EFI/BOOT/BOOTX64.EFI', 'boot/limine/limine.conf', 'boot/orion.elf'):
        path = os.path.join(esp, *rel.split('/'))
        if not os.path.isfile(path):
            raise SystemExit('mkimage: falta %s' % path)
        with open(path, 'rb') as fh:
            fat_add(root, rel, fh.read())
    return root


def root_tree(esp):
    root = mkext2.Node('dir', 0o755)
    for top in sorted(os.listdir(esp)):
        full = os.path.join(esp, top)
        if top in ROOT_SKIP or top.startswith('.') or not os.path.isdir(full):
            continue
        for dirpath, dirnames, filenames in os.walk(full):
            dirnames[:] = [d for d in dirnames if not d.startswith('.')]
            rel = os.path.relpath(dirpath, esp).replace('\\', '/')
            mkext2.add_path(root, '/' + rel, mkext2.Node('dir', 0o755))
            for fn in filenames:
                with open(os.path.join(dirpath, fn), 'rb') as fh:
                    data = fh.read()
                mode = 0o755 if data[:4] == b'\x7fELF' else 0o644
                mode = SPECIAL_MODES.get(rel + '/' + fn, mode)
                mkext2.add_path(root, '/' + rel + '/' + fn, mkext2.Node('file', mode, data))
    for name, mode in MOUNT_POINTS.items():
        if name not in root.children:
            root.children[name] = mkext2.Node('dir', mode)
    mkext2.add_path(root, '/boot/efi', mkext2.Node('dir', 0o755))
    root.children.setdefault('lost+found', mkext2.Node('dir', 0o700))
    for path, (uid, gid) in SPECIAL_OWNERS.items():
        parent = root
        for part in path.split('/'):
            parent = parent.children.setdefault(part, mkext2.Node('dir', 0o755))
        node = root
        for part in path.split('/'):
            node = node.children.get(part) if node else None
        if node:
            node.uid, node.gid = uid, gid
    return root


def tree_bytes(node):
    if node.kind == 'file':
        return len(node.data)
    return 4096 + sum(tree_bytes(c) for c in node.children.values())


def gpt_entry(ptype, first, last, name):
    raw = name.encode('utf-16-le')[:72]
    return struct.pack('<16s16sQQQ72s', ptype.bytes_le, uuid.uuid4().bytes_le, first, last, 0, raw)


def gpt_header(my, alt, first_usable, last_usable, disk_guid, entries_lba, entries_crc):
    h = struct.pack('<8sIIIIQQQQ16sQIII', b'EFI PART', 0x00010000, 92, 0, 0, my, alt, first_usable, last_usable,
                    disk_guid, entries_lba, GPT_ENTRIES, GPT_ENTRY_SIZE, entries_crc)
    crc = zlib.crc32(h) & 0xFFFFFFFF
    h = h[:16] + struct.pack('<I', crc) + h[20:]
    return h + bytes(SECTOR - len(h))


def main():
    ap = argparse.ArgumentParser(description='Gera o disco do Orion: GPT com particao EFI (FAT16) e raiz (ext2).')
    ap.add_argument('out')
    ap.add_argument('--esp', default='out/esp', help='diretorio com o que o build gerou')
    ap.add_argument('--esp-size', type=int, default=64, help='MiB da particao EFI')
    ap.add_argument('--root-size', type=int, default=0, help='MiB da raiz (0 = o conteudo mais folga)')
    a = ap.parse_args()

    now = int(time.time())
    esp_size = a.esp_size * MIB
    tree = root_tree(a.esp)
    root_size = a.root_size * MIB
    if not root_size:
        need = tree_bytes(tree) * 3 // 2 + 160 * MIB
        root_size = max(256 * MIB, (need + MIB - 1) // MIB * MIB)

    esp_start = MIB // SECTOR
    esp_sectors = esp_size // SECTOR
    root_start = esp_start + esp_sectors
    root_sectors = root_size // SECTOR
    total = root_start + root_sectors + MIB // SECTOR
    last = total - 1

    esp = Fat16(esp_size, 'ORION-EFI', esp_start, now).build(esp_tree(a.esp))
    img = mkext2.Image(root_size, 4096, 'orion-raiz', 8192)
    mkext2.build(img, tree)

    entries = gpt_entry(ESP_TYPE, esp_start, root_start - 1, 'EFI system partition')
    entries += gpt_entry(ROOT_TYPE, root_start, root_start + root_sectors - 1, 'Orion raiz')
    entries += bytes(GPT_ENTRIES * GPT_ENTRY_SIZE - len(entries))
    ecrc = zlib.crc32(entries) & 0xFFFFFFFF
    disk_guid = uuid.uuid4().bytes_le
    first_usable = 2 + GPT_ENTRY_SECTORS
    last_usable = last - 1 - GPT_ENTRY_SECTORS
    primary = gpt_header(1, last, first_usable, last_usable, disk_guid, 2, ecrc)
    backup = gpt_header(last, 1, first_usable, last_usable, disk_guid, last - GPT_ENTRY_SECTORS, ecrc)

    mbr = bytearray(SECTOR)
    struct.pack_into('<B3sB3sII', mbr, 446, 0, b'\x00\x02\x00', 0xEE, b'\xFF\xFF\xFF', 1, min(last, 0xFFFFFFFF))
    mbr[510:512] = b'\x55\xAA'

    tmp = a.out + '.tmp'
    with open(tmp, 'wb') as fh:
        fh.truncate(total * SECTOR)
        for lba, data in ((0, mbr), (1, primary), (2, entries), (esp_start, esp), (root_start, img.img),
                          (last - GPT_ENTRY_SECTORS, entries), (last, backup)):
            fh.seek(lba * SECTOR)
            fh.write(data)
    os.replace(tmp, a.out)
    print('mkimage: %s, %d MiB (EFI %d MiB em FAT16, raiz %d MiB em ext2, %d inodes)' % (
        a.out, total * SECTOR // MIB, a.esp_size, root_size // MIB, img.inodes))


if __name__ == '__main__':
    sys.exit(main())
