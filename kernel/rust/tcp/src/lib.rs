#![cfg_attr(not(test), no_std)]
#![allow(clippy::missing_safety_doc)]

extern crate alloc;

#[cfg(test)]
mod tests;

use alloc::collections::VecDeque;
use alloc::vec::Vec;

pub const HDR_LEN: usize = 20;
pub const FIN: u8 = 0x01;
pub const SYN: u8 = 0x02;
pub const RST: u8 = 0x04;
pub const PSH: u8 = 0x08;
pub const ACK: u8 = 0x10;
pub const PROTO_TCP: u8 = 6;

pub const RX_CAP: usize = 32768;
pub const TX_CAP: usize = 32768;
pub const MSS_OURS: u16 = 1460;
pub const MSS_DEFAULT: u16 = 536;
pub const PORT_FIRST: u16 = 49152;

pub const EV_READABLE: u32 = 1;
pub const EV_WRITABLE: u32 = 2;
pub const EV_CONNECTED: u32 = 4;
pub const EV_ERROR: u32 = 8;
pub const EV_CLOSED: u32 = 16;

const EINVAL: i32 = 22;
const EPIPE: i32 = 32;
const EBADMSG: i32 = 74;
const EADDRINUSE: i32 = 98;
const EADDRNOTAVAIL: i32 = 99;
const ENOBUFS: i32 = 105;
const ENOTCONN: i32 = 107;
const ECONNREFUSED: i32 = 111;
const ECONNRESET: i32 = 104;
const ENOENT: i32 = 2;

#[derive(Debug, PartialEq, Eq)]
pub struct Segment<'a> {
    pub sport: u16,
    pub dport: u16,
    pub seq: u32,
    pub ack: u32,
    pub flags: u8,
    pub window: u16,
    pub options: &'a [u8],
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

    pub fn mss(&self) -> Option<u16> {
        let o = self.options;
        let mut i = 0;
        while i < o.len() {
            match o[i] {
                0 => break,
                1 => i += 1,
                kind => {
                    let len = *o.get(i + 1)? as usize;
                    if len < 2 || i + len > o.len() {
                        return None;
                    }
                    if kind == 2 && len == 4 {
                        return Some(u16::from_be_bytes([o[i + 2], o[i + 3]]));
                    }
                    i += len;
                }
            }
        }
        None
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
        options: &seg[HDR_LEN..doff],
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

pub fn build_with(out: &mut [u8], src: u32, dst: u32, h: &Header, opts: &[u8], payload: &[u8]) -> Result<usize, i32> {
    if !opts.len().is_multiple_of(4) || opts.len() > 40 {
        return Err(EINVAL);
    }
    let hl = HDR_LEN + opts.len();
    let total = hl + payload.len();
    if out.len() < total || total > 0xFFFF {
        return Err(ENOBUFS);
    }
    let o = &mut out[..total];
    o[0..2].copy_from_slice(&h.sport.to_be_bytes());
    o[2..4].copy_from_slice(&h.dport.to_be_bytes());
    o[4..8].copy_from_slice(&h.seq.to_be_bytes());
    o[8..12].copy_from_slice(&h.ack.to_be_bytes());
    o[12] = ((hl / 4) as u8) << 4;
    o[13] = h.flags;
    o[14..16].copy_from_slice(&h.window.to_be_bytes());
    o[16..20].fill(0);
    o[HDR_LEN..hl].copy_from_slice(opts);
    o[hl..].copy_from_slice(payload);
    let c = checksum(src, dst, o);
    o[16..18].copy_from_slice(&c.to_be_bytes());
    Ok(total)
}

pub fn build(out: &mut [u8], src: u32, dst: u32, h: &Header, payload: &[u8]) -> Result<usize, i32> {
    build_with(out, src, dst, h, &[], payload)
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

fn lt(a: u32, b: u32) -> bool {
    (a.wrapping_sub(b) as i32) < 0
}

fn le(a: u32, b: u32) -> bool {
    a == b || lt(a, b)
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
                k.listener() && k.local_port == key.local_port && (k.local_ip == 0 || k.local_ip == key.local_ip)
            })
        })?;
        Some(&mut self.entries[i].1)
    }

    pub fn find<F: Fn(&T) -> bool>(&mut self, f: F) -> Option<(Key, &mut T)> {
        let i = self.entries.iter().position(|(_, v)| f(v))?;
        let e = &mut self.entries[i];
        Some((e.0, &mut e.1))
    }

    pub fn keys(&self) -> impl Iterator<Item = &Key> {
        self.entries.iter().map(|(k, _)| k)
    }

    pub fn retain<F: FnMut(&T) -> bool>(&mut self, mut f: F) {
        self.entries.retain(|(_, v)| f(v));
    }
}

