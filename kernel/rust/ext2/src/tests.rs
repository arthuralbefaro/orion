use super::*;
use std::cell::{Cell, RefCell};
use std::path::PathBuf;
use std::process::Command;
use std::time::{SystemTime, UNIX_EPOCH};
use std::vec::Vec as StdVec;

const READ_BUDGET: u64 = 400_000;

struct Disk {
    data: RefCell<StdVec<u8>>,
    reads: Cell<u64>,
}

unsafe extern "C" fn disk_read(ctx: *mut c_void, off: u64, buf: *mut u8, len: u32, _meta: bool) -> i32 {
    let disk = &*(ctx as *const Disk);
    let n = disk.reads.get() + 1;
    disk.reads.set(n);
    assert!(n < READ_BUDGET, "leituras demais numa imagem so: provavel laco");
    let end = match off.checked_add(len as u64) {
        Some(e) => e,
        None => return -EIO,
    };
    let data = disk.data.borrow();
    if end > data.len() as u64 {
        return -EIO;
    }
    core::ptr::copy_nonoverlapping(data.as_ptr().add(off as usize), buf, len as usize);
    0
}

unsafe extern "C" fn disk_write(ctx: *mut c_void, off: u64, buf: *const u8, len: u32) -> i32 {
    let disk = &*(ctx as *const Disk);
    let mut data = disk.data.borrow_mut();
    let end = match off.checked_add(len as u64) {
        Some(e) => e,
        None => return -EIO,
    };
    if end > data.len() as u64 {
        return -EIO;
    }
    core::ptr::copy_nonoverlapping(buf, data.as_mut_ptr().add(off as usize), len as usize);
    0
}

struct Rng(u64);

impl Rng {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }

    fn below(&mut self, n: u64) -> u64 {
        if n == 0 {
            0
        } else {
            self.next() % n
        }
    }
}

struct Batch {
    left: u32,
}

unsafe extern "C" fn batch_cb(ctx: *mut c_void, _name: *const u8, _len: u32, _ino: u32, _t: u8, _next: u64) -> bool {
    let b = &mut *(ctx as *mut Batch);
    if b.left == 0 {
        return false;
    }
    b.left -= 1;
    true
}

struct Walk<'a> {
    fs: &'a Fs,
    rng: &'a mut Rng,
    ops: u32,
}

impl Walk<'_> {
    fn readdir_batches(&mut self, ino: u32) {
        let mut pos = 0u64;
        for _ in 0..64 {
            let before = pos;
            let mut b = Batch { left: 1 + self.rng.below(7) as u32 };
            let r = unsafe { ext2_readdir(self.fs, ino, &mut pos, batch_cb, &mut b as *mut Batch as *mut c_void) };
            if r < 0 || pos == before {
                break;
            }
        }
        for cookie in [1u64, 7, 13, u32::MAX as u64, u64::MAX, self.rng.next()] {
            let mut pos = cookie;
            let mut b = Batch { left: 3 };
            unsafe {
                ext2_readdir(self.fs, ino, &mut pos, batch_cb, &mut b as *mut Batch as *mut c_void);
            }
        }
    }

    fn visit(&mut self, ino: u32, depth: u32) {
        if depth > 8 || self.ops > 4000 {
            return;
        }
        self.ops += 1;
        let mut st = Ext2Stat { mode: 0, uid: 0, gid: 0, nlink: 0, size: 0, atime: 0, mtime: 0, ctime: 0, blocks: 0 };
        if unsafe { ext2_stat(self.fs, ino, &mut st) } < 0 {
            return;
        }
        match st.mode & S_IFMT {
            S_IFDIR => {
                self.readdir_batches(ino);
                let mut up = 0u32;
                unsafe {
                    ext2_lookup(self.fs, ino, b"..".as_ptr(), 2, &mut up);
                }
                let mut children = StdVec::new();
                if let Ok(inode) = self.fs.inode(ino) {
                    let _ = self.fs.dir_walk(&inode, 0, |name, i, _, _| {
                        if name != b"." && name != b".." && children.len() < 300 {
                            children.push((name.to_vec(), i));
                        }
                        true
                    });
                }
                for (name, i) in children {
                    let mut found = 0u32;
                    unsafe {
                        ext2_lookup(self.fs, ino, name.as_ptr(), name.len() as u32, &mut found);
                    }
                    self.visit(i, depth + 1);
                }
            }
            S_IFLNK => {
                let mut buf = [0u8; 300];
                for size in [1u64, 59, 60, 61, 300] {
                    unsafe {
                        ext2_readlink(self.fs, ino, buf.as_mut_ptr(), size);
                    }
                }
            }
            _ => {
                let mut buf = vec![0u8; 8192];
                let offsets = [0, st.size / 2, st.size.saturating_sub(1), st.size, st.size + 1,
                               self.rng.below(st.size + 1), u64::MAX - 10];
                for off in offsets {
                    let len = 1 + self.rng.below(8192);
                    unsafe {
                        ext2_read(self.fs, ino, off, buf.as_mut_ptr(), len);
                    }
                }
            }
        }
    }
}

