import argparse
import os
import struct
import sys
import zlib

EPOCH = 1792000000
ROOT_INO = 2
FIRST_INO = 11
INODE_SIZE = 128
S_IFDIR = 0o040000
S_IFREG = 0o100000
S_IFLNK = 0o120000
FT_REG, FT_DIR, FT_LNK = 1, 2, 7


class Node:
    def __init__(self, kind, mode, data=b'', target=b''):
        self.kind = kind
        self.mode = mode
        self.data = data
        self.target = target
        self.children = {}
        self.ino = 0
        self.uid = 0
        self.gid = 0


def has_super(group):
    if group in (0, 1):
        return True
    for base in (3, 5, 7):
        n = base
        while n < group:
            n *= base
        if n == group:
            return True
    return False


class Image:
    def __init__(self, size_bytes, bs, label, inodes_per_group):
        self.bs = bs
        self.blocks = size_bytes // bs
        self.first_data = 1 if bs == 1024 else 0
        self.bpg = 8 * bs
        self.groups = (self.blocks - self.first_data + self.bpg - 1) // self.bpg
        self.ipg = inodes_per_group
        self.inodes = self.ipg * self.groups
        self.itable_blocks = (self.ipg * INODE_SIZE + bs - 1) // bs
        self.gdt_blocks = (self.groups * 32 + bs - 1) // bs
        self.label = label.encode()[:16]
        self.img = bytearray(self.blocks * bs)
        self.block_used = bytearray(self.blocks)
        self.inode_used = bytearray(self.inodes + 1)
        self.layout = []
        for g in range(self.groups):
            start = self.first_data + g * self.bpg
            end = min(start + self.bpg, self.blocks)
            cur = start
            if has_super(g):
                for b in range(cur, cur + 1 + self.gdt_blocks):
                    self.block_used[b] = 1
                cur += 1 + self.gdt_blocks
            bb, ib, it = cur, cur + 1, cur + 2
            if it + self.itable_blocks > end:
                raise SystemExit('grupo %d pequeno demais para a tabela de inodes' % g)
            for b in range(bb, it + self.itable_blocks):
                self.block_used[b] = 1
            self.layout.append((start, end, bb, ib, it))
        for b in range(self.first_data):
            self.block_used[b] = 1
        for i in range(1, FIRST_INO):
            self.inode_used[i] = 1
        self.next_block = 0

    def alloc_block(self):
        while self.next_block < self.blocks and self.block_used[self.next_block]:
            self.next_block += 1
        if self.next_block >= self.blocks:
            raise SystemExit('imagem ext2 sem espaco')
        b = self.next_block
        self.block_used[b] = 1
        return b

    def alloc_inode(self):
        for i in range(FIRST_INO, self.inodes + 1):
            if not self.inode_used[i]:
                self.inode_used[i] = 1
                return i
        raise SystemExit('imagem ext2 sem inodes')

    def write_block(self, b, data):
        assert len(data) <= self.bs
        self.img[b * self.bs:b * self.bs + len(data)] = data

    def map_blocks(self, data, sparse):
        bs = self.bs
        nblocks = (len(data) + bs - 1) // bs
        ptrs = []
        for n in range(nblocks):
            chunk = data[n * bs:(n + 1) * bs]
            if sparse and chunk.count(0) == len(chunk):
                ptrs.append(0)
                continue
            b = self.alloc_block()
            self.write_block(b, chunk)
            ptrs.append(b)
        return ptrs

    def build_tree(self, ptrs):
        per = self.bs // 4
        block = [0] * 15
        used = 0

        def table(entries):
            nonlocal used
            if not any(entries):
                return 0
            b = self.alloc_block()
            used += 1
            self.write_block(b, struct.pack('<%dI' % per, *(entries + [0] * (per - len(entries)))))
            return b

        def level(items, depth):
            if depth == 1:
                return table(items)
            span = per ** (depth - 1)
            subs = [level(items[i:i + span], depth - 1) for i in range(0, len(items), span)]
            return table(subs)

        for i in range(min(12, len(ptrs))):
            block[i] = ptrs[i]
        rest = ptrs[12:]
        for slot, depth in ((12, 1), (13, 2), (14, 3)):
            span = per ** depth
            chunk, rest = rest[:span], rest[span:]
            if chunk:
                block[slot] = level(chunk, depth)
        if rest:
            raise SystemExit('arquivo grande demais para a imagem')
        data_blocks = sum(1 for p in ptrs if p)
        return block, (data_blocks + used) * (self.bs // 512)

    def write_inode(self, ino, mode, size, links, block, sectors, flags=0, uid=0, gid=0):
        g, idx = (ino - 1) // self.ipg, (ino - 1) % self.ipg
        off = self.layout[g][4] * self.bs + idx * INODE_SIZE
        raw = struct.pack('<HHIIIIIHHIII', mode, uid & 0xFFFF, size & 0xFFFFFFFF, EPOCH, EPOCH, EPOCH, 0, gid & 0xFFFF, links,
                          sectors, flags, 0)
        raw += struct.pack('<15I', *block)
        raw += struct.pack('<IIII', 0, 0, (size >> 32) if (mode & 0o170000) == S_IFREG else 0, 0)
        raw += bytes(INODE_SIZE - len(raw))
        self.img[off:off + INODE_SIZE] = raw

    def dir_data(self, entries):
        bs = self.bs
        blocks = []
        cur = bytearray()
        last = None
        for name, ino, ft in entries:
            nb = name.encode()
            need = (8 + len(nb) + 3) & ~3
            if len(cur) + need > bs:
                lino, lname, lft, loff = last
                rec = bs - loff
                cur[loff:loff + 8] = struct.pack('<IHBB', lino, rec, len(lname), lft)
                cur += bytes(bs - len(cur))
                blocks.append(bytes(cur))
                cur = bytearray()
            last = (ino, nb, ft, len(cur))
            cur += struct.pack('<IHBB', ino, need, len(nb), ft) + nb + bytes(need - 8 - len(nb))
        lino, lname, lft, loff = last
        cur[loff:loff + 8] = struct.pack('<IHBB', lino, bs - loff, len(lname), lft)
        cur += bytes(bs - len(cur))
        blocks.append(bytes(cur))
        return b''.join(blocks)

    def emit(self, node, parent_ino):
        if node.kind == 'dir':
            subdirs = sum(1 for c in node.children.values() if c.kind == 'dir')
            for c in node.children.values():
                if not c.ino:
                    c.ino = self.alloc_inode()
            entries = [('.', node.ino, FT_DIR), ('..', parent_ino, FT_DIR)]
            for name in sorted(node.children):
                c = node.children[name]
                ft = {'dir': FT_DIR, 'file': FT_REG, 'link': FT_LNK}[c.kind]
                entries.append((name, c.ino, ft))
            data = self.dir_data(entries)
            block, sectors = self.build_tree(self.map_blocks(data, False))
            self.write_inode(node.ino, S_IFDIR | node.mode, len(data), 2 + subdirs, block, sectors,
                             uid=node.uid, gid=node.gid)
            for name in sorted(node.children):
                self.emit(node.children[name], node.ino)
        elif node.kind == 'file':
            block, sectors = self.build_tree(self.map_blocks(node.data, True))
            self.write_inode(node.ino, S_IFREG | node.mode, len(node.data), 1, block, sectors,
                             uid=node.uid, gid=node.gid)
        else:
            t = node.target
            if len(t) < 60:
                raw = t + bytes(60 - len(t))
                block = list(struct.unpack('<15I', raw))
                self.write_inode(node.ino, S_IFLNK | 0o777, len(t), 1, block, 0)
            else:
                block, sectors = self.build_tree(self.map_blocks(t, False))
                self.write_inode(node.ino, S_IFLNK | 0o777, len(t), 1, block, sectors)

    def finish(self):
        bs = self.bs
        free_blocks_total = 0
        free_inodes_total = 0
        gdt = bytearray()
        dirs = self.dir_count
        for g, (start, end, bb, ib, it) in enumerate(self.layout):
            bmap = bytearray(bs)
            free_b = 0
            for i in range(self.bpg):
                b = start + i
                if b >= end or self.block_used[b]:
                    bmap[i // 8] |= 1 << (i % 8)
                else:
                    free_b += 1
            imap = bytearray(bs)
            free_i = 0
            for i in range(self.ipg):
                ino = g * self.ipg + i + 1
                if self.inode_used[ino]:
                    imap[i // 8] |= 1 << (i % 8)
                else:
                    free_i += 1
            for i in range(self.ipg, bs * 8):
                imap[i // 8] |= 1 << (i % 8)
            self.write_block(bb, bmap)
            self.write_block(ib, imap)
            used_dirs = dirs.get(g, 0)
            gdt += struct.pack('<IIIHHH14x', bb, ib, it, free_b, free_i, used_dirs)
            free_blocks_total += free_b
            free_inodes_total += free_i
        sb = struct.pack('<13I6H4I2H', self.inodes, self.blocks, 0, free_blocks_total, free_inodes_total,
                         self.first_data, (bs >> 10).bit_length() - 1, (bs >> 10).bit_length() - 1, self.bpg,
                         self.bpg, self.ipg, EPOCH, EPOCH, 0, 0xFFFF, 0xEF53, 1, 1, 0, EPOCH, 0, 0, 1, 0, 0)
        sb += struct.pack('<IHHIII', FIRST_INO, INODE_SIZE, 0, 0, 0x0002, 0x0003)
        sb += bytes(16) + self.label + bytes(16 - len(self.label))
        sb += bytes(1024 - len(sb))
        for g, (start, end, bb, ib, it) in enumerate(self.layout):
            if not has_super(g):
                continue
            gsb = bytearray(sb)
            struct.pack_into('<H', gsb, 90, g)
            sb_off = 1024 if g == 0 else start * bs
            self.img[sb_off:sb_off + 1024] = gsb
            gdt_off = (start + 1) * bs
            self.img[gdt_off:gdt_off + len(gdt)] = gdt


def build(img, root):
    root.ino = ROOT_INO
    img.inode_used[ROOT_INO] = 1
    img.dir_count = {}

    def count(node):
        if node.kind == 'dir':
            g = (node.ino - 1) // img.ipg if node.ino else 0
            img.dir_count[g] = img.dir_count.get(g, 0) + 1
            for c in node.children.values():
                count(c)

    img.emit(root, ROOT_INO)
    count(root)
    img.finish()


def add_path(root, path, node):
    parts = [p for p in path.strip('/').split('/') if p]
    cur = root
    for p in parts[:-1]:
        if p not in cur.children:
            cur.children[p] = Node('dir', 0o755)
        cur = cur.children[p]
    cur.children[parts[-1]] = node


def pattern(n, seed):
    out = bytearray(n)
    x = seed & 0xFFFFFFFF
    for i in range(0, n, 4):
        x = (x * 1103515245 + 12345) & 0xFFFFFFFF
        out[i:i + 4] = struct.pack('<I', x)[:min(4, n - i)]
    return bytes(out)


def test_tree(extra):
    root = Node('dir', 0o755)
    files = {}

    def f(path, data):
        add_path(root, path, Node('file', 0o755 if data[:4] == b'\x7fELF' else 0o644, data))
        files[path] = data

    f('/dir/a.txt', b'ola ext2\n')
    f('/dir/sub/deep/arquivo.txt', b'bem fundo no ext2\n')
    f('/empty', b'')
    f('/exact', pattern(4096, 7))
    f('/big.bin', pattern(5 * 1024 * 1024 + 123, 42))
    sparse = bytearray(3 * 1024 * 1024)
    sparse[:5000] = pattern(5000, 1)
    sparse[-7000:] = pattern(7000, 2)
    f('/sparse.bin', bytes(sparse))
    for i in range(200):
        f('/many/arquivo-%03d' % i, ('conteudo %d\n' % i).encode())
    huge = bytearray(70 * 1024 * 1024)
    huge[-4096:] = pattern(4096, 9)
    add_path(root, '/huge-sparse.bin', Node('file', 0o644, bytes(huge)))
    tail = bytes(huge[-4096:])
    for dest, src in extra:
        with open(src, 'rb') as fh:
            f(dest, fh.read())
    add_path(root, '/link', Node('link', 0o777, target=b'dir/a.txt'))
    long_target = ('dir/' + 'x' * 70 + '/destino').encode()
    add_path(root, '/longlink', Node('link', 0o777, target=long_target))
    root.children['lost+found'] = Node('dir', 0o700)
    lines = ['%s %d %08x' % (p, len(d), zlib.crc32(d) & 0xFFFFFFFF) for p, d in sorted(files.items())]
    lines.append('@link link dir/a.txt')
    lines.append('@link longlink %s' % long_target.decode())
    lines.append('@count many 200')
    lines.append('@tail huge-sparse.bin %d %08x' % (len(huge), zlib.crc32(tail) & 0xFFFFFFFF))
    manifest = ('\n'.join(lines) + '\n').encode()
    add_path(root, '/MANIFEST', Node('file', 0o644, manifest))
    return root


def tree_from_dir(path):
    root = Node('dir', 0o755)
    for dirpath, dirnames, filenames in os.walk(path):
        rel = os.path.relpath(dirpath, path).replace('\\', '/')
        for d in dirnames:
            p = '/' + (d if rel == '.' else rel + '/' + d)
            add_path(root, p, Node('dir', 0o755))
        for fn in filenames:
            p = '/' + (fn if rel == '.' else rel + '/' + fn)
            with open(os.path.join(dirpath, fn), 'rb') as fh:
                add_path(root, p, Node('file', 0o755, fh.read()))
    root.children.setdefault('lost+found', Node('dir', 0o700))
    return root


def main():
    ap = argparse.ArgumentParser(description='Cria uma imagem ext2 (rev 1) a partir de um diretorio ou da arvore de teste.')
    ap.add_argument('out')
    ap.add_argument('--size', type=int, default=32, help='tamanho em MiB')
    ap.add_argument('--block', type=int, default=4096, choices=(1024, 2048, 4096))
    ap.add_argument('--inodes-per-group', type=int, default=1024)
    ap.add_argument('--label', default='ORION')
    ap.add_argument('--from-dir')
    ap.add_argument('--test', action='store_true')
    ap.add_argument('--add', action='append', default=[], help='destino=origem (com --test)')
    a = ap.parse_args()
    if a.test:
        extra = [tuple(x.split('=', 1)) for x in a.add]
        root = test_tree(extra)
    elif a.from_dir:
        root = tree_from_dir(a.from_dir)
    else:
        root = Node('dir', 0o755)
        root.children['lost+found'] = Node('dir', 0o700)
    img = Image(a.size * 1024 * 1024, a.block, a.label, a.inodes_per_group)
    build(img, root)
    tmp = a.out + '.tmp'
    with open(tmp, 'wb') as fh:
        fh.write(img.img)
    os.replace(tmp, a.out)
    print('mkext2: %s, %d blocos de %d bytes, %d grupo(s), %d inodes' % (a.out, img.blocks, a.block, img.groups,
                                                                          img.inodes))


if __name__ == '__main__':
    sys.exit(main())
