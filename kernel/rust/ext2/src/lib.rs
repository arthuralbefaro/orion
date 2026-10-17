#![cfg_attr(not(test), no_std)]
#![allow(clippy::missing_safety_doc)]

extern crate alloc;

#[cfg(not(test))]
mod kernel;
#[cfg(test)]
mod tests;

use alloc::boxed::Box;
use alloc::vec::Vec;
use core::ffi::c_void;

const EIO: i32 = 5;
const ENOENT: i32 = 2;
const ENOMEM: i32 = 12;
const ENOTDIR: i32 = 20;
const EINVAL: i32 = 22;
const EFBIG: i32 = 27;
const ENOSPC: i32 = 28;
const EROFS: i32 = 30;
const ENAMETOOLONG: i32 = 36;

const EXT2_MAGIC: u16 = 0xEF53;
const ROOT_INO: u32 = 2;
const N_DIRECT: u64 = 12;
const MAX_GROUPS: u32 = 65536;
const MIN_BLOCKS_PER_GROUP: u32 = 256;
const MAX_DIR_BYTES: u64 = 16 * 1024 * 1024;
const MAX_DIR_ENTRIES: u32 = 65536;
const INCOMPAT_FILETYPE: u32 = 0x0002;
const INCOMPAT_RECOVER: u32 = 0x0004;
const INCOMPAT_SUPPORTED: u32 = INCOMPAT_FILETYPE;
const RO_COMPAT_SPARSE_SUPER: u32 = 0x0001;
const RO_COMPAT_LARGE_FILE: u32 = 0x0002;
const RO_COMPAT_WRITABLE: u32 = RO_COMPAT_SPARSE_SUPER | RO_COMPAT_LARGE_FILE;
const STATE_VALID: u16 = 0x0001;
const STATE_ERRORS: u16 = 0x0002;

const S_IFMT: u32 = 0o170000;
const S_IFDIR: u32 = 0o040000;
const S_IFLNK: u32 = 0o120000;
const S_IFREG: u32 = 0o100000;

pub type ReadFn = unsafe extern "C" fn(ctx: *mut c_void, off: u64, buf: *mut u8, len: u32, meta: bool) -> i32;
pub type WriteFn = unsafe extern "C" fn(ctx: *mut c_void, off: u64, buf: *const u8, len: u32) -> i32;
pub type DirFn =
    unsafe extern "C" fn(ctx: *mut c_void, name: *const u8, len: u32, ino: u32, dtype: u8, next: u64) -> bool;

#[repr(C)]
pub struct Ext2Info {
    pub block_size: u32,
    pub blocks: u32,
    pub free_blocks: u32,
    pub inodes: u32,
    pub free_inodes: u32,
    pub groups: u32,
    pub rev: u32,
    pub inode_size: u32,
    pub writable: u32,
    pub label: [u8; 17],
}

#[repr(C)]
pub struct Ext2Stat {
    pub mode: u32,
    pub uid: u32,
    pub gid: u32,
    pub nlink: u32,
    pub size: u64,
    pub atime: u32,
    pub mtime: u32,
    pub ctime: u32,
    pub blocks: u32,
}

struct Group {
    block_bitmap: u32,
    inode_bitmap: u32,
    inode_table: u32,
    free_blocks: u32,
    free_inodes: u32,
}

pub struct Fs {
    ctx: *mut c_void,
    read: ReadFn,
    write: Option<WriteFn>,
    block_size: u32,
    blocks: u32,
    inodes: u32,
    first_data_block: u32,
    blocks_per_group: u32,
    inodes_per_group: u32,
    inode_size: u32,
    gdt_off: u64,
    gdt_blocks: u32,
    free_blocks: u32,
    free_inodes: u32,
    filetype: bool,
    large_file: bool,
    dirty: bool,
    groups: Vec<Group>,
}

struct Inode {
    mode: u32,
    uid: u32,
    gid: u32,
    nlink: u32,
    size: u64,
    atime: u32,
    mtime: u32,
    ctime: u32,
    sectors: u32,
    file_acl: u32,
    block: [u32; 15],
}

fn le16(b: &[u8], off: usize) -> u16 {
    u16::from_le_bytes([b[off], b[off + 1]])
}

fn le32(b: &[u8], off: usize) -> u32 {
    u32::from_le_bytes([b[off], b[off + 1], b[off + 2], b[off + 3]])
}

fn put16(b: &mut [u8], off: usize, v: u16) {
    b[off..off + 2].copy_from_slice(&v.to_le_bytes());
}