fn image(name: &str, args: &[&str]) -> StdVec<u8> {
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../..");
    let out = std::env::temp_dir().join(format!("orion-ext2-{}.img", name));
    let python = std::env::var("PYTHON").unwrap_or_else(|_| "python".into());
    let status = Command::new(python)
        .arg(root.join("tools/mkext2.py"))
        .arg(&out)
        .arg("--test")
        .args(args)
        .status()
        .expect("python");
    assert!(status.success());
    std::fs::read(&out).expect("imagem")
}

fn empty_info() -> Ext2Info {
    Ext2Info {
        block_size: 0,
        blocks: 0,
        free_blocks: 0,
        inodes: 0,
        free_inodes: 0,
        groups: 0,
        rev: 0,
        inode_size: 0,
        writable: 0,
        label: [0; 17],
    }
}

fn walk(data: StdVec<u8>, rng: &mut Rng) -> bool {
    let disk = Disk { data: RefCell::new(data), reads: Cell::new(0) };
    let mut info = empty_info();
    let fs = match mount(&disk as *const Disk as *mut c_void, disk_read, None, &mut info) {
        Ok(fs) => fs,
        Err(_) => return false,
    };
    let mut w = Walk { fs: &fs, rng, ops: 0 };
    w.visit(ROOT_INO, 0);
    true
}

fn seed() -> u64 {
    if let Ok(s) = std::env::var("ORION_FUZZ_SEED") {
        return s.parse().expect("ORION_FUZZ_SEED");
    }
    let t = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_nanos() as u64).unwrap_or(1);
    t | 1
}

fn mutate(base: &[u8], rng: &mut Rng, block: u64) -> StdVec<u8> {
    let mut data = base.to_vec();
    let flips = 1 + rng.below(24);
    for _ in 0..flips {
        let pos = if rng.below(2) == 0 {
            rng.below(core::cmp::min(base.len(), 600 * 1024) as u64)
        } else {
            rng.below(base.len() as u64)
        } as usize;
        match rng.below(4) {
            0 => data[pos] = rng.next() as u8,
            1 => {
                let at = pos & !3;
                if at + 4 <= data.len() {
                    let v: u32 = match rng.below(6) {
                        0 => 0,
                        1 => 4,
                        2 => block as u32,
                        3 => u32::MAX,
                        4 => (base.len() as u64 / block) as u32,
                        _ => rng.next() as u32,
                    };
                    data[at..at + 4].copy_from_slice(&v.to_le_bytes());
                }
            }
            2 => {
                let at = pos & !1;
                if at + 2 <= data.len() {
                    let v: u16 = match rng.below(4) {
                        0 => 0,
                        1 => 8,
                        2 => block as u16,
                        _ => 0xFFFF,
                    };
                    data[at..at + 2].copy_from_slice(&v.to_le_bytes());
                }
            }
            _ => data[pos] ^= 1 << rng.below(8),
        }
    }
    data
}