#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum State {
    Closed = 0,
    SynSent = 2,
    Established = 4,
    FinWait1 = 5,
    FinWait2 = 6,
    CloseWait = 7,
    Closing = 8,
    LastAck = 9,
    TimeWait = 10,
}

pub struct Conn {
    pub id: u32,
    pub state: State,
    iss: u32,
    snd_una: u32,
    snd_nxt: u32,
    snd_wnd: u32,
    snd_wl1: u32,
    snd_wl2: u32,
    rcv_nxt: u32,
    mss: u16,
    rx: VecDeque<u8>,
    tx: VecDeque<u8>,
    fin_queued: bool,
    fin_sent: bool,
    fin_acked: bool,
    peer_fin: bool,
    ack_pending: bool,
    detached: bool,
    error: i32,
}

#[derive(Default)]
struct Reply {
    flags: u8,
    events: u32,
}

impl Conn {
    fn new(id: u32, iss: u32) -> Conn {
        Conn {
            id,
            state: State::SynSent,
            iss,
            snd_una: iss,
            snd_nxt: iss.wrapping_add(1),
            snd_wnd: 0,
            snd_wl1: 0,
            snd_wl2: 0,
            rcv_nxt: 0,
            mss: MSS_DEFAULT,
            rx: VecDeque::new(),
            tx: VecDeque::new(),
            fin_queued: false,
            fin_sent: false,
            fin_acked: false,
            peer_fin: false,
            ack_pending: false,
            detached: false,
            error: 0,
        }
    }

    fn rcv_wnd(&self) -> u32 {
        (RX_CAP - self.rx.len()) as u32
    }

    fn header(&self, key: &Key, seq: u32, flags: u8) -> Header {
        Header {
            sport: key.local_port,
            dport: key.remote_port,
            seq,
            ack: self.rcv_nxt,
            flags,
            window: self.rcv_wnd().min(0xFFFF) as u16,
        }
    }

    fn fail(&mut self, err: i32) -> u32 {
        self.state = State::Closed;
        self.error = err;
        self.rx.clear();
        self.tx.clear();
        EV_ERROR | EV_READABLE | EV_WRITABLE
    }

    fn synchronized(&self) -> bool {
        !matches!(self.state, State::Closed | State::SynSent)
    }

    fn acceptable(&self, s: &Segment) -> bool {
        let len = s.seq_len();
        let wnd = self.rcv_wnd();
        let in_wnd = |x: u32| le(self.rcv_nxt, x) && lt(x, self.rcv_nxt.wrapping_add(wnd));
        match (len, wnd) {
            (0, 0) => s.seq == self.rcv_nxt,
            (0, _) => in_wnd(s.seq),
            (_, 0) => false,
            _ => in_wnd(s.seq) || in_wnd(s.seq.wrapping_add(len - 1)),
        }
    }

    fn input_syn_sent(&mut self, s: &Segment) -> Reply {
        let mut r = Reply::default();
        let has_ack = s.flags & ACK != 0;
        if has_ack && (le(s.ack, self.iss) || lt(self.snd_nxt, s.ack)) {
            if s.flags & RST == 0 {
                r.flags = RST;
            }
            return r;
        }
        if s.flags & RST != 0 {
            if has_ack {
                r.events = self.fail(ECONNREFUSED);
            }
            return r;
        }
        if s.flags & SYN == 0 || !has_ack {
            return r;
        }
        self.rcv_nxt = s.seq.wrapping_add(1);
        self.snd_una = s.ack;
        self.snd_wnd = s.window as u32;
        self.snd_wl1 = s.seq;
        self.snd_wl2 = s.ack;
        self.mss = s.mss().unwrap_or(MSS_DEFAULT).clamp(64, MSS_OURS);
        self.state = State::Established;
        r.flags = ACK;
        r.events = EV_CONNECTED | EV_WRITABLE;
        r
    }