fn put32(b: &mut [u8], off: usize, v: u32) {
    b[off..off + 4].copy_from_slice(&v.to_le_bytes());
}

fn zeroed(len: usize) -> Result<Vec<u8>, i32> {
    let mut v = Vec::new();
    if v.try_reserve_exact(len).is_err() {
        return Err(ENOMEM);
    }
    v.resize(len, 0u8);
    Ok(v)
}

impl Fs {
    fn read_at(&self, off: u64, buf: &mut [u8], meta: bool) -> Result<(), i32> {
        let r = unsafe { (self.read)(self.ctx, off, buf.as_mut_ptr(), buf.len() as u32, meta) };
        if r < 0 {
            Err(-r)
        } else {
            Ok(())
        }
    }

    fn write_at(&self, off: u64, buf: &[u8]) -> Result<(), i32> {
        let write = self.write.ok_or(EROFS)?;
        let r = unsafe { write(self.ctx, off, buf.as_ptr(), buf.len() as u32) };
        if r < 0 {
            Err(-r)
        } else {
            Ok(())
        }
    }

    fn read_block(&self, block: u32, buf: &mut [u8], meta: bool) -> Result<(), i32> {
        if block >= self.blocks || buf.len() != self.block_size as usize {
            return Err(EIO);
        }
        self.read_at(block as u64 * self.block_size as u64, buf, meta)
    }

    fn write_block(&self, block: u32, buf: &[u8]) -> Result<(), i32> {
        if block == 0 || block >= self.blocks || buf.len() != self.block_size as usize {
            return Err(EIO);
        }
        self.write_at(block as u64 * self.block_size as u64, buf)
    }

    fn sectors_per_block(&self) -> u32 {
        self.block_size / 512
    }

    fn inode_off(&self, ino: u32) -> Result<u64, i32> {
        if ino == 0 || ino > self.inodes {
            return Err(ENOENT);
        }
        let group = ((ino - 1) / self.inodes_per_group) as usize;
        let index = (ino - 1) % self.inodes_per_group;
        let g = self.groups.get(group).ok_or(EIO)?;
        let table_bytes = self.inodes_per_group as u64 * self.inode_size as u64;
        let table_blocks = table_bytes.div_ceil(self.block_size as u64);
        if g.inode_table as u64 + table_blocks > self.blocks as u64 {
            return Err(EIO);
        }
        Ok(g.inode_table as u64 * self.block_size as u64 + index as u64 * self.inode_size as u64)
    }

    fn inode(&self, ino: u32) -> Result<Inode, i32> {
        let off = self.inode_off(ino)?;
        let mut raw = [0u8; 128];
        self.read_at(off, &mut raw, true)?;
        let mode = le16(&raw, 0) as u32;
        let mut size = le32(&raw, 4) as u64;
        if self.large_file && (mode & S_IFMT) == S_IFREG {
            size |= (le32(&raw, 108) as u64) << 32;
            if size > self.max_file_size() {
                return Err(EIO);
            }
        }
        let mut block = [0u32; 15];
        for (i, b) in block.iter_mut().enumerate() {
            *b = le32(&raw, 40 + i * 4);
        }
        Ok(Inode {
            mode,
            uid: le16(&raw, 2) as u32 | (le16(&raw, 120) as u32) << 16,
            gid: le16(&raw, 24) as u32 | (le16(&raw, 122) as u32) << 16,
            nlink: le16(&raw, 26) as u32,
            size,
            atime: le32(&raw, 8),
            ctime: le32(&raw, 12),
            mtime: le32(&raw, 16),
            sectors: le32(&raw, 28),
            file_acl: le32(&raw, 104),
            block,
        })
    }

    fn write_inode(&self, ino: u32, i: &Inode) -> Result<(), i32> {
        let off = self.inode_off(ino)?;
        let mut raw = [0u8; 128];
        self.read_at(off, &mut raw, true)?;
        put32(&mut raw, 4, i.size as u32);
        if (i.mode & S_IFMT) == S_IFREG && self.large_file {
            put32(&mut raw, 108, (i.size >> 32) as u32);
        }
        put32(&mut raw, 8, i.atime);
        put32(&mut raw, 12, i.ctime);
        put32(&mut raw, 16, i.mtime);
        put32(&mut raw, 28, i.sectors);
        for (k, b) in i.block.iter().enumerate() {
            put32(&mut raw, 40 + k * 4, *b);
        }
        self.write_at(off, &raw)
    }

    fn max_file_size(&self) -> u64 {
        let per = self.block_size as u64 / 4;
        let blocks = N_DIRECT + per + per * per + per * per * per;
        core::cmp::min(blocks.saturating_mul(self.block_size as u64), i64::MAX as u64)
    }

