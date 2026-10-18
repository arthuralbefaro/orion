#![cfg_attr(not(test), no_std)]
#![allow(clippy::missing_safety_doc)]

extern crate alloc;

#[cfg(test)]
mod tests;

use alloc::vec::Vec;

pub const HDR_LEN: usize = 20;
pub const FIN: u8 = 0x01;
pub const SYN: u8 = 0x02;
pub const RST: u8 = 0x04;
pub const PSH: u8 = 0x08;
pub const ACK: u8 = 0x10;
pub const PROTO_TCP: u8 = 6;

const EINVAL: i32 = 22;
const EBADMSG: i32 = 74;
const EADDRINUSE: i32 = 98;
const ENOBUFS: i32 = 105;

#[derive(Debug, PartialEq, Eq)]
pub struct Segment<'a> {
    pub sport: u16,
    pub dport: u16,
    pub seq: u32,
    pub ack: u32,
    pub flags: u8,
    pub window: u16,
    pub payload: &'a [u8],
}

impl Segment<'_> {
    pub fn seq_len(&self) -> u32 {
        let mut n = self.payload.len() as u32;
        if self.flags & SYN != 0 {
            n = n.wrapping_add(1);
        }
        if self.flags & FIN != 0 {
            n = n.wrapping_add(1);
        }
        n
    }
}

fn be16(b: &[u8], off: usize) -> u16 {
    u16::from_be_bytes([b[off], b[off + 1]])
}

fn be32(b: &[u8], off: usize) -> u32 {
    u32::from_be_bytes([b[off], b[off + 1], b[off + 2], b[off + 3]])
}

fn sum_bytes(mut acc: u32, data: &[u8]) -> u32 {
    let (pairs, rest) = data.as_chunks::<2>();
    for c in pairs {
        acc += u16::from_be_bytes(*c) as u32;
    }
    if let [last] = rest {
        acc += (*last as u32) << 8;
    }
    acc
}

fn fold(mut acc: u32) -> u16 {
    while acc > 0xFFFF {
        acc = (acc & 0xFFFF) + (acc >> 16);
    }
    acc as u16
}

pub fn checksum(src: u32, dst: u32, seg: &[u8]) -> u16 {
    let mut acc = 0u32;
    acc = sum_bytes(acc, &src.to_be_bytes());
    acc = sum_bytes(acc, &dst.to_be_bytes());
    acc += PROTO_TCP as u32;
    acc += seg.len() as u32;
    acc = sum_bytes(acc, seg);
    !fold(acc)
}

pub fn parse(src: u32, dst: u32, seg: &[u8]) -> Result<Segment<'_>, i32> {
    if seg.len() < HDR_LEN || seg.len() > 0xFFFF {
        return Err(EBADMSG);
    }
    let doff = (seg[12] >> 4) as usize * 4;
    if doff < HDR_LEN || doff > seg.len() {
        return Err(EBADMSG);
    }
    if checksum(src, dst, seg) != 0 {
        return Err(EINVAL);
    }
    Ok(Segment {
        sport: be16(seg, 0),
        dport: be16(seg, 2),
        seq: be32(seg, 4),
        ack: be32(seg, 8),
        flags: seg[13],
        window: be16(seg, 14),
        payload: &seg[doff..],
    })
}

pub struct Header {
    pub sport: u16,
    pub dport: u16,
    pub seq: u32,
    pub ack: u32,
    pub flags: u8,
    pub window: u16,
}

pub fn build(out: &mut [u8], src: u32, dst: u32, h: &Header, payload: &[u8]) -> Result<usize, i32> {
    let total = HDR_LEN + payload.len();
    if out.len() < total || total > 0xFFFF {
        return Err(ENOBUFS);
    }
    let o = &mut out[..total];
    o[0..2].copy_from_slice(&h.sport.to_be_bytes());
    o[2..4].copy_from_slice(&h.dport.to_be_bytes());
    o[4..8].copy_from_slice(&h.seq.to_be_bytes());
    o[8..12].copy_from_slice(&h.ack.to_be_bytes());
    o[12] = (HDR_LEN as u8 / 4) << 4;
    o[13] = h.flags;
    o[14..16].copy_from_slice(&h.window.to_be_bytes());
    o[16..20].fill(0);
    o[HDR_LEN..].copy_from_slice(payload);
    let c = checksum(src, dst, o);
    o[16..18].copy_from_slice(&c.to_be_bytes());
    Ok(total)
}