    fn input(&mut self, s: &Segment) -> Reply {
        if self.state == State::SynSent {
            return self.input_syn_sent(s);
        }
        let mut r = Reply::default();
        if !self.acceptable(s) {
            if s.flags & RST == 0 {
                r.flags = ACK;
            }
            return r;
        }
        if s.flags & RST != 0 {
            r.events = self.fail(ECONNRESET);
            return r;
        }
        if s.flags & SYN != 0 {
            r.flags = ACK;
            return r;
        }
        if s.flags & ACK == 0 {
            return r;
        }
        if lt(self.snd_una, s.ack) && le(s.ack, self.snd_nxt) {
            let mut acked = s.ack.wrapping_sub(self.snd_una) as usize;
            if self.fin_sent && s.ack == self.snd_nxt {
                self.fin_acked = true;
                acked -= 1;
            }
            let acked = acked.min(self.tx.len());
            self.tx.drain(..acked);
            self.snd_una = s.ack;
            r.events |= EV_WRITABLE;
        } else if lt(self.snd_nxt, s.ack) {
            r.flags = ACK;
            return r;
        }
        if le(s.ack, self.snd_nxt)
            && (lt(self.snd_wl1, s.seq) || (self.snd_wl1 == s.seq && le(self.snd_wl2, s.ack)))
        {
            self.snd_wnd = s.window as u32;
            self.snd_wl1 = s.seq;
            self.snd_wl2 = s.ack;
            r.events |= EV_WRITABLE;
        }
        if self.fin_acked {
            match self.state {
                State::FinWait1 => self.state = State::FinWait2,
                State::Closing => self.state = State::TimeWait,
                State::LastAck => {
                    self.state = State::Closed;
                    r.events |= EV_CLOSED;
                }
                _ => {}
            }
        }
        let mut data = s.payload;
        let mut seq = s.seq;
        if lt(seq, self.rcv_nxt) {
            let skip = (self.rcv_nxt.wrapping_sub(seq) as usize).min(data.len());
            data = &data[skip..];
            seq = seq.wrapping_add(skip as u32);
        }
        let receiving = matches!(self.state, State::Established | State::FinWait1 | State::FinWait2);
        let mut all_in = data.is_empty() && seq == self.rcv_nxt;
        if !data.is_empty() {
            r.flags = ACK;
            if receiving && seq == self.rcv_nxt {
                let take = data.len().min(RX_CAP - self.rx.len());
                if !self.detached {
                    self.rx.extend(&data[..take]);
                }
                self.rcv_nxt = self.rcv_nxt.wrapping_add(take as u32);
                all_in = take == data.len();
                if take > 0 {
                    r.events |= EV_READABLE;
                }
            }
        }
        if s.flags & FIN != 0 && all_in && !self.peer_fin {
            self.rcv_nxt = self.rcv_nxt.wrapping_add(1);
            self.peer_fin = true;
            r.flags = ACK;
            r.events |= EV_READABLE;
            self.state = match self.state {
                State::Established => State::CloseWait,
                State::FinWait1 if self.fin_acked => State::TimeWait,
                State::FinWait1 => State::Closing,
                State::FinWait2 => State::TimeWait,
                other => other,
            };
        } else if s.flags & FIN != 0 && self.peer_fin {
            r.flags = ACK;
        }
        if self.detached && self.peer_fin && self.state == State::FinWait2 {
            self.state = State::TimeWait;
        }
        r
    }

    fn data_in_flight(&self) -> usize {
        let fin = (self.fin_sent && !self.fin_acked) as usize;
        (self.snd_nxt.wrapping_sub(self.snd_una) as usize).saturating_sub(fin)
    }