    fn write_limit(&self) -> u64 {
        if self.large_file {
            self.max_file_size()
        } else {
            core::cmp::min(self.max_file_size(), u32::MAX as u64)
        }
    }

    fn indirect(&self, table: u32, index: u64) -> Result<u32, i32> {
        if table == 0 {
            return Ok(0);
        }
        if table >= self.blocks {
            return Err(EIO);
        }
        let mut entry = [0u8; 4];
        self.read_at(table as u64 * self.block_size as u64 + index * 4, &mut entry, true)?;
        let b = le32(&entry, 0);
        if b >= self.blocks {
            return Err(EIO);
        }
        Ok(b)
    }

    fn map(&self, ino: &Inode, n: u64) -> Result<u32, i32> {
        let per = self.block_size as u64 / 4;
        if n < N_DIRECT {
            let b = ino.block[n as usize];
            if b >= self.blocks {
                return Err(EIO);
            }
            return Ok(b);
        }
        let mut n = n - N_DIRECT;
        if n < per {
            return self.indirect(ino.block[12], n);
        }
        n -= per;
        if n < per * per {
            let mid = self.indirect(ino.block[13], n / per)?;
            return self.indirect(mid, n % per);
        }
        n -= per * per;
        if n < per * per * per {
            let top = self.indirect(ino.block[14], n / (per * per))?;
            let mid = self.indirect(top, (n / per) % per)?;
            return self.indirect(mid, n % per);
        }
        Err(EIO)
    }

    fn read_data(&self, ino: &Inode, off: u64, out: &mut [u8], meta: bool) -> Result<usize, i32> {
        if off >= ino.size {
            return Ok(0);
        }
        let len = core::cmp::min(out.len() as u64, ino.size - off) as usize;
        let bs = self.block_size as u64;
        let mut done = 0usize;
        while done < len {
            let pos = off + done as u64;
            let within = (pos % bs) as usize;
            let chunk = core::cmp::min(bs as usize - within, len - done);
            let b = self.map(ino, pos / bs)?;
            let dst = &mut out[done..done + chunk];
            if b == 0 {
                dst.fill(0);
            } else {
                self.read_at(b as u64 * bs + within as u64, dst, meta)?;
            }
            done += chunk;
        }
        Ok(done)
    }

    fn dir_walk<F: FnMut(&[u8], u32, u8, u64) -> bool>(&self, ino: &Inode, start: u64, mut f: F) -> Result<(), i32> {
        if (ino.mode & S_IFMT) != S_IFDIR {
            return Err(ENOTDIR);
        }
        if ino.size > MAX_DIR_BYTES {
            return Err(EIO);
        }
        let bs = self.block_size as u64;
        let mut block = zeroed(bs as usize)?;
        let mut pos = start - start % bs;
        let mut loaded = u64::MAX;
        let mut seen = 0u32;
        while pos < ino.size {
            seen += 1;
            if seen > MAX_DIR_ENTRIES {
                return Err(EIO);
            }
            let blk = pos / bs;
            if blk != loaded {
                let b = self.map(ino, blk)?;
                if b == 0 {
                    block.fill(0);
                } else {
                    self.read_block(b, &mut block, true)?;
                }
                loaded = blk;
            }
            let within = (pos % bs) as usize;
            if within + 8 > bs as usize {
                return Err(EIO);
            }
            let entry_ino = le32(&block, within);
            let rec_len = le16(&block, within + 4) as usize;
            let name_len = if self.filetype {
                block[within + 6] as usize
            } else {
                le16(&block, within + 6) as usize
            };
            let dtype = if self.filetype { block[within + 7] } else { 0 };
            if rec_len < 8 || !rec_len.is_multiple_of(4) || within + rec_len > bs as usize || name_len + 8 > rec_len {
                return Err(EIO);
            }
            let next = pos + rec_len as u64;
            let name = &block[within + 8..within + 8 + name_len];
            let valid = !name.is_empty() && !name.iter().any(|&c| c == b'/' || c == 0);
            if pos >= start && entry_ino != 0 && valid && !f(name, entry_ino, dtype, next) {
                return Ok(());
            }
            pos = next;
        }
        Ok(())
    }

    fn lookup(&self, dir: u32, name: &[u8]) -> Result<u32, i32> {
        let ino = self.inode(dir)?;
        let mut found = 0u32;
        self.dir_walk(&ino, 0, |n, i, _, _| {
            if n == name {
                found = i;
                false
            } else {
                true
            }
        })?;
        if found == 0 {
            return Err(ENOENT);
        }
        if found > self.inodes {
            return Err(EIO);
        }
        Ok(found)
    }