pub fn reset_for(seg: &Segment) -> Option<Header> {
    if seg.flags & RST != 0 {
        return None;
    }
    let (seq, ack, flags) = if seg.flags & ACK != 0 {
        (seg.ack, 0, RST)
    } else {
        (0, seg.seq.wrapping_add(seg.seq_len()), RST | ACK)
    };
    Some(Header { sport: seg.dport, dport: seg.sport, seq, ack, flags, window: 0 })
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Key {
    pub local_ip: u32,
    pub local_port: u16,
    pub remote_ip: u32,
    pub remote_port: u16,
}

impl Key {
    fn listener(&self) -> bool {
        self.remote_ip == 0 && self.remote_port == 0
    }
}

pub struct Table<T> {
    entries: Vec<(Key, T)>,
}

impl<T> Default for Table<T> {
    fn default() -> Self {
        Table { entries: Vec::new() }
    }
}

impl<T> Table<T> {
    pub fn len(&self) -> usize {
        self.entries.len()
    }

    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }

    pub fn insert(&mut self, key: Key, value: T) -> Result<(), i32> {
        if self.entries.iter().any(|(k, _)| *k == key) {
            return Err(EADDRINUSE);
        }
        if self.entries.try_reserve(1).is_err() {
            return Err(ENOBUFS);
        }
        self.entries.push((key, value));
        Ok(())
    }

    pub fn remove(&mut self, key: &Key) -> Option<T> {
        let i = self.entries.iter().position(|(k, _)| k == key)?;
        Some(self.entries.swap_remove(i).1)
    }

    pub fn lookup(&mut self, key: &Key) -> Option<&mut T> {
        let exact = self.entries.iter().position(|(k, _)| k == key);
        let i = exact.or_else(|| {
            self.entries.iter().position(|(k, _)| {
                k.listener()
                    && k.local_port == key.local_port
                    && (k.local_ip == 0 || k.local_ip == key.local_ip)
            })
        })?;
        Some(&mut self.entries[i].1)
    }
}

pub struct Conn;

#[repr(C)]
#[derive(Default, Clone, Copy)]
pub struct TcpStats {
    pub rx_segments: u64,
    pub rx_bad: u64,
    pub rx_dropped: u64,
    pub tx_resets: u64,
    pub connections: u64,
}

#[derive(Default)]
pub struct Stack {
    table: Table<Conn>,
    stats: TcpStats,
}

impl Stack {
    pub fn input(&mut self, src: u32, dst: u32, seg: &[u8], out: &mut [u8]) -> Result<usize, i32> {
        let s = match parse(src, dst, seg) {
            Ok(s) => s,
            Err(e) => {
                self.stats.rx_bad += 1;
                return Err(e);
            }
        };
        self.stats.rx_segments += 1;
        let key = Key { local_ip: dst, local_port: s.dport, remote_ip: src, remote_port: s.sport };
        if self.table.lookup(&key).is_some() {
            self.stats.rx_dropped += 1;
            return Ok(0);
        }
        match reset_for(&s) {
            Some(h) => {
                let n = build(out, dst, src, &h, &[])?;
                self.stats.tx_resets += 1;
                Ok(n)
            }
            None => {
                self.stats.rx_dropped += 1;
                Ok(0)
            }
        }
    }

    pub fn stats(&self) -> TcpStats {
        let mut st = self.stats;
        st.connections = self.table.len() as u64;
        st
    }
}

#[no_mangle]
pub extern "C" fn tcp_stack_new() -> *mut Stack {
    let p = unsafe { alloc::alloc::alloc(core::alloc::Layout::new::<Stack>()) } as *mut Stack;
    if !p.is_null() {
        unsafe { p.write(Stack::default()) };
    }
    p
}

#[no_mangle]
pub unsafe extern "C" fn tcp_input(
    st: *mut Stack,
    src: u32,
    dst: u32,
    seg: *const u8,
    len: usize,
    out: *mut u8,
    cap: usize,
) -> i64 {
    if st.is_null() || seg.is_null() || out.is_null() {
        return -(EINVAL as i64);
    }
    let st = &mut *st;
    let seg = core::slice::from_raw_parts(seg, len);
    let out = core::slice::from_raw_parts_mut(out, cap);
    match st.input(src, dst, seg, out) {
        Ok(n) => n as i64,
        Err(e) => -(e as i64),
    }
}

#[no_mangle]
pub unsafe extern "C" fn tcp_stats(st: *const Stack, out: *mut TcpStats) {
    if st.is_null() || out.is_null() {
        return;
    }
    *out = (*st).stats();
}