    fn unsent(&self) -> usize {
        self.tx.len() - self.data_in_flight().min(self.tx.len())
    }

    fn output(&mut self, key: &Key, out: &mut [u8]) -> Result<usize, i32> {
        if !self.synchronized() {
            return Ok(0);
        }
        let in_flight = self.snd_nxt.wrapping_sub(self.snd_una);
        let sent = self.data_in_flight();
        let unsent = self.unsent();
        let room = self.snd_wnd.saturating_sub(in_flight) as usize;
        let n = unsent.min(room).min(self.mss as usize);
        let sending = matches!(self.state, State::Established | State::CloseWait);
        if n > 0 && sending && !self.fin_sent {
            let mut payload = [0u8; MSS_OURS as usize];
            for (i, b) in self.tx.range(sent..sent + n).enumerate() {
                payload[i] = *b;
            }
            let flags = if n == unsent { ACK | PSH } else { ACK };
            let h = self.header(key, self.snd_nxt, flags);
            let len = build(out, key.local_ip, key.remote_ip, &h, &payload[..n])?;
            self.snd_nxt = self.snd_nxt.wrapping_add(n as u32);
            self.ack_pending = false;
            return Ok(len);
        }
        if self.fin_queued && !self.fin_sent && unsent == 0 && sending {
            let h = self.header(key, self.snd_nxt, FIN | ACK);
            let len = build(out, key.local_ip, key.remote_ip, &h, &[])?;
            self.snd_nxt = self.snd_nxt.wrapping_add(1);
            self.fin_sent = true;
            self.ack_pending = false;
            self.state = if self.state == State::Established { State::FinWait1 } else { State::LastAck };
            return Ok(len);
        }
        if self.ack_pending {
            let h = self.header(key, self.snd_nxt, ACK);
            let len = build(out, key.local_ip, key.remote_ip, &h, &[])?;
            self.ack_pending = false;
            return Ok(len);
        }
        Ok(0)
    }

    fn finished(&self) -> bool {
        matches!(self.state, State::Closed | State::TimeWait)
    }
}

#[repr(C)]
#[derive(Default, Clone, Copy)]
pub struct TcpStats {
    pub rx_segments: u64,
    pub rx_bad: u64,
    pub rx_dropped: u64,
    pub tx_resets: u64,
    pub connections: u64,
}

#[repr(C)]
#[derive(Default, Clone, Copy, Debug, PartialEq, Eq)]
pub struct TcpEvent {
    pub conn: u32,
    pub events: u32,
}

#[repr(C)]
#[derive(Default, Clone, Copy, Debug)]
pub struct TcpInfo {
    pub state: u32,
    pub error: i32,
    pub readable: u32,
    pub writable: u32,
    pub unacked: u32,
    pub eof: u32,
    pub local_ip: u32,
    pub remote_ip: u32,
    pub local_port: u16,
    pub remote_port: u16,
}

pub struct Stack {
    table: Table<Conn>,
    stats: TcpStats,
    next_id: u32,
    next_port: u16,
}

impl Default for Stack {
    fn default() -> Self {
        Stack { table: Table::default(), stats: TcpStats::default(), next_id: 1, next_port: PORT_FIRST }
    }
}