    fn group_blocks(&self, g: usize) -> u32 {
        let start = g as u64 * self.blocks_per_group as u64;
        let left = (self.blocks - self.first_data_block) as u64 - start;
        core::cmp::min(left, self.blocks_per_group as u64) as u32
    }

    fn inode_table_blocks(&self) -> u32 {
        (self.inodes_per_group as u64 * self.inode_size as u64).div_ceil(self.block_size as u64) as u32
    }

    fn is_meta(&self, b: u32) -> bool {
        if b <= self.first_data_block + self.gdt_blocks {
            return true;
        }
        let table = self.inode_table_blocks();
        self.groups.iter().any(|g| {
            b == g.block_bitmap || b == g.inode_bitmap || (b >= g.inode_table && b < g.inode_table + table)
        })
    }

    fn alloc_block(&mut self, goal: usize) -> Result<u32, i32> {
        let n = self.groups.len();
        let mut bitmap = zeroed(self.block_size as usize)?;
        for k in 0..n {
            let g = (goal + k) % n;
            if self.groups[g].free_blocks == 0 {
                continue;
            }
            self.read_block(self.groups[g].block_bitmap, &mut bitmap, true)?;
            let count = self.group_blocks(g);
            for i in 0..count {
                let byte = (i / 8) as usize;
                let bit = 1u8 << (i % 8);
                if bitmap[byte] & bit != 0 {
                    continue;
                }
                let b = self.first_data_block + g as u32 * self.blocks_per_group + i;
                if b >= self.blocks || self.is_meta(b) {
                    continue;
                }
                bitmap[byte] |= bit;
                self.write_block(self.groups[g].block_bitmap, &bitmap)?;
                self.groups[g].free_blocks -= 1;
                self.free_blocks = self.free_blocks.saturating_sub(1);
                self.dirty = true;
                return Ok(b);
            }
        }
        Err(ENOSPC)
    }

    fn free_block(&mut self, b: u32) -> Result<(), i32> {
        if b < self.first_data_block || b >= self.blocks || self.is_meta(b) {
            return Err(EIO);
        }
        let rel = b - self.first_data_block;
        let g = (rel / self.blocks_per_group) as usize;
        let i = rel % self.blocks_per_group;
        let mut bitmap = zeroed(self.block_size as usize)?;
        let bm = self.groups.get(g).ok_or(EIO)?.block_bitmap;
        self.read_block(bm, &mut bitmap, true)?;
        let byte = (i / 8) as usize;
        let bit = 1u8 << (i % 8);
        if bitmap[byte] & bit == 0 {
            return Err(EIO);
        }
        bitmap[byte] &= !bit;
        self.write_block(bm, &bitmap)?;
        self.groups[g].free_blocks += 1;
        self.free_blocks += 1;
        self.dirty = true;
        Ok(())
    }

    fn new_block(&mut self, goal: usize, zero: bool, sectors: &mut u32) -> Result<u32, i32> {
        let b = self.alloc_block(goal)?;
        if zero {
            let z = zeroed(self.block_size as usize)?;
            if let Err(e) = self.write_block(b, &z) {
                let _ = self.free_block(b);
                return Err(e);
            }
        }
        *sectors += self.sectors_per_block();
        Ok(b)
    }

    fn entry_alloc(&mut self, table: u32, idx: u64, zero: bool, goal: usize, sectors: &mut u32) -> Result<(u32, bool), i32> {
        if table == 0 || table >= self.blocks {
            return Err(EIO);
        }
        let off = table as u64 * self.block_size as u64 + idx * 4;
        let mut entry = [0u8; 4];
        self.read_at(off, &mut entry, true)?;
        let cur = le32(&entry, 0);
        if cur != 0 {
            if cur >= self.blocks {
                return Err(EIO);
            }
            return Ok((cur, false));
        }
        let b = self.new_block(goal, zero, sectors)?;
        if let Err(e) = self.write_at(off, &b.to_le_bytes()) {
            let _ = self.free_block(b);
            *sectors = sectors.saturating_sub(self.sectors_per_block());
            return Err(e);
        }
        Ok((b, true))
    }