#[test]
fn clean_images_walk() {
    let mut rng = Rng(1);
    for (name, args) in [
        ("limpa-1k", &["--size", "8", "--block", "1024"][..]),
        ("limpa-4k", &["--size", "8", "--block", "4096"][..]),
        ("limpa-grupos", &["--size", "32", "--block", "1024", "--inodes-per-group", "128"][..]),
    ] {
        assert!(walk(image(name, args), &mut rng), "imagem {} nao montou", name);
    }
}

#[test]
fn mutated_images_never_panic() {
    let s = seed();
    println!("semente do fuzz: {} (ORION_FUZZ_SEED={} repete)", s, s);
    let mut rng = Rng(s);
    for (name, args, block, rounds) in [
        ("fuzz-1k", &["--size", "8", "--block", "1024"][..], 1024u64, 400),
        ("fuzz-4k", &["--size", "8", "--block", "4096"][..], 4096u64, 400),
        ("fuzz-grupos", &["--size", "32", "--block", "1024", "--inodes-per-group", "128"][..], 1024u64, 120),
    ] {
        let base = image(name, args);
        let mut mounted = 0;
        for _ in 0..rounds {
            let data = mutate(&base, &mut rng, block);
            if walk(data, &mut rng) {
                mounted += 1;
            }
        }
        assert!(mounted > 0, "{}: nenhuma imagem mutada montou (semente {})", name, s);
    }
}

fn pattern(n: usize, seed: u32) -> StdVec<u8> {
    let mut x = seed;
    (0..n)
        .map(|_| {
            x = x.wrapping_mul(1103515245).wrapping_add(12345);
            (x >> 16) as u8
        })
        .collect()
}

fn read_all(fs: &Fs, ino: u32, off: u64, len: usize) -> StdVec<u8> {
    let mut out = vec![0u8; len];
    let n = unsafe { ext2_read(fs, ino, off, out.as_mut_ptr(), len as u64) };
    assert_eq!(n, len as i64, "leitura curta em {}", off);
    out
}

fn write_ok(fs: &mut Fs, ino: u32, off: u64, data: &[u8]) {
    let n = unsafe { ext2_write(fs, ino, off, data.as_ptr(), data.len() as u64, 1_700_000_000) };
    assert_eq!(n, data.len() as i64, "escrita curta em {}", off);
}

fn size_of(fs: &Fs, ino: u32) -> u64 {
    fs.inode(ino).expect("inode").size
}

fn fsck(name: &str, data: &[u8]) {
    let path = std::env::temp_dir().join(format!("orion-ext2-{}-rw.img", name));
    std::fs::write(&path, data).expect("grava imagem");
    match Command::new("e2fsck").arg("-fn").arg(&path).output() {
        Ok(out) => assert!(
            out.status.success(),
            "e2fsck achou problemas em {}:\n{}{}",
            name,
            String::from_utf8_lossy(&out.stdout),
            String::from_utf8_lossy(&out.stderr)
        ),
        Err(_) => println!("e2fsck ausente: {} conferida so pelos testes internos", name),
    }
}