impl Stack {
    pub fn input(&mut self, src: u32, dst: u32, seg: &[u8], out: &mut [u8], ev: &mut TcpEvent) -> Result<usize, i32> {
        *ev = TcpEvent::default();
        let s = match parse(src, dst, seg) {
            Ok(s) => s,
            Err(e) => {
                self.stats.rx_bad += 1;
                return Err(e);
            }
        };
        self.stats.rx_segments += 1;
        let key = Key { local_ip: dst, local_port: s.dport, remote_ip: src, remote_port: s.sport };
        if let Some(c) = self.table.lookup(&key).filter(|c| c.state != State::Closed) {
            let r = c.input(&s);
            ev.conn = c.id;
            ev.events = r.events;
            let n = if r.flags & RST != 0 {
                let h = Header { sport: key.local_port, dport: key.remote_port, seq: s.ack, ack: 0, flags: RST, window: 0 };
                self.stats.tx_resets += 1;
                build(out, dst, src, &h, &[])?
            } else if r.flags & ACK != 0 {
                let h = c.header(&key, c.snd_nxt, ACK);
                c.ack_pending = false;
                build(out, dst, src, &h, &[])?
            } else {
                0
            };
            self.reap();
            return Ok(n);
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

    fn reap(&mut self) {
        self.table.retain(|c| !(c.detached && c.finished()));
    }

    fn free_port(&mut self) -> Option<u16> {
        for _ in 0..(u16::MAX - PORT_FIRST) {
            let p = self.next_port;
            self.next_port = if p == u16::MAX { PORT_FIRST } else { p + 1 };
            if !self.table.keys().any(|k| k.local_port == p) {
                return Some(p);
            }
        }
        None
    }

    pub fn connect(&mut self, local_ip: u32, remote_ip: u32, remote_port: u16, iss: u32, out: &mut [u8]) -> Result<(u32, usize), i32> {
        if remote_ip == 0 || remote_port == 0 || local_ip == 0 {
            return Err(EINVAL);
        }
        let port = self.free_port().ok_or(EADDRNOTAVAIL)?;
        let key = Key { local_ip, local_port: port, remote_ip, remote_port };
        let id = self.next_id;
        self.next_id = self.next_id.wrapping_add(1).max(1);
        let c = Conn::new(id, iss);
        let h = Header { sport: port, dport: remote_port, seq: iss, ack: 0, flags: SYN, window: c.rcv_wnd() as u16 };
        let mss = MSS_OURS.to_be_bytes();
        let n = build_with(out, local_ip, remote_ip, &h, &[2, 4, mss[0], mss[1]], &[])?;
        self.table.insert(key, c)?;
        Ok((id, n))
    }

    fn conn(&mut self, id: u32) -> Result<(Key, &mut Conn), i32> {
        self.table.find(|c| c.id == id).ok_or(ENOENT)
    }

    pub fn write(&mut self, id: u32, data: &[u8]) -> Result<usize, i32> {
        let (_, c) = self.conn(id)?;
        if c.error != 0 {
            return Err(c.error);
        }
        match c.state {
            State::Established | State::CloseWait if !c.fin_queued => {}
            State::SynSent => return Err(ENOTCONN),
            _ => return Err(EPIPE),
        }
        let take = data.len().min(TX_CAP - c.tx.len());
        c.tx.extend(&data[..take]);
        Ok(take)
    }

    pub fn output(&mut self, id: u32, out: &mut [u8]) -> Result<(usize, u32), i32> {
        let (key, c) = self.conn(id)?;
        let n = c.output(&key, out)?;
        Ok((n, key.remote_ip))
    }

    pub fn read(&mut self, id: u32, buf: &mut [u8]) -> Result<usize, i32> {
        let (_, c) = self.conn(id)?;
        let before = c.rcv_wnd();
        let n = buf.len().min(c.rx.len());
        for (dst, src) in buf.iter_mut().zip(c.rx.drain(..n)) {
            *dst = src;
        }
        if n > 0 && before < c.mss as u32 && c.rcv_wnd() >= c.mss as u32 && c.synchronized() {
            c.ack_pending = true;
        }
        if n == 0 && c.error != 0 {
            return Err(c.error);
        }
        Ok(n)
    }

    pub fn close(&mut self, id: u32) -> Result<(), i32> {
        let (_, c) = self.conn(id)?;
        match c.state {
            State::SynSent => c.state = State::Closed,
            State::Established | State::CloseWait => c.fin_queued = true,
            _ => {}
        }
        Ok(())
    }

    pub fn detach(&mut self, id: u32) {
        if let Ok((_, c)) = self.conn(id) {
            c.detached = true;
            c.rx.clear();
        }
        self.reap();
    }

    pub fn info(&mut self, id: u32) -> Result<TcpInfo, i32> {
        let (key, c) = self.conn(id)?;
        Ok(TcpInfo {
            state: c.state as u32,
            error: c.error,
            readable: c.rx.len() as u32,
            writable: (TX_CAP - c.tx.len()) as u32,
            unacked: c.snd_nxt.wrapping_sub(c.snd_una),
            eof: (c.peer_fin && c.rx.is_empty()) as u32,
            local_ip: key.local_ip,
            remote_ip: key.remote_ip,
            local_port: key.local_port,
            remote_port: key.remote_port,
        })
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

fn ret(r: Result<usize, i32>) -> i64 {
    match r {
        Ok(n) => n as i64,
        Err(e) => -(e as i64),
    }
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
    ev: *mut TcpEvent,
) -> i64 {
    if st.is_null() || seg.is_null() || out.is_null() || ev.is_null() {
        return -(EINVAL as i64);
    }
    let seg = core::slice::from_raw_parts(seg, len);
    let out = core::slice::from_raw_parts_mut(out, cap);
    ret((*st).input(src, dst, seg, out, &mut *ev))
}

#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn tcp_connect(
    st: *mut Stack,
    local_ip: u32,
    remote_ip: u32,
    remote_port: u16,
    iss: u32,
    out: *mut u8,
    cap: usize,
    id: *mut u32,
) -> i64 {
    if st.is_null() || out.is_null() || id.is_null() {
        return -(EINVAL as i64);
    }
    let out = core::slice::from_raw_parts_mut(out, cap);
    match (*st).connect(local_ip, remote_ip, remote_port, iss, out) {
        Ok((c, n)) => {
            *id = c;
            n as i64
        }
        Err(e) => -(e as i64),
    }
}

#[no_mangle]
pub unsafe extern "C" fn tcp_write(st: *mut Stack, id: u32, data: *const u8, len: usize) -> i64 {
    if st.is_null() || (data.is_null() && len > 0) {
        return -(EINVAL as i64);
    }
    let data = if len == 0 { &[][..] } else { core::slice::from_raw_parts(data, len) };
    ret((*st).write(id, data))
}

#[no_mangle]
pub unsafe extern "C" fn tcp_output(st: *mut Stack, id: u32, out: *mut u8, cap: usize, dst: *mut u32) -> i64 {
    if st.is_null() || out.is_null() || dst.is_null() {
        return -(EINVAL as i64);
    }
    let out = core::slice::from_raw_parts_mut(out, cap);
    match (*st).output(id, out) {
        Ok((n, ip)) => {
            *dst = ip;
            n as i64
        }
        Err(e) => -(e as i64),
    }
}

#[no_mangle]
pub unsafe extern "C" fn tcp_read(st: *mut Stack, id: u32, buf: *mut u8, len: usize) -> i64 {
    if st.is_null() || (buf.is_null() && len > 0) {
        return -(EINVAL as i64);
    }
    let buf = if len == 0 { &mut [][..] } else { core::slice::from_raw_parts_mut(buf, len) };
    ret((*st).read(id, buf))
}

#[no_mangle]
pub unsafe extern "C" fn tcp_close(st: *mut Stack, id: u32) -> i32 {
    if st.is_null() {
        return -EINVAL;
    }
    match (*st).close(id) {
        Ok(()) => 0,
        Err(e) => -e,
    }
}

#[no_mangle]
pub unsafe extern "C" fn tcp_detach(st: *mut Stack, id: u32) {
    if !st.is_null() {
        (*st).detach(id);
    }
}

#[no_mangle]
pub unsafe extern "C" fn tcp_info(st: *mut Stack, id: u32, out: *mut TcpInfo) -> i32 {
    if st.is_null() || out.is_null() {
        return -EINVAL;
    }
    match (*st).info(id) {
        Ok(i) => {
            *out = i;
            0
        }
        Err(e) => -e,
    }
}

#[no_mangle]
pub unsafe extern "C" fn tcp_stats(st: *const Stack, out: *mut TcpStats) {
    if st.is_null() || out.is_null() {
        return;
    }
    *out = (*st).stats();
}