    fn bmap_alloc(&mut self, ino: &mut Inode, n: u64, goal: usize) -> Result<(u32, bool), i32> {
        let per = self.block_size as u64 / 4;
        if n < N_DIRECT {
            let cur = ino.block[n as usize];
            if cur != 0 {
                if cur >= self.blocks {
                    return Err(EIO);
                }
                return Ok((cur, false));
            }
            let b = self.new_block(goal, false, &mut ino.sectors)?;
            ino.block[n as usize] = b;
            return Ok((b, true));
        }
        let mut rel = n - N_DIRECT;
        let mut depth = 1u32;
        let mut span = per;
        while rel >= span {
            rel -= span;
            depth += 1;
            if depth > 3 {
                return Err(EFBIG);
            }
            span *= per;
        }
        let slot = 11 + depth as usize;
        if ino.block[slot] == 0 {
            ino.block[slot] = self.new_block(goal, true, &mut ino.sectors)?;
        }
        let mut table = ino.block[slot];
        let mut unit = span / per;
        for level in 0..depth {
            let idx = rel / unit;
            rel %= unit;
            let last = level + 1 == depth;
            let (b, fresh) = self.entry_alloc(table, idx, !last, goal, &mut ino.sectors)?;
            if last {
                return Ok((b, fresh));
            }
            table = b;
            unit /= per;
        }
        Err(EIO)
    }

    fn free_tree(&mut self, table: u32, depth: u32, start: u64, sectors: &mut u32) -> Result<bool, i32> {
        if table == 0 || table >= self.blocks {
            return Err(EIO);
        }
        let per = self.block_size as u64 / 4;
        let span = per.pow(depth - 1);
        let mut buf = zeroed(self.block_size as usize)?;
        self.read_block(table, &mut buf, true)?;
        let spb = self.sectors_per_block();
        let mut changed = false;
        let mut empty = true;
        for i in 0..per {
            let e = le32(&buf, i as usize * 4);
            if e == 0 {
                continue;
            }
            let entry_start = i * span;
            if entry_start + span <= start {
                empty = false;
                continue;
            }
            let gone = if depth == 1 {
                true
            } else {
                self.free_tree(e, depth - 1, start.saturating_sub(entry_start), sectors)?
            };
            if gone {
                self.free_block(e)?;
                put32(&mut buf, i as usize * 4, 0);
                *sectors = sectors.saturating_sub(spb);
                changed = true;
            } else {
                empty = false;
            }
        }
        if changed {
            self.write_block(table, &buf)?;
        }
        Ok(empty)
    }

    fn free_from(&mut self, ino: &mut Inode, keep: u64) -> Result<(), i32> {
        let per = self.block_size as u64 / 4;
        let spb = self.sectors_per_block();
        for n in keep..N_DIRECT {
            let b = ino.block[n as usize];
            if b != 0 {
                self.free_block(b)?;
                ino.block[n as usize] = 0;
                ino.sectors = ino.sectors.saturating_sub(spb);
            }
        }
        let mut base = N_DIRECT;
        let mut span = per;
        for depth in 1..=3u32 {
            let slot = 11 + depth as usize;
            let t = ino.block[slot];
            let start = keep.saturating_sub(base);
            if t != 0 && start < span && self.free_tree(t, depth, start, &mut ino.sectors)? {
                self.free_block(t)?;
                ino.block[slot] = 0;
                ino.sectors = ino.sectors.saturating_sub(spb);
            }
            base += span;
            span *= per;
        }
        Ok(())
    }

    fn regular(&self, ino_n: u32) -> Result<Inode, i32> {
        if self.write.is_none() {
            return Err(EROFS);
        }
        let ino = self.inode(ino_n)?;
        if (ino.mode & S_IFMT) != S_IFREG {
            return Err(EINVAL);
        }
        Ok(ino)
    }

    fn write_data(&mut self, ino_n: u32, off: u64, data: &[u8], now: u32) -> Result<usize, i32> {
        let mut ino = self.regular(ino_n)?;
        let end = off.checked_add(data.len() as u64).ok_or(EFBIG)?;
        if end > self.write_limit() {
            return Err(EFBIG);
        }
        let goal = ((ino_n - 1) / self.inodes_per_group) as usize;
        let bs = self.block_size as u64;
        let mut tmp = zeroed(bs as usize)?;
        let mut done = 0usize;
        let mut err = 0;
        while done < data.len() {
            let pos = off + done as u64;
            let within = (pos % bs) as usize;
            let chunk = core::cmp::min(bs as usize - within, data.len() - done);
            let (b, fresh) = match self.bmap_alloc(&mut ino, pos / bs, goal) {
                Ok(x) => x,
                Err(e) => {
                    err = e;
                    break;
                }
            };
            let src = &data[done..done + chunk];
            let r = if within == 0 && chunk == bs as usize {
                self.write_block(b, src)
            } else {
                let loaded = if fresh {
                    tmp.fill(0);
                    Ok(())
                } else {
                    self.read_block(b, &mut tmp, false)
                };
                loaded.and_then(|_| {
                    tmp[within..within + chunk].copy_from_slice(src);
                    self.write_block(b, &tmp)
                })
            };
            if let Err(e) = r {
                err = e;
                break;
            }
            done += chunk;
        }
        if off + done as u64 > ino.size {
            ino.size = off + done as u64;
        }
        if done > 0 {
            ino.mtime = now;
            ino.ctime = now;
        }
        self.write_inode(ino_n, &ino)?;
        if done == 0 && err != 0 {
            return Err(err);
        }
        Ok(done)
    }