#[test]
fn write_truncate_keep_the_image_valid() {
    for (name, args, block) in [
        ("escrita-1k", &["--size", "32", "--block", "1024", "--inodes-per-group", "128"][..], 1024u64),
        ("escrita-4k", &["--size", "16", "--block", "4096"][..], 4096u64),
    ] {
        let disk = Disk { data: RefCell::new(image(name, args)), reads: Cell::new(0) };
        let mut info = empty_info();
        let ctx = &disk as *const Disk as *mut c_void;
        let mut fs = mount(ctx, disk_read, Some(disk_write), &mut info).expect("monta rw");
        assert_eq!(info.writable, 1, "{} montou so leitura", name);
        let free0 = fs.free_blocks;
        let ino = fs.lookup(ROOT_INO, b"empty").expect("empty");

        let a = pattern(300 * 1024 + 7, 1);
        write_ok(&mut fs, ino, 0, &a);
        assert_eq!(read_all(&fs, ino, 0, a.len()), a, "{}: dados do inicio", name);

        let far = 20 * 1024 * 1024u64;
        write_ok(&mut fs, ino, far, b"fim do arquivo");
        assert_eq!(size_of(&fs, ino), far + 14);
        assert!(read_all(&fs, ino, 5 * 1024 * 1024, 4096).iter().all(|&b| b == 0), "{}: buraco", name);
        assert_eq!(read_all(&fs, ino, far, 14), b"fim do arquivo");

        let mid = pattern(5000, 2);
        write_ok(&mut fs, ino, block * 3 + 100, &mid);
        assert_eq!(read_all(&fs, ino, block * 3 + 100, mid.len()), mid);
        assert_eq!(read_all(&fs, ino, 0, 100), a[..100].to_vec());

        assert_eq!(unsafe { ext2_truncate(&mut *fs, ino, 100_000, 1_700_000_001) }, 0);
        assert_eq!(size_of(&fs, ino), 100_000);
        assert_eq!(unsafe { ext2_truncate(&mut *fs, ino, 200_000, 1_700_000_002) }, 0);
        assert!(read_all(&fs, ino, 100_000, 100_000).iter().all(|&b| b == 0), "{}: cauda depois do truncate", name);
        assert_eq!(unsafe { ext2_truncate(&mut *fs, ino, 0, 1_700_000_003) }, 0);
        assert_eq!(fs.free_blocks, free0, "{}: truncate(0) nao devolveu todos os blocos", name);
        assert_eq!(fs.inode(ino).expect("inode").sectors, 0);

        let mut total = 0u64;
        let chunk = pattern(64 * 1024, 3);
        loop {
            let n = unsafe { ext2_write(&mut *fs, ino, total, chunk.as_ptr(), chunk.len() as u64, 1_700_000_004) };
            if n <= 0 {
                assert_eq!(n, -(ENOSPC as i64), "{}: erro ao encher o disco", name);
                break;
            }
            total += n as u64;
            if (n as usize) < chunk.len() {
                break;
            }
        }
        assert_eq!(fs.free_blocks, 0, "{}: disco cheio mas sobrou bloco livre", name);
        assert_eq!(unsafe { ext2_truncate(&mut *fs, ino, 0, 1_700_000_005) }, 0);
        assert_eq!(fs.free_blocks, free0, "{}: blocos perdidos depois de encher e esvaziar", name);

        let big = fs.lookup(ROOT_INO, b"big.bin").expect("big.bin");
        let before = read_all(&fs, big, 0, 8192);
        let patch = pattern(3000, 4);
        write_ok(&mut fs, big, 1_000_001, &patch);
        assert_eq!(read_all(&fs, big, 1_000_001, patch.len()), patch);
        assert_eq!(read_all(&fs, big, 0, 8192), before);

        write_ok(&mut fs, ino, 0, b"ola de novo\n");
        assert_eq!(unsafe { ext2_sync(&mut *fs, 1_700_000_006, true) }, 0);
        drop(fs);
        let data = disk.data.borrow();
        fsck(name, &data);
    }
}

#[test]
fn read_only_mount_refuses_writes() {
    let disk = Disk {
        data: RefCell::new(image("so-leitura", &["--size", "8", "--block", "1024"])),
        reads: Cell::new(0),
    };
    let mut info = empty_info();
    let ctx = &disk as *const Disk as *mut c_void;
    let mut fs = mount(ctx, disk_read, None, &mut info).expect("monta");
    assert_eq!(info.writable, 0);
    let ino = fs.lookup(ROOT_INO, b"empty").expect("empty");
    let n = unsafe { ext2_write(&mut *fs, ino, 0, b"x".as_ptr(), 1, 0) };
    assert_eq!(n, -(EROFS as i64));
    assert_eq!(unsafe { ext2_truncate(&mut *fs, ino, 10, 0) }, -EROFS);
}
