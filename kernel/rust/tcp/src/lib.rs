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
const EAGAIN: i32 = 11;

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

    pub fn iter(&self) -> impl Iterator<Item = (&Key, &T)> {
        self.entries.iter().map(|(k, v)| (k, v))
    }

    pub fn iter_mut(&mut self) -> impl Iterator<Item = (&Key, &mut T)> {
        self.entries.iter_mut().map(|(k, v)| (&*k, v))
    }
}

#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum State {
    Closed = 0,
    Listen = 1,
    SynSent = 2,
    SynReceived = 3,
    Established = 4,
    FinWait1 = 5,
    FinWait2 = 6,
    CloseWait = 7,
    Closing = 8,
    LastAck = 9,
    TimeWait = 10,
}

pub const RTO_INIT: u64 = 1000;
pub const RTO_MIN: u64 = 200;
pub const RTO_MAX: u64 = 60_000;
pub const SYN_RETRIES: u32 = 5;
pub const DATA_RETRIES: u32 = 8;
pub const MSL: u64 = 30_000;
const ETIMEDOUT: i32 = 110;

pub struct Conn {
    pub id: u32,
    pub state: State,
    iss: u32,
    snd_una: u32,
    snd_nxt: u32,
    snd_max: u32,
    snd_wnd: u32,
    snd_wl1: u32,
    snd_wl2: u32,
    rcv_nxt: u32,
    mss: u16,
    rx: VecDeque<u8>,
    tx: VecDeque<u8>,
    ooo: Vec<(u32, Vec<u8>)>,
    fin_queued: bool,
    fin_sent: bool,
    fin_acked: bool,
    peer_fin: bool,
    ack_pending: bool,
    detached: bool,
    error: i32,
    rto: u64,
    srtt: u64,
    rttvar: u64,
    rtt_probe: Option<(u32, u64)>,
    rexmit_at: u64,
    retries: u32,
    persist_at: u64,
    persist_wait: u64,
    timewait_at: u64,
    dupacks: u32,
    fast_rexmit: bool,
    in_recovery: bool,
    cwnd: u32,
    ssthresh: u32,
    retransmits: u64,
    parent: u32,
    backlog: u32,
    accept_q: VecDeque<u32>,
}

#[derive(Default)]
struct Reply {
    flags: u8,
    events: u32,
    synack: bool,
    established: bool,
}