    fn truncate(&mut self, ino_n: u32, size: u64, now: u32) -> Result<(), i32> {
        let mut ino = self.regular(ino_n)?;
        if size > self.write_limit() {
            return Err(EFBIG);
        }
        let bs = self.block_size as u64;
        if size < ino.size {
            self.free_from(&mut ino, size.div_ceil(bs))?;
            let within = (size % bs) as usize;
            if within != 0 {
                let b = self.map(&ino, size / bs)?;
                if b != 0 {
                    let mut tmp = zeroed(bs as usize)?;
                    self.read_block(b, &mut tmp, false)?;
                    tmp[within..].fill(0);
                    self.write_block(b, &tmp)?;
                }
            }
        }
        ino.size = size;
        ino.mtime = now;
        ino.ctime = now;
        self.write_inode(ino_n, &ino)
    }

    fn sync(&mut self, now: u32, clean: bool) -> Result<(), i32> {
        if self.write.is_none() || (!self.dirty && !clean) {
            return Ok(());
        }
        let mut sb = [0u8; 1024];
        self.read_at(1024, &mut sb, true)?;
        put32(&mut sb, 12, self.free_blocks);
        put32(&mut sb, 16, self.free_inodes);
        put32(&mut sb, 48, now);
        let state = le16(&sb, 58);
        put16(&mut sb, 58, if clean { state | STATE_VALID } else { state & !STATE_VALID });
        self.write_at(1024, &sb)?;
        if self.dirty {
            let mut gdt = zeroed(self.groups.len() * 32)?;
            self.read_at(self.gdt_off, &mut gdt, true)?;
            for (i, g) in self.groups.iter().enumerate() {
                put16(&mut gdt, i * 32 + 12, g.free_blocks as u16);
                put16(&mut gdt, i * 32 + 14, g.free_inodes as u16);
            }
            self.write_at(self.gdt_off, &gdt)?;
            self.dirty = false;
        }
        Ok(())
    }
}

fn mount(ctx: *mut c_void, read: ReadFn, write: Option<WriteFn>, info: &mut Ext2Info) -> Result<Box<Fs>, i32> {
    let mut sb = [0u8; 1024];
    let r = unsafe { read(ctx, 1024, sb.as_mut_ptr(), 1024, true) };
    if r < 0 {
        return Err(-r);
    }
    if le16(&sb, 56) != EXT2_MAGIC {
        return Err(EINVAL);
    }
    let log = le32(&sb, 24);
    if log > 2 {
        return Err(EINVAL);
    }
    let block_size = 1024u32 << log;
    let rev = le32(&sb, 76);
    let (inode_size, incompat, ro_compat) = if rev >= 1 {
        (le16(&sb, 88) as u32, le32(&sb, 96), le32(&sb, 100))
    } else {
        (128, 0, 0)
    };
    if incompat & INCOMPAT_RECOVER != 0 || incompat & !(INCOMPAT_SUPPORTED | INCOMPAT_RECOVER) != 0 {
        return Err(EINVAL);
    }
    let blocks = le32(&sb, 4);
    let inodes = le32(&sb, 0);
    let first_data_block = le32(&sb, 20);
    let blocks_per_group = le32(&sb, 32);
    let inodes_per_group = le32(&sb, 40);
    if inode_size < 128
        || inode_size > block_size
        || !inode_size.is_power_of_two()
        || blocks_per_group < MIN_BLOCKS_PER_GROUP
        || blocks_per_group > 8 * block_size
        || inodes_per_group == 0
        || inodes_per_group > 8 * block_size
        || blocks <= first_data_block
        || inodes == 0
    {
        return Err(EINVAL);
    }
    let groups_n = (blocks - first_data_block).div_ceil(blocks_per_group);
    if groups_n == 0
        || groups_n > MAX_GROUPS
        || groups_n as u64 * inodes_per_group as u64 > inodes as u64 + inodes_per_group as u64
    {
        return Err(EINVAL);
    }
    let mut last = [0u8; 1];
    let r = unsafe { read(ctx, blocks as u64 * block_size as u64 - 1, last.as_mut_ptr(), 1, true) };
    if r < 0 {
        return Err(EINVAL);
    }
    let gdt_bytes = groups_n as usize * 32;
    let mut gdt = zeroed(gdt_bytes)?;
    let gdt_off = (first_data_block as u64 + 1) * block_size as u64;
    let r = unsafe { read(ctx, gdt_off, gdt.as_mut_ptr(), gdt_bytes as u32, true) };
    if r < 0 {
        return Err(-r);
    }
    let mut groups = Vec::new();
    if groups.try_reserve_exact(groups_n as usize).is_err() {
        return Err(ENOMEM);
    }
    let mut sane = true;
    for g in 0..groups_n as usize {
        let table = le32(&gdt, g * 32 + 8);
        if table == 0 || table >= blocks {
            return Err(EINVAL);
        }
        let group = Group {
            block_bitmap: le32(&gdt, g * 32),
            inode_bitmap: le32(&gdt, g * 32 + 4),
            inode_table: table,
            free_blocks: le16(&gdt, g * 32 + 12) as u32,
            free_inodes: le16(&gdt, g * 32 + 14) as u32,
        };
        if group.block_bitmap == 0
            || group.block_bitmap >= blocks
            || group.inode_bitmap == 0
            || group.inode_bitmap >= blocks
            || group.free_blocks > blocks_per_group
            || group.free_inodes > inodes_per_group
        {
            sane = false;
        }
        groups.push(group);
    }
    let state = le16(&sb, 58);
    let writable = write.is_some()
        && sane
        && ro_compat & !RO_COMPAT_WRITABLE == 0
        && state & STATE_ERRORS == 0;
    let mut fs = Fs {
        ctx,
        read,
        write: if writable { write } else { None },
        block_size,
        blocks,
        inodes,
        first_data_block,
        blocks_per_group,
        inodes_per_group,
        inode_size,
        gdt_off,
        gdt_blocks: (gdt_bytes as u64).div_ceil(block_size as u64) as u32,
        free_blocks: le32(&sb, 12),
        free_inodes: le32(&sb, 16),
        filetype: incompat & INCOMPAT_FILETYPE != 0,
        large_file: ro_compat & RO_COMPAT_LARGE_FILE != 0,
        dirty: false,
        groups,
    };
    let root = fs.inode(ROOT_INO)?;
    if (root.mode & S_IFMT) != S_IFDIR {
        return Err(EINVAL);
    }
    if writable {
        put16(&mut sb, 58, state & !STATE_VALID);
        if fs.write_at(1024, &sb).is_err() {
            fs.write = None;
        }
    }
    info.block_size = block_size;
    info.blocks = blocks;
    info.free_blocks = fs.free_blocks;
    info.inodes = inodes;
    info.free_inodes = fs.free_inodes;
    info.groups = groups_n;
    info.rev = rev;
    info.inode_size = inode_size;
    info.writable = fs.write.is_some() as u32;
    info.label = [0; 17];
    if rev >= 1 {
        info.label[..16].copy_from_slice(&sb[120..136]);
    }
    let layout = core::alloc::Layout::new::<Fs>();
    let raw = unsafe { alloc::alloc::alloc(layout) } as *mut Fs;
    if raw.is_null() {
        return Err(ENOMEM);
    }
    unsafe {
        raw.write(fs);
        Ok(Box::from_raw(raw))
    }
}

fn errno(e: i32) -> i32 {
    -e
}

#[no_mangle]
pub unsafe extern "C" fn ext2_mount(
    ctx: *mut c_void,
    read: ReadFn,
    write: Option<WriteFn>,
    out: *mut *mut Fs,
    info: *mut Ext2Info,
) -> i32 {
    if out.is_null() || info.is_null() {
        return errno(EINVAL);
    }
    match mount(ctx, read, write, &mut *info) {
        Ok(fs) => {
            *out = Box::into_raw(fs);
            0
        }
        Err(e) => errno(e),
    }
}

#[no_mangle]
pub unsafe extern "C" fn ext2_umount(fs: *mut Fs) {
    if !fs.is_null() {
        drop(Box::from_raw(fs));
    }
}

#[no_mangle]
pub extern "C" fn ext2_root_ino() -> u32 {
    ROOT_INO
}