impl Conn {
    fn new(id: u32, iss: u32) -> Conn {
        Conn {
            id,
            state: State::SynSent,
            iss,
            snd_una: iss,
            snd_nxt: iss.wrapping_add(1),
            snd_max: iss.wrapping_add(1),
            snd_wnd: 0,
            snd_wl1: 0,
            snd_wl2: 0,
            rcv_nxt: 0,
            mss: MSS_DEFAULT,
            rx: VecDeque::new(),
            tx: VecDeque::new(),
            ooo: Vec::new(),
            fin_queued: false,
            fin_sent: false,
            fin_acked: false,
            peer_fin: false,
            ack_pending: false,
            detached: false,
            error: 0,
            rto: RTO_INIT,
            srtt: 0,
            rttvar: 0,
            rtt_probe: None,
            rexmit_at: 0,
            retries: 0,
            persist_at: 0,
            persist_wait: 0,
            timewait_at: 0,
            dupacks: 0,
            fast_rexmit: false,
            in_recovery: false,
            cwnd: 0,
            ssthresh: u32::MAX / 2,
            retransmits: 0,
            parent: 0,
            backlog: 0,
            accept_q: VecDeque::new(),
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
        self.ooo.clear();
        self.rexmit_at = 0;
        self.persist_at = 0;
        EV_ERROR | EV_READABLE | EV_WRITABLE
    }

    fn synchronized(&self) -> bool {
        !matches!(self.state, State::Closed | State::Listen | State::SynSent | State::SynReceived)
    }

    fn input_syn_received(&mut self, s: &Segment, now: u64) -> Option<Reply> {
        let mut r = Reply::default();
        if s.flags & RST != 0 {
            if s.seq == self.rcv_nxt {
                self.state = State::Closed;
                self.detached = true;
                self.rexmit_at = 0;
            }
            return Some(r);
        }
        if s.flags & SYN != 0 {
            if s.seq.wrapping_add(1) == self.rcv_nxt {
                r.synack = true;
            }
            return Some(r);
        }
        if s.flags & ACK == 0 {
            return Some(r);
        }
        if s.ack != self.iss.wrapping_add(1) {
            r.flags = RST;
            return Some(r);
        }
        if let Some((seq, at)) = self.rtt_probe.take() {
            if le(seq, s.ack) {
                self.sample_rtt(now.saturating_sub(at));
            }
        }
        self.snd_una = s.ack;
        self.snd_wnd = s.window as u32;
        self.snd_wl1 = s.seq;
        self.snd_wl2 = s.ack;
        self.rexmit_at = 0;
        self.retries = 0;
        self.state = State::Established;
        None
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

    fn flight(&self) -> u32 {
        self.snd_max.wrapping_sub(self.snd_una)
    }

    fn data_in_flight(&self) -> usize {
        let fin = (self.fin_sent && !self.fin_acked) as usize;
        (self.snd_nxt.wrapping_sub(self.snd_una) as usize).saturating_sub(fin)
    }

    fn unsent(&self) -> usize {
        self.tx.len() - self.data_in_flight().min(self.tx.len())
    }

    fn arm(&mut self, now: u64) {
        if self.rexmit_at == 0 {
            self.rexmit_at = now + self.rto;
        }
    }

    fn sample_rtt(&mut self, r: u64) {
        if self.srtt == 0 {
            self.srtt = r.max(1);
            self.rttvar = r / 2;
        } else {
            let diff = self.srtt.abs_diff(r);
            self.rttvar = (3 * self.rttvar + diff) / 4;
            self.srtt = (7 * self.srtt + r) / 8;
        }
        self.rto = (self.srtt + (4 * self.rttvar).max(10)).clamp(RTO_MIN, RTO_MAX);
    }

    fn set_timewait(&mut self, now: u64) {
        self.state = State::TimeWait;
        self.timewait_at = now + 2 * MSL;
        self.rexmit_at = 0;
        self.persist_at = 0;
    }

    fn input_syn_sent(&mut self, s: &Segment, now: u64) -> Reply {
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
        if let Some((seq, at)) = self.rtt_probe.take() {
            if le(seq, s.ack) {
                self.sample_rtt(now.saturating_sub(at));
            }
        }
        self.rcv_nxt = s.seq.wrapping_add(1);
        self.snd_una = s.ack;
        self.snd_wnd = s.window as u32;
        self.snd_wl1 = s.seq;
        self.snd_wl2 = s.ack;
        self.mss = s.mss().unwrap_or(MSS_DEFAULT).clamp(64, MSS_OURS);
        self.cwnd = 10 * self.mss as u32;
        self.rexmit_at = 0;
        self.retries = 0;
        self.state = State::Established;
        r.flags = ACK;
        r.events = EV_CONNECTED | EV_WRITABLE;
        r
    }

    fn on_new_ack(&mut self, s: &Segment, now: u64) {
        let mut acked = s.ack.wrapping_sub(self.snd_una) as usize;
        if self.fin_queued && !self.fin_acked && self.fin_seq_acked(s.ack) {
            self.fin_acked = true;
            self.fin_sent = true;
            acked -= 1;
        }
        let data_acked = acked.min(self.tx.len());
        self.tx.drain(..data_acked);
        if let Some((seq, at)) = self.rtt_probe {
            if le(seq, s.ack) {
                self.rtt_probe = None;
                self.sample_rtt(now.saturating_sub(at));
            }
        }
        self.snd_una = s.ack;
        if lt(self.snd_nxt, self.snd_una) {
            self.snd_nxt = self.snd_una;
        }
        self.retries = 0;
        self.dupacks = 0;
        let mss = self.mss as u32;
        if self.in_recovery {
            self.in_recovery = false;
            self.cwnd = self.ssthresh;
        } else if self.cwnd < self.ssthresh {
            self.cwnd = self.cwnd.saturating_add((data_acked as u32).min(mss));
        } else {
            self.cwnd = self.cwnd.saturating_add((mss * mss / self.cwnd.max(1)).max(1));
        }
        self.rexmit_at = if self.snd_una == self.snd_max { 0 } else { now + self.rto };
    }

    fn fin_seq_acked(&self, ack: u32) -> bool {
        let fin_seq = self.snd_una.wrapping_add(self.tx.len() as u32);
        lt(fin_seq, ack)
    }

    fn take_ooo(&mut self) {
        loop {
            let nxt = self.rcv_nxt;
            self.ooo.retain(|(seq, d)| lt(nxt, seq.wrapping_add(d.len() as u32)));
            let Some(i) = self.ooo.iter().position(|(seq, _)| le(*seq, nxt)) else { break };
            let (seq, data) = self.ooo.swap_remove(i);
            let skip = self.rcv_nxt.wrapping_sub(seq) as usize;
            let room = RX_CAP - self.rx.len();
            let take = (data.len() - skip).min(room);
            if take == 0 {
                break;
            }
            if !self.detached {
                self.rx.extend(&data[skip..skip + take]);
            }
            self.rcv_nxt = self.rcv_nxt.wrapping_add(take as u32);
        }
    }

    fn keep_ooo(&mut self, seq: u32, data: &[u8]) {
        let limit = self.rcv_nxt.wrapping_add(self.rcv_wnd());
        let mut len = data.len();
        if lt(limit, seq.wrapping_add(len as u32)) {
            len = limit.wrapping_sub(seq) as usize;
        }
        if len == 0 || self.ooo.iter().any(|(s, d)| *s == seq && d.len() >= len) {
            return;
        }
        let held: usize = self.ooo.iter().map(|(_, d)| d.len()).sum();
        if held + len > RX_CAP || self.ooo.try_reserve(1).is_err() {
            return;
        }
        let mut v = Vec::new();
        if v.try_reserve_exact(len).is_err() {
            return;
        }
        v.extend_from_slice(&data[..len]);
        self.ooo.push((seq, v));
    }

    fn input(&mut self, s: &Segment, now: u64) -> Reply {
        if self.state == State::SynSent {
            return self.input_syn_sent(s, now);
        }
        let mut established = false;
        if self.state == State::SynReceived {
            if let Some(r) = self.input_syn_received(s, now) {
                return r;
            }
            established = true;
        }
        let mut r = self.input_synchronized(s, now);
        r.established = established;
        if established {
            r.events |= EV_CONNECTED | EV_WRITABLE;
        }
        r
    }

    fn input_synchronized(&mut self, s: &Segment, now: u64) -> Reply {
        let mut r = Reply::default();
        if !self.acceptable(s) {
            if s.flags & RST == 0 {
                r.flags = ACK;
            }
            if self.state == State::TimeWait && s.flags & FIN != 0 {
                self.timewait_at = now + 2 * MSL;
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
        if lt(self.snd_una, s.ack) && le(s.ack, self.snd_max) {
            self.on_new_ack(s, now);
            r.events |= EV_WRITABLE;
        } else if lt(self.snd_max, s.ack) {
            r.flags = ACK;
            return r;
        } else if s.ack == self.snd_una
            && s.payload.is_empty()
            && s.flags & FIN == 0
            && s.window as u32 == self.snd_wnd
            && self.flight() > 0
        {
            self.dupacks += 1;
            if self.dupacks == 3 {
                let mss = self.mss as u32;
                self.ssthresh = (self.flight() / 2).max(2 * mss);
                self.cwnd = self.ssthresh + 3 * mss;
                self.in_recovery = true;
                self.fast_rexmit = true;
                self.rtt_probe = None;
            } else if self.dupacks > 3 {
                self.cwnd = self.cwnd.saturating_add(self.mss as u32);
            }
        }
        if le(s.ack, self.snd_max) && (lt(self.snd_wl1, s.seq) || (self.snd_wl1 == s.seq && le(self.snd_wl2, s.ack))) {
            if self.snd_wnd == 0 && s.window != 0 {
                self.persist_at = 0;
                self.persist_wait = 0;
            }
            self.snd_wnd = s.window as u32;
            self.snd_wl1 = s.seq;
            self.snd_wl2 = s.ack;
            r.events |= EV_WRITABLE;
        }
        if self.fin_acked {
            match self.state {
                State::FinWait1 => self.state = State::FinWait2,
                State::Closing => self.set_timewait(now),
                State::LastAck => {
                    self.state = State::Closed;
                    self.rexmit_at = 0;
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
                self.take_ooo();
                r.events |= EV_READABLE;
            } else if receiving && lt(self.rcv_nxt, seq) {
                self.keep_ooo(seq, data);
            }
        }
        if s.flags & FIN != 0 && all_in && !self.peer_fin && seq.wrapping_add(data.len() as u32) == self.rcv_nxt {
            self.rcv_nxt = self.rcv_nxt.wrapping_add(1);
            self.peer_fin = true;
            r.flags = ACK;
            r.events |= EV_READABLE;
            match self.state {
                State::Established => self.state = State::CloseWait,
                State::FinWait1 if self.fin_acked => self.set_timewait(now),
                State::FinWait1 => self.state = State::Closing,
                State::FinWait2 => self.set_timewait(now),
                _ => {}
            }
        } else if s.flags & FIN != 0 && self.peer_fin {
            r.flags = ACK;
            if self.state == State::TimeWait {
                self.timewait_at = now + 2 * MSL;
            }
        }
        r
    }

    fn output(&mut self, key: &Key, out: &mut [u8], now: u64) -> Result<usize, i32> {
        if !self.synchronized() {
            return Ok(0);
        }
        let mss = self.mss as usize;
        if self.fast_rexmit {
            self.fast_rexmit = false;
            let n = self.tx.len().min(mss);
            if n > 0 {
                let mut payload = [0u8; MSS_OURS as usize];
                for (i, b) in self.tx.range(..n).enumerate() {
                    payload[i] = *b;
                }
                let h = self.header(key, self.snd_una, ACK);
                let len = build(out, key.local_ip, key.remote_ip, &h, &payload[..n])?;
                self.retransmits += 1;
                self.ack_pending = false;
                self.rexmit_at = now + self.rto;
                return Ok(len);
            }
        }
        let in_flight = self.snd_nxt.wrapping_sub(self.snd_una);
        let sent = self.data_in_flight();
        let unsent = self.unsent();
        let wnd = self.snd_wnd.min(self.cwnd.max(mss as u32));
        let room = wnd.saturating_sub(in_flight) as usize;
        let n = unsent.min(room).min(mss);
        let sending = matches!(
            self.state,
            State::Established | State::CloseWait | State::FinWait1 | State::Closing | State::LastAck
        );
        if n > 0 && sending && !self.fin_sent {
            let mut payload = [0u8; MSS_OURS as usize];
            for (i, b) in self.tx.range(sent..sent + n).enumerate() {
                payload[i] = *b;
            }
            let flags = if n == unsent { ACK | PSH } else { ACK };
            let h = self.header(key, self.snd_nxt, flags);
            let len = build(out, key.local_ip, key.remote_ip, &h, &payload[..n])?;
            let end = self.snd_nxt.wrapping_add(n as u32);
            if lt(self.snd_nxt, self.snd_max) {
                self.retransmits += 1;
            } else if self.rtt_probe.is_none() {
                self.rtt_probe = Some((end, now));
            }
            self.snd_nxt = end;
            if lt(self.snd_max, end) {
                self.snd_max = end;
            }
            self.ack_pending = false;
            self.arm(now);
            return Ok(len);
        }
        if self.fin_queued && !self.fin_sent && unsent == 0 && sending && !self.fin_acked {
            let h = self.header(key, self.snd_nxt, FIN | ACK);
            let len = build(out, key.local_ip, key.remote_ip, &h, &[])?;
            let end = self.snd_nxt.wrapping_add(1);
            if lt(self.snd_nxt, self.snd_max) {
                self.retransmits += 1;
            }
            self.snd_nxt = end;
            if lt(self.snd_max, end) {
                self.snd_max = end;
            }
            self.fin_sent = true;
            self.ack_pending = false;
            self.arm(now);
            self.state = match self.state {
                State::Established => State::FinWait1,
                State::CloseWait => State::LastAck,
                other => other,
            };
            return Ok(len);
        }
        if self.ack_pending {
            let h = self.header(key, self.snd_nxt, ACK);
            let len = build(out, key.local_ip, key.remote_ip, &h, &[])?;
            self.ack_pending = false;
            return Ok(len);
        }
        if unsent > 0 && sending && self.snd_wnd == 0 && in_flight == 0 && self.persist_at == 0 {
            self.persist_wait = self.rto;
            self.persist_at = now + self.persist_wait;
        }
        Ok(0)
    }

    fn syn(&self, key: &Key, out: &mut [u8]) -> Result<usize, i32> {
        let h = Header {
            sport: key.local_port,
            dport: key.remote_port,
            seq: self.iss,
            ack: 0,
            flags: SYN,
            window: self.rcv_wnd().min(0xFFFF) as u16,
        };
        let mss = MSS_OURS.to_be_bytes();
        build_with(out, key.local_ip, key.remote_ip, &h, &[2, 4, mss[0], mss[1]], &[])
    }

    fn syn_ack(&self, key: &Key, out: &mut [u8]) -> Result<usize, i32> {
        let h = Header {
            sport: key.local_port,
            dport: key.remote_port,
            seq: self.iss,
            ack: self.rcv_nxt,
            flags: SYN | ACK,
            window: self.rcv_wnd().min(0xFFFF) as u16,
        };
        let mss = MSS_OURS.to_be_bytes();
        build_with(out, key.local_ip, key.remote_ip, &h, &[2, 4, mss[0], mss[1]], &[])
    }

    fn tick(&mut self, key: &Key, now: u64, out: &mut [u8], ev: &mut u32) -> Result<usize, i32> {
        if self.state == State::SynReceived && self.rexmit_at != 0 && now >= self.rexmit_at {
            self.retries += 1;
            if self.retries > SYN_RETRIES {
                self.state = State::Closed;
                self.detached = true;
                self.rexmit_at = 0;
                return Ok(0);
            }
            self.rto = (self.rto * 2).min(RTO_MAX);
            self.rexmit_at = now + self.rto;
            self.rtt_probe = None;
            self.retransmits += 1;
            return self.syn_ack(key, out);
        }
        if self.detached && self.fin_queued && !self.fin_sent && self.synchronized() && self.rexmit_at == 0 {
            let n = self.output(key, out, now)?;
            if n > 0 {
                return Ok(n);
            }
        }
        if self.state == State::TimeWait && self.timewait_at != 0 && now >= self.timewait_at {
            self.state = State::Closed;
            self.timewait_at = 0;
            *ev |= EV_CLOSED;
            return Ok(0);
        }
        if self.rexmit_at != 0 && now >= self.rexmit_at {
            self.retries += 1;
            let limit = if self.state == State::SynSent { SYN_RETRIES } else { DATA_RETRIES };
            if self.retries > limit {
                *ev |= self.fail(ETIMEDOUT);
                return Ok(0);
            }
            self.rto = (self.rto * 2).min(RTO_MAX);
            self.rexmit_at = now + self.rto;
            self.rtt_probe = None;
            self.retransmits += 1;
            if self.state == State::SynSent {
                return self.syn(key, out);
            }
            let mss = self.mss as u32;
            self.ssthresh = (self.flight() / 2).max(2 * mss);
            self.cwnd = mss;
            self.in_recovery = false;
            self.dupacks = 0;
            self.snd_nxt = self.snd_una;
            if self.fin_sent && !self.fin_acked {
                self.fin_sent = false;
            }
            return self.output(key, out, now);
        }
        if self.persist_at != 0 && now >= self.persist_at {
            if self.snd_wnd != 0 || self.unsent() == 0 {
                self.persist_at = 0;
                return Ok(0);
            }
            self.persist_wait = (self.persist_wait * 2).min(RTO_MAX);
            self.persist_at = now + self.persist_wait;
            let h = self.header(key, self.snd_una.wrapping_sub(1), ACK);
            return build(out, key.local_ip, key.remote_ip, &h, &[]);
        }
        Ok(0)
    }

    fn finished(&self) -> bool {
        self.state == State::Closed || (self.state == State::TimeWait && self.timewait_at == 0)
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
    pub retransmits: u64,
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
    pub rto_ms: u32,
    pub cwnd: u32,
}

pub struct Stack {
    table: Table<Conn>,
    stats: TcpStats,
    next_id: u32,
    next_port: u16,
    retired_retransmits: u64,
    rng: u64,
}

impl Default for Stack {
    fn default() -> Self {
        Stack::with_seed(0x9E37_79B9_7F4A_7C15)
    }
}

impl Stack {
    pub fn with_seed(seed: u64) -> Stack {
        Stack {
            table: Table::default(),
            stats: TcpStats::default(),
            next_id: 1,
            next_port: PORT_FIRST,
            retired_retransmits: 0,
            rng: seed | 1,
        }
    }

    fn random(&mut self) -> u32 {
        self.rng ^= self.rng << 13;
        self.rng ^= self.rng >> 7;
        self.rng ^= self.rng << 17;
        (self.rng >> 16) as u32
    }

    fn new_id(&mut self) -> u32 {
        let id = self.next_id;
        self.next_id = self.next_id.wrapping_add(1).max(1);
        id
    }

    fn listener_input(&mut self, lid: u32, key: Key, s: &Segment, now: u64, out: &mut [u8]) -> Result<usize, i32> {
        if s.flags & RST != 0 {
            self.stats.rx_dropped += 1;
            return Ok(0);
        }
        if s.flags & ACK != 0 {
            let h = reset_for(s).ok_or(EINVAL)?;
            self.stats.tx_resets += 1;
            return build(out, key.local_ip, key.remote_ip, &h, &[]);
        }
        if s.flags & SYN == 0 {
            self.stats.rx_dropped += 1;
            return Ok(0);
        }
        let pending = self.table.iter().filter(|(_, c)| c.parent == lid).count();
        let (_, l) = self.conn(lid)?;
        if pending + l.accept_q.len() >= l.backlog as usize {
            self.stats.rx_dropped += 1;
            return Ok(0);
        }
        let id = self.new_id();
        let iss = self.random();
        let mut c = Conn::new(id, iss);
        c.state = State::SynReceived;
        c.parent = lid;
        c.rcv_nxt = s.seq.wrapping_add(1);
        c.snd_wnd = s.window as u32;
        c.mss = s.mss().unwrap_or(MSS_DEFAULT).clamp(64, MSS_OURS);
        c.cwnd = 10 * c.mss as u32;
        c.rtt_probe = Some((iss.wrapping_add(1), now));
        c.arm(now);
        let n = c.syn_ack(&key, out)?;
        self.table.insert(key, c)?;
        Ok(n)
    }

    pub fn input(
        &mut self,
        src: u32,
        dst: u32,
        seg: &[u8],
        now: u64,
        out: &mut [u8],
        ev: &mut TcpEvent,
    ) -> Result<usize, i32> {
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
        let found = self.table.lookup(&key).filter(|c| c.state != State::Closed).map(|c| (c.state, c.id));
        if let Some((State::Listen, lid)) = found {
            return self.listener_input(lid, key, &s, now, out);
        }
        if let Some(c) = self.table.lookup(&key).filter(|c| c.state != State::Closed) {
            let r = c.input(&s, now);
            ev.conn = c.id;
            ev.events = r.events;
            let parent = if r.established { c.parent } else { 0 };
            let n = if r.synack {
                c.syn_ack(&key, out)?
            } else if r.flags & RST != 0 {
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
            if parent != 0 {
                let child = ev.conn;
                match self.conn(parent) {
                    Ok((_, l)) if l.state == State::Listen => {
                        l.accept_q.push_back(child);
                        *ev = TcpEvent { conn: parent, events: EV_READABLE };
                    }
                    _ => {
                        if let Ok((_, c)) = self.conn(child) {
                            c.fail(ECONNRESET);
                            c.detached = true;
                        }
                    }
                }
            }
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
        let mut gone = 0;
        self.table.retain(|c| {
            let keep = !(c.detached && c.finished());
            if !keep {
                gone += c.retransmits;
            }
            keep
        });
        self.retired_retransmits += gone;
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

    pub fn connect(
        &mut self,
        local_ip: u32,
        remote_ip: u32,
        remote_port: u16,
        iss: u32,
        now: u64,
        out: &mut [u8],
    ) -> Result<(u32, usize), i32> {
        if remote_ip == 0 || remote_port == 0 || local_ip == 0 {
            return Err(EINVAL);
        }
        let port = self.free_port().ok_or(EADDRNOTAVAIL)?;
        let key = Key { local_ip, local_port: port, remote_ip, remote_port };
        let id = self.new_id();
        let mut c = Conn::new(id, iss);
        let n = c.syn(&key, out)?;
        c.rtt_probe = Some((iss.wrapping_add(1), now));
        c.arm(now);
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

    pub fn output(&mut self, id: u32, out: &mut [u8], now: u64) -> Result<(usize, u32), i32> {
        let (key, c) = self.conn(id)?;
        let n = c.output(&key, out, now)?;
        Ok((n, key.remote_ip))
    }

    pub fn tick(&mut self, now: u64, out: &mut [u8], ev: &mut TcpEvent) -> Result<(usize, u32), i32> {
        *ev = TcpEvent::default();
        let mut found = (0, 0);
        for (key, c) in self.table.iter_mut() {
            let mut e = 0;
            let n = c.tick(key, now, out, &mut e)?;
            if n > 0 || e != 0 {
                ev.conn = c.id;
                ev.events = e;
                found = (n, key.remote_ip);
                break;
            }
        }
        self.reap();
        Ok(found)
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

    pub fn port_busy(&self, port: u16, reuse: bool) -> bool {
        self.table
            .iter()
            .any(|(k, c)| k.local_port == port && (c.state == State::Listen || (!reuse && c.state != State::Closed)))
    }

    pub fn listen(&mut self, local_ip: u32, port: u16, backlog: u32, reuse: bool) -> Result<u32, i32> {
        if port == 0 {
            return Err(EINVAL);
        }
        if self.port_busy(port, reuse) {
            return Err(EADDRINUSE);
        }
        let id = self.new_id();
        let mut c = Conn::new(id, 0);
        c.state = State::Listen;
        c.backlog = backlog.clamp(1, 128);
        let key = Key { local_ip, local_port: port, remote_ip: 0, remote_port: 0 };
        self.table.insert(key, c)?;
        Ok(id)
    }

    pub fn accept(&mut self, lid: u32) -> Result<u32, i32> {
        let (_, l) = self.conn(lid)?;
        if l.state != State::Listen {
            return Err(EINVAL);
        }
        let child = l.accept_q.pop_front().ok_or(EAGAIN)?;
        if let Ok((_, c)) = self.conn(child) {
            c.parent = 0;
        }
        Ok(child)
    }

    pub fn close(&mut self, id: u32) -> Result<(), i32> {
        let (_, c) = self.conn(id)?;
        if c.state == State::Listen {
            c.state = State::Closed;
            let queued: Vec<u32> = c.accept_q.drain(..).collect();
            for (_, child) in self.table.iter_mut() {
                if child.parent == id || queued.contains(&child.id) {
                    child.parent = 0;
                    child.detached = true;
                    child.rx.clear();
                    match child.state {
                        State::Established | State::CloseWait => child.fin_queued = true,
                        _ => {
                            child.state = State::Closed;
                            child.rexmit_at = 0;
                        }
                    }
                }
            }
            return Ok(());
        }
        match c.state {
            State::SynSent => {
                c.state = State::Closed;
                c.rexmit_at = 0;
            }
            State::Established | State::CloseWait => c.fin_queued = true,
            _ => {}
        }
        Ok(())
    }

    pub fn detach(&mut self, id: u32) {
        if let Ok((_, c)) = self.conn(id) {
            c.detached = true;
            c.rx.clear();
            c.ooo.clear();
        }
        self.reap();
    }

    pub fn info(&mut self, id: u32) -> Result<TcpInfo, i32> {
        let (key, c) = self.conn(id)?;
        Ok(TcpInfo {
            state: c.state as u32,
            error: c.error,
            readable: if c.state == State::Listen { c.accept_q.len() as u32 } else { c.rx.len() as u32 },
            writable: (TX_CAP - c.tx.len()) as u32,
            unacked: c.snd_max.wrapping_sub(c.snd_una),
            eof: (c.peer_fin && c.rx.is_empty()) as u32,
            local_ip: key.local_ip,
            remote_ip: key.remote_ip,
            local_port: key.local_port,
            remote_port: key.remote_port,
            rto_ms: c.rto as u32,
            cwnd: c.cwnd,
        })
    }

    pub fn stats(&self) -> TcpStats {
        let mut st = self.stats;
        st.connections = self.table.len() as u64;
        st.retransmits = self.retired_retransmits + self.table.iter().map(|(_, c)| c.retransmits).sum::<u64>();
        st
    }
}

#[no_mangle]
pub extern "C" fn tcp_stack_new(seed: u64) -> *mut Stack {
    let p = unsafe { alloc::alloc::alloc(core::alloc::Layout::new::<Stack>()) } as *mut Stack;
    if !p.is_null() {
        unsafe { p.write(Stack::with_seed(seed)) };
    }
    p
}

#[no_mangle]
pub unsafe extern "C" fn tcp_listen(st: *mut Stack, local_ip: u32, port: u16, backlog: u32, reuse: bool) -> i64 {
    if st.is_null() {
        return -(EINVAL as i64);
    }
    match (*st).listen(local_ip, port, backlog, reuse) {
        Ok(id) => id as i64,
        Err(e) => -(e as i64),
    }
}

#[no_mangle]
pub unsafe extern "C" fn tcp_accept(st: *mut Stack, lid: u32) -> i64 {
    if st.is_null() {
        return -(EINVAL as i64);
    }
    match (*st).accept(lid) {
        Ok(id) => id as i64,
        Err(e) => -(e as i64),
    }
}

#[no_mangle]
pub unsafe extern "C" fn tcp_port_busy(st: *const Stack, port: u16, reuse: bool) -> bool {
    !st.is_null() && (*st).port_busy(port, reuse)
}

fn ret(r: Result<usize, i32>) -> i64 {
    match r {
        Ok(n) => n as i64,
        Err(e) => -(e as i64),
    }
}

#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn tcp_input(
    st: *mut Stack,
    src: u32,
    dst: u32,
    seg: *const u8,
    len: usize,
    now: u64,
    out: *mut u8,
    cap: usize,
    ev: *mut TcpEvent,
) -> i64 {
    if st.is_null() || seg.is_null() || out.is_null() || ev.is_null() {
        return -(EINVAL as i64);
    }
    let seg = core::slice::from_raw_parts(seg, len);
    let out = core::slice::from_raw_parts_mut(out, cap);
    ret((*st).input(src, dst, seg, now, out, &mut *ev))
}

#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn tcp_connect(
    st: *mut Stack,
    local_ip: u32,
    remote_ip: u32,
    remote_port: u16,
    iss: u32,
    now: u64,
    out: *mut u8,
    cap: usize,
    id: *mut u32,
) -> i64 {
    if st.is_null() || out.is_null() || id.is_null() {
        return -(EINVAL as i64);
    }
    let out = core::slice::from_raw_parts_mut(out, cap);
    match (*st).connect(local_ip, remote_ip, remote_port, iss, now, out) {
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
pub unsafe extern "C" fn tcp_output(st: *mut Stack, id: u32, now: u64, out: *mut u8, cap: usize, dst: *mut u32) -> i64 {
    if st.is_null() || out.is_null() || dst.is_null() {
        return -(EINVAL as i64);
    }
    let out = core::slice::from_raw_parts_mut(out, cap);
    match (*st).output(id, out, now) {
        Ok((n, ip)) => {
            *dst = ip;
            n as i64
        }
        Err(e) => -(e as i64),
    }
}

#[no_mangle]
pub unsafe extern "C" fn tcp_tick(
    st: *mut Stack,
    now: u64,
    out: *mut u8,
    cap: usize,
    dst: *mut u32,
    ev: *mut TcpEvent,
) -> i64 {
    if st.is_null() || out.is_null() || dst.is_null() || ev.is_null() {
        return -(EINVAL as i64);
    }
    let out = core::slice::from_raw_parts_mut(out, cap);
    match (*st).tick(now, out, &mut *ev) {
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