#[no_mangle]
pub unsafe extern "C" fn ext2_stat(fs: *const Fs, ino: u32, out: *mut Ext2Stat) -> i32 {
    let fs = &*fs;
    match fs.inode(ino) {
        Ok(i) => {
            *out = Ext2Stat {
                mode: i.mode,
                uid: i.uid,
                gid: i.gid,
                nlink: i.nlink,
                size: i.size,
                atime: i.atime,
                mtime: i.mtime,
                ctime: i.ctime,
                blocks: i.sectors,
            };
            0
        }
        Err(e) => errno(e),
    }
}

#[no_mangle]
pub unsafe extern "C" fn ext2_lookup(fs: *const Fs, dir: u32, name: *const u8, len: u32, out: *mut u32) -> i32 {
    if len == 0 || len > 255 {
        return errno(if len == 0 { ENOENT } else { ENAMETOOLONG });
    }
    let fs = &*fs;
    let name = core::slice::from_raw_parts(name, len as usize);
    match fs.lookup(dir, name) {
        Ok(ino) => {
            *out = ino;
            0
        }
        Err(e) => errno(e),
    }
}

#[no_mangle]
pub unsafe extern "C" fn ext2_readdir(fs: *const Fs, dir: u32, pos: *mut u64, cb: DirFn, ctx: *mut c_void) -> i32 {
    let fs = &*fs;
    let ino = match fs.inode(dir) {
        Ok(i) => i,
        Err(e) => return errno(e),
    };
    let start = *pos;
    let mut cur = start;
    let r = fs.dir_walk(&ino, start, |name, i, dtype, next| {
        if name == b"." || name == b".." {
            cur = next;
            return true;
        }
        if !cb(ctx, name.as_ptr(), name.len() as u32, i, dtype, next) {
            return false;
        }
        cur = next;
        true
    });
    *pos = cur;
    match r {
        Ok(()) => 0,
        Err(e) => errno(e),
    }
}

#[no_mangle]
pub unsafe extern "C" fn ext2_read(fs: *const Fs, ino: u32, off: u64, buf: *mut u8, len: u64) -> i64 {
    let fs = &*fs;
    let inode = match fs.inode(ino) {
        Ok(i) => i,
        Err(e) => return errno(e) as i64,
    };
    if (inode.mode & S_IFMT) == S_IFDIR {
        return errno(EINVAL) as i64;
    }
    let out = core::slice::from_raw_parts_mut(buf, len as usize);
    match fs.read_data(&inode, off, out, false) {
        Ok(n) => n as i64,
        Err(e) => errno(e) as i64,
    }
}

#[no_mangle]
pub unsafe extern "C" fn ext2_readlink(fs: *const Fs, ino: u32, buf: *mut u8, size: u64) -> i64 {
    let fs = &*fs;
    let inode = match fs.inode(ino) {
        Ok(i) => i,
        Err(e) => return errno(e) as i64,
    };
    if (inode.mode & S_IFMT) != S_IFLNK {
        return errno(EINVAL) as i64;
    }
    let out = core::slice::from_raw_parts_mut(buf, size as usize);
    let n = core::cmp::min(inode.size, size) as usize;
    let ea = if inode.file_acl != 0 { fs.block_size / 512 } else { 0 };
    if inode.size <= 60 && inode.sectors.saturating_sub(ea) == 0 {
        for (i, b) in out[..n].iter_mut().enumerate() {
            *b = (inode.block[i / 4] >> ((i % 4) * 8)) as u8;
        }
        return n as i64;
    }
    match fs.read_data(&inode, 0, &mut out[..n], true) {
        Ok(n) => n as i64,
        Err(e) => errno(e) as i64,
    }
}

#[no_mangle]
pub unsafe extern "C" fn ext2_write(fs: *mut Fs, ino: u32, off: u64, buf: *const u8, len: u64, now: u32) -> i64 {
    let fs = &mut *fs;
    if len == 0 {
        return 0;
    }
    let data = core::slice::from_raw_parts(buf, len as usize);
    match fs.write_data(ino, off, data, now) {
        Ok(n) => n as i64,
        Err(e) => errno(e) as i64,
    }
}

#[no_mangle]
pub unsafe extern "C" fn ext2_truncate(fs: *mut Fs, ino: u32, size: u64, now: u32) -> i32 {
    let fs = &mut *fs;
    match fs.truncate(ino, size, now) {
        Ok(()) => 0,
        Err(e) => errno(e),
    }
}

#[no_mangle]
pub unsafe extern "C" fn ext2_sync(fs: *mut Fs, now: u32, clean: bool) -> i32 {
    let fs = &mut *fs;
    match fs.sync(now, clean) {
        Ok(()) => 0,
        Err(e) => errno(e),
    }
}
