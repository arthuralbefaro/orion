use super::*;
use std::vec::Vec as StdVec;

const HOST: u32 = 0x0A00_0202;
const ME: u32 = 0x0A00_020F;

fn reference_checksum(src: u32, dst: u32, seg: &[u8]) -> u16 {
    let mut words: StdVec<u16> = StdVec::new();
    for ip in [src, dst] {
        words.push((ip >> 16) as u16);
        words.push(ip as u16);
    }
    words.push(PROTO_TCP as u16);
    words.push(seg.len() as u16);
    let mut padded = seg.to_vec();
    if padded.len() % 2 == 1 {
        padded.push(0);
    }
    for c in padded.chunks(2) {
        words.push(((c[0] as u16) << 8) | c[1] as u16);
    }
    let mut sum: u64 = words.iter().map(|&w| w as u64).sum();
    while sum >> 16 != 0 {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    !(sum as u16)
}

fn segment(flags: u8, seq: u32, ack: u32, payload: &[u8]) -> StdVec<u8> {
    let mut buf = vec![0u8; HDR_LEN + payload.len()];
    let h = Header { sport: 40000, dport: 7, seq, ack, flags, window: 8192 };
    let n = build(&mut buf, HOST, ME, &h, payload).expect("monta");
    buf.truncate(n);
    buf
}

struct Rng(u64);

impl Rng {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }
}

#[test]
fn checksum_matches_a_plain_implementation() {
    let mut rng = Rng(0x9E37_79B9_7F4A_7C15);
    for len in 0..200usize {
        let payload: StdVec<u8> = (0..len).map(|_| rng.next() as u8).collect();
        let seg = segment(ACK | PSH, rng.next() as u32, rng.next() as u32, &payload);
        let mut zeroed = seg.clone();
        zeroed[16] = 0;
        zeroed[17] = 0;
        let stored = u16::from_be_bytes([seg[16], seg[17]]);
        assert_eq!(stored, reference_checksum(HOST, ME, &zeroed), "tamanho {}", len);
        assert_eq!(checksum(HOST, ME, &seg), 0, "segmento valido soma zero ({} bytes)", len);
    }
}

#[test]
fn parse_reads_the_fields_and_refuses_bad_segments() {
    let seg = segment(SYN, 1000, 0, b"oi");
    let s = parse(HOST, ME, &seg).expect("valido");
    assert_eq!((s.sport, s.dport, s.seq, s.ack, s.flags, s.window), (40000, 7, 1000, 0, SYN, 8192));
    assert_eq!(s.payload, b"oi");
    assert_eq!(s.seq_len(), 3);
    assert_eq!(parse(HOST, ME, &seg[..19]), Err(EBADMSG));
    let mut bad = seg.clone();
    bad[12] = 4 << 4;
    assert_eq!(parse(HOST, ME, &bad), Err(EBADMSG));
    bad[12] = 15 << 4;
    assert_eq!(parse(HOST, ME, &bad), Err(EBADMSG));
    let mut flipped = seg.clone();
    flipped[20] ^= 1;
    assert_eq!(parse(HOST, ME, &flipped), Err(EINVAL));
    assert_eq!(parse(HOST, ME ^ 1, &seg), Err(EINVAL), "o pseudo-cabecalho entra na soma");
}

#[test]
fn options_are_skipped() {
    let mut seg = segment(SYN, 5, 0, &[]);
    seg.extend_from_slice(&[2, 4, 0x05, 0xB4, 1, 1, 4, 2]);
    seg.extend_from_slice(b"xy");
    seg[12] = 7 << 4;
    seg[16] = 0;
    seg[17] = 0;
    let c = checksum(HOST, ME, &seg);
    seg[16..18].copy_from_slice(&c.to_be_bytes());
    let s = parse(HOST, ME, &seg).expect("com opcoes");
    assert_eq!(s.payload, b"xy");
}

#[test]
fn reset_follows_rfc_793() {
    let syn = segment(SYN, 1000, 0, &[]);
    let h = reset_for(&parse(HOST, ME, &syn).unwrap()).expect("SYN sem ACK recebe RST");
    assert_eq!((h.sport, h.dport, h.seq, h.ack, h.flags), (7, 40000, 0, 1001, RST | ACK));

    let data_fin = segment(FIN | PSH, 0xFFFF_FFFE, 0, b"abc");
    let h = reset_for(&parse(HOST, ME, &data_fin).unwrap()).unwrap();
    assert_eq!((h.seq, h.ack), (0, 2), "SEG.SEQ + SEG.LEN, com volta no 2^32");

    let ack = segment(ACK, 50, 777, b"dados");
    let h = reset_for(&parse(HOST, ME, &ack).unwrap()).unwrap();
    assert_eq!((h.seq, h.ack, h.flags), (777, 0, RST), "com ACK o RST usa SEG.ACK e nao tem ACK");

    let rst = segment(RST | ACK, 1, 2, &[]);
    assert!(reset_for(&parse(HOST, ME, &rst).unwrap()).is_none(), "RST nunca responde RST");
}

#[test]
fn table_finds_exact_then_listener() {
    let mut t: Table<u32> = Table::default();
    let listen = Key { local_ip: 0, local_port: 80, remote_ip: 0, remote_port: 0 };
    let conn = Key { local_ip: ME, local_port: 80, remote_ip: HOST, remote_port: 40000 };
    assert_eq!(t.insert(listen, 1), Ok(()));
    assert_eq!(t.insert(listen, 9), Err(EADDRINUSE));
    assert_eq!(t.lookup(&conn).copied(), Some(1), "sem conexao, vale quem escuta");
    assert_eq!(t.insert(conn, 2), Ok(()));
    assert_eq!(t.lookup(&conn).copied(), Some(2), "a conexao exata vem antes");
    let other = Key { remote_port: 40001, ..conn };
    assert_eq!(t.lookup(&other).copied(), Some(1));
    let elsewhere = Key { local_port: 81, ..conn };
    assert_eq!(t.lookup(&elsewhere), None);
    assert_eq!(t.remove(&listen), Some(1));
    assert_eq!(t.lookup(&other), None);
    assert_eq!(t.len(), 1);
}

#[test]
fn closed_port_answers_with_a_valid_reset() {
    let mut st = Stack::default();
    let syn = segment(SYN, 4242, 0, &[]);
    let mut out = [0u8; 64];
    let n = st.input(HOST, ME, &syn, 0, &mut out, &mut TcpEvent::default()).expect("entrada");
    assert_eq!(n, HDR_LEN);
    let r = parse(ME, HOST, &out[..n]).expect("o RST tem checksum certo do nosso lado");
    assert_eq!((r.sport, r.dport, r.seq, r.ack, r.flags), (7, 40000, 0, 4243, RST | ACK));
    assert_eq!(st.input(HOST, ME, &segment(RST, 1, 0, &[]), 0, &mut out, &mut TcpEvent::default()), Ok(0));
    let mut broken = syn.clone();
    broken[4] ^= 0x80;
    assert_eq!(st.input(HOST, ME, &broken, 0, &mut out, &mut TcpEvent::default()), Err(EINVAL));
    assert_eq!(st.input(HOST, ME, &syn, 0, &mut [0u8; 10], &mut TcpEvent::default()), Err(ENOBUFS));
    let s = st.stats();
    assert_eq!((s.rx_segments, s.rx_bad, s.tx_resets, s.rx_dropped), (3, 1, 1, 1));
}

#[test]
fn random_input_never_panics() {
    let mut rng = Rng(0xDEAD_BEEF_1234_5678);
    let mut st = Stack::default();
    let mut out = [0u8; 128];
    for i in 0..20000 {
        let len = (rng.next() % 80) as usize;
        let mut seg: StdVec<u8> = (0..len).map(|_| rng.next() as u8).collect();
        if i % 2 == 0 && len >= HDR_LEN {
            seg[12] = ((5 + rng.next() % 11) as u8) << 4;
            seg[16] = 0;
            seg[17] = 0;
            let c = checksum(HOST, ME, &seg);
            seg[16..18].copy_from_slice(&c.to_be_bytes());
        }
        let _ = st.input(HOST, ME, &seg, 0, &mut out, &mut TcpEvent::default());
    }
    assert!(st.stats().tx_resets > 0);
}

struct Peer {
    port: u16,
    local_port: u16,
}

impl Peer {
    fn seg(&self, flags: u8, seq: u32, ack: u32, window: u16, opts: &[u8], payload: &[u8]) -> StdVec<u8> {
        let mut buf = vec![0u8; HDR_LEN + opts.len() + payload.len()];
        let h = Header { sport: self.port, dport: self.local_port, seq, ack, flags, window };
        let n = build_with(&mut buf, HOST, ME, &h, opts, payload).expect("monta");
        buf.truncate(n);
        buf
    }
}

fn feed(st: &mut Stack, seg: &[u8]) -> (Option<Segment<'static>>, TcpEvent) {
    let mut out = vec![0u8; 2048];
    let mut ev = TcpEvent::default();
    let n = st.input(HOST, ME, seg, 0, &mut out, &mut ev).expect("entrada");
    out.truncate(n);
    let leaked: &'static [u8] = Box::leak(out.into_boxed_slice());
    let reply = if n == 0 { None } else { Some(parse(ME, HOST, leaked).expect("resposta valida")) };
    (reply, ev)
}

fn drain(st: &mut Stack, id: u32) -> StdVec<Segment<'static>> {
    let mut segs = StdVec::new();
    loop {
        let mut out = vec![0u8; 2048];
        let (n, ip) = st.output(id, &mut out, 0).expect("saida");
        if n == 0 {
            return segs;
        }
        assert_eq!(ip, HOST);
        out.truncate(n);
        let leaked: &'static [u8] = Box::leak(out.into_boxed_slice());
        segs.push(parse(ME, HOST, leaked).expect("segmento valido"));
    }
}

const ISS: u32 = 0xFFFF_FF00;
const PEER_ISS: u32 = 5000;

fn connected(window: u16, mss: u16) -> (Stack, u32, Peer) {
    let mut st = Stack::default();
    let mut out = [0u8; 64];
    let (id, n) = st.connect(ME, HOST, 80, ISS, 0, &mut out).expect("connect");
    let syn = parse(ME, HOST, &out[..n]).expect("SYN valido");
    assert_eq!((syn.flags, syn.seq, syn.dport), (SYN, ISS, 80));
    assert_eq!(syn.mss(), Some(MSS_OURS));
    assert_eq!(st.info(id).unwrap().state, State::SynSent as u32);
    let peer = Peer { port: 80, local_port: syn.sport };
    let m = mss.to_be_bytes();
    let (r, ev) = feed(&mut st, &peer.seg(SYN | ACK, PEER_ISS, ISS.wrapping_add(1), window, &[2, 4, m[0], m[1]], &[]));
    let r = r.expect("ACK do handshake");
    assert_eq!((r.flags, r.seq, r.ack), (ACK, ISS.wrapping_add(1), PEER_ISS + 1));
    assert_eq!(ev, TcpEvent { conn: id, events: EV_CONNECTED | EV_WRITABLE });
    assert_eq!(st.info(id).unwrap().state, State::Established as u32);
    (st, id, peer)
}

#[test]
fn handshake_then_send_within_the_peer_window() {
    let (mut st, id, peer) = connected(4000, 1000);
    let data: StdVec<u8> = (0..5000u32).map(|i| (i * 7 + i / 251) as u8).collect();
    assert_eq!(st.write(id, &data), Ok(5000));
    let first = drain(&mut st, id);
    assert_eq!(first.len(), 4, "a janela de 4000 com MSS 1000 da quatro segmentos");
    assert!(first.iter().all(|s| s.payload.len() == 1000));
    assert_eq!(first[0].seq, ISS.wrapping_add(1), "o seq passa pelo 2^32 sem erro");
    assert_eq!(st.info(id).unwrap().unacked, 4000);
    let (_, ev) = feed(&mut st, &peer.seg(ACK, PEER_ISS + 1, ISS.wrapping_add(2001), 4000, &[], &[]));
    assert!(ev.events & EV_WRITABLE != 0);
    let second = drain(&mut st, id);
    assert_eq!(second.iter().map(|s| s.payload.len()).sum::<usize>(), 1000);
    assert_eq!(second.last().unwrap().flags, ACK | PSH);
    let mut got = StdVec::new();
    for s in first.iter().chain(second.iter()) {
        got.extend_from_slice(s.payload);
    }
    assert_eq!(got, data);
    feed(&mut st, &peer.seg(ACK, PEER_ISS + 1, ISS.wrapping_add(5001), 4000, &[], &[]));
    let info = st.info(id).unwrap();
    assert_eq!((info.unacked, info.writable), (0, TX_CAP as u32));
}

#[test]
fn zero_window_stops_and_window_update_resumes() {
    let (mut st, id, peer) = connected(0, 1460);
    st.write(id, b"espera").unwrap();
    assert!(drain(&mut st, id).is_empty());
    feed(&mut st, &peer.seg(ACK, PEER_ISS + 1, ISS.wrapping_add(1), 100, &[], &[]));
    let segs = drain(&mut st, id);
    assert_eq!(segs.len(), 1);
    assert_eq!(segs[0].payload, b"espera");
}

#[test]
fn receive_in_order_and_ack_everything() {
    let (mut st, id, peer) = connected(8192, 1460);
    let (r, ev) = feed(&mut st, &peer.seg(ACK | PSH, PEER_ISS + 1, ISS.wrapping_add(1), 8192, &[], b"hello "));
    assert_eq!(r.unwrap().ack, PEER_ISS + 7);
    assert!(ev.events & EV_READABLE != 0);
    let (r, _) = feed(&mut st, &peer.seg(ACK, PEER_ISS + 100, ISS.wrapping_add(1), 8192, &[], b"fora de ordem"));
    assert_eq!(r.unwrap().ack, PEER_ISS + 7, "fora de ordem recebe ACK repetido");
    let (r, _) = feed(&mut st, &peer.seg(ACK, PEER_ISS + 1, ISS.wrapping_add(1), 8192, &[], b"hello world"));
    assert_eq!(r.unwrap().ack, PEER_ISS + 12, "retransmissao com sobreposicao entrega so o novo");
    let mut buf = [0u8; 64];
    let n = st.read(id, &mut buf).unwrap();
    assert_eq!(&buf[..n], b"hello world");
    assert_eq!(st.read(id, &mut buf), Ok(0));
}

#[test]
fn receive_window_is_respected() {
    let (mut st, id, peer) = connected(8192, 1460);
    let chunk = vec![b'z'; 1000];
    let mut seq = PEER_ISS + 1;
    let mut last = None;
    for _ in 0..40 {
        let (r, _) = feed(&mut st, &peer.seg(ACK, seq, ISS.wrapping_add(1), 8192, &[], &chunk));
        let r = r.unwrap();
        seq = r.ack;
        last = Some(r.window);
    }
    assert_eq!(seq, PEER_ISS + 1 + RX_CAP as u32, "aceita ate encher o buffer");
    assert_eq!(last, Some(0));
    let mut buf = vec![0u8; 4000];
    assert_eq!(st.read(id, &mut buf), Ok(4000));
    let segs = drain(&mut st, id);
    assert_eq!(segs.len(), 1, "a janela reaberta gera um ACK de atualizacao");
    assert_eq!(segs[0].window, 4000);
}

#[test]
fn active_close_goes_through_fin_wait() {
    let (mut st, id, peer) = connected(8192, 1460);
    st.write(id, b"tchau").unwrap();
    st.close(id).unwrap();
    assert_eq!(st.write(id, b"x"), Err(EPIPE));
    let segs = drain(&mut st, id);
    assert_eq!(segs.len(), 2);
    assert_eq!(segs[1].flags, FIN | ACK);
    assert_eq!(st.info(id).unwrap().state, State::FinWait1 as u32);
    feed(&mut st, &peer.seg(ACK, PEER_ISS + 1, ISS.wrapping_add(7), 8192, &[], &[]));
    assert_eq!(st.info(id).unwrap().state, State::FinWait2 as u32);
    assert!(drain(&mut st, id).is_empty(), "nada a mandar depois do FIN reconhecido");
    let (r, ev) = feed(&mut st, &peer.seg(FIN | ACK, PEER_ISS + 1, ISS.wrapping_add(7), 8192, &[], &[]));
    assert_eq!(r.unwrap().ack, PEER_ISS + 2);
    assert!(ev.events & EV_READABLE != 0);
    let info = st.info(id).unwrap();
    assert_eq!((info.state, info.eof), (State::TimeWait as u32, 1));
    st.detach(id);
    assert_eq!(st.stats().connections, 1, "TIME_WAIT segura a conexao por 2*MSL");
    let mut out = [0u8; 64];
    let mut ev = TcpEvent::default();
    assert_eq!(st.tick(2 * MSL - 1, &mut out, &mut ev), Ok((0, 0)));
    assert_eq!(st.stats().connections, 1);
    st.tick(2 * MSL + 1, &mut out, &mut ev).unwrap();
    assert_eq!(st.stats().connections, 0, "depois de 2*MSL a conexao solta sai da tabela");
}

#[test]
fn passive_close_goes_through_last_ack() {
    let (mut st, id, peer) = connected(8192, 1460);
    let (r, _) = feed(&mut st, &peer.seg(FIN | ACK | PSH, PEER_ISS + 1, ISS.wrapping_add(1), 8192, &[], b"fim"));
    assert_eq!(r.unwrap().ack, PEER_ISS + 5);
    let info = st.info(id).unwrap();
    assert_eq!((info.state, info.eof, info.readable), (State::CloseWait as u32, 0, 3));
    let mut buf = [0u8; 8];
    assert_eq!(st.read(id, &mut buf), Ok(3));
    assert_eq!(st.info(id).unwrap().eof, 1);
    st.write(id, b"resposta").unwrap();
    st.close(id).unwrap();
    let segs = drain(&mut st, id);
    assert_eq!(segs.last().unwrap().flags, FIN | ACK);
    assert_eq!(st.info(id).unwrap().state, State::LastAck as u32);
    let (_, ev) = feed(&mut st, &peer.seg(ACK, PEER_ISS + 5, ISS.wrapping_add(10), 8192, &[], &[]));
    assert!(ev.events & EV_CLOSED != 0);
    assert_eq!(st.info(id).unwrap().state, State::Closed as u32);
}

#[test]
fn reset_and_refusal() {
    let (mut st, id, peer) = connected(8192, 1460);
    let (_, ev) = feed(&mut st, &peer.seg(RST, PEER_ISS + 1, 0, 0, &[], &[]));
    assert!(ev.events & EV_ERROR != 0);
    assert_eq!(st.info(id).unwrap().error, ECONNRESET);
    assert_eq!(st.read(id, &mut [0u8; 4]), Err(ECONNRESET));
    let (_, ev) = feed(&mut st, &peer.seg(RST, PEER_ISS + 999_999, 0, 0, &[], &[]));
    assert_eq!(ev.events, 0, "a conexao ja fechada nao recebe mais nada");

    let mut st2 = Stack::default();
    let mut out = [0u8; 64];
    let (id2, n) = st2.connect(ME, HOST, 81, 77, 0, &mut out).unwrap();
    let peer2 = Peer { port: 81, local_port: parse(ME, HOST, &out[..n]).unwrap().sport };
    let (r, ev) = feed(&mut st2, &peer2.seg(ACK, 1, 12345, 0, &[], &[]));
    let r = r.expect("ACK que nao reconhece o SYN recebe RST");
    assert_eq!((r.flags, r.seq), (RST, 12345));
    assert_eq!(ev.events, 0);
    let (_, ev) = feed(&mut st2, &peer2.seg(RST | ACK, 0, 78, 0, &[], &[]));
    assert!(ev.events & EV_ERROR != 0);
    assert_eq!(st2.info(id2).unwrap().error, ECONNREFUSED);
}

#[test]
fn ports_are_distinct_and_connections_counted() {
    let mut st = Stack::default();
    let mut out = [0u8; 64];
    let mut ports = StdVec::new();
    for i in 0..50 {
        let (id, n) = st.connect(ME, HOST, 80, i, 0, &mut out).unwrap();
        ports.push(parse(ME, HOST, &out[..n]).unwrap().sport);
        assert_eq!(st.info(id).unwrap().local_port, *ports.last().unwrap());
    }
    ports.sort();
    ports.dedup();
    assert_eq!(ports.len(), 50);
    assert_eq!(st.stats().connections, 50);
    assert_eq!(st.connect(0, HOST, 80, 1, 0, &mut out), Err(EINVAL));
}

#[test]
fn random_segments_on_a_live_connection_never_panic() {
    let mut rng = Rng(0x1234_5678_9ABC_DEF1);
    for round in 0..200 {
        let (mut st, id, peer) = connected(8192, 536);
        st.write(id, &vec![1u8; (rng.next() % 3000) as usize]).unwrap();
        for i in 0..60 {
            let flags = (rng.next() as u8) & 0x1F;
            let base = if i % 3 == 0 { PEER_ISS + 1 } else { rng.next() as u32 };
            let ack = if i % 2 == 0 { ISS.wrapping_add(rng.next() as u32 % 4000) } else { rng.next() as u32 };
            let payload = vec![b'q'; (rng.next() % 1500) as usize];
            let _ = st.input(HOST, ME, &peer.seg(flags, base, ack, rng.next() as u16, &[], &payload), 0, &mut [0u8; 2048], &mut TcpEvent::default());
            let _ = drain(&mut st, id);
            let _ = st.read(id, &mut [0u8; 700]);
            if round % 7 == 0 && i == 30 {
                let _ = st.close(id);
            }
        }
        st.detach(id);
    }
}

fn drain_at(st: &mut Stack, id: u32, now: u64) -> StdVec<Segment<'static>> {
    let mut segs = StdVec::new();
    loop {
        let mut out = vec![0u8; 2048];
        let (n, _) = st.output(id, &mut out, now).expect("saida");
        if n == 0 {
            return segs;
        }
        out.truncate(n);
        let leaked: &'static [u8] = Box::leak(out.into_boxed_slice());
        segs.push(parse(ME, HOST, leaked).expect("segmento valido"));
    }
}

fn tick_at(st: &mut Stack, now: u64) -> (StdVec<Segment<'static>>, StdVec<TcpEvent>) {
    let mut segs = StdVec::new();
    let mut evs = StdVec::new();
    loop {
        let mut out = vec![0u8; 2048];
        let mut ev = TcpEvent::default();
        let (n, _) = st.tick(now, &mut out, &mut ev).expect("tick");
        if ev.conn != 0 && ev.events != 0 {
            evs.push(ev);
        }
        if n == 0 && ev.conn == 0 {
            return (segs, evs);
        }
        if n > 0 {
            out.truncate(n);
            let leaked: &'static [u8] = Box::leak(out.into_boxed_slice());
            segs.push(parse(ME, HOST, leaked).expect("segmento valido"));
        }
    }
}

fn feed_at(st: &mut Stack, seg: &[u8], now: u64) -> (Option<Segment<'static>>, TcpEvent) {
    let mut out = vec![0u8; 2048];
    let mut ev = TcpEvent::default();
    let n = st.input(HOST, ME, seg, now, &mut out, &mut ev).expect("entrada");
    out.truncate(n);
    let leaked: &'static [u8] = Box::leak(out.into_boxed_slice());
    let reply = if n == 0 { None } else { Some(parse(ME, HOST, leaked).expect("resposta valida")) };
    (reply, ev)
}

#[test]
fn syn_is_retransmitted_with_backoff_then_times_out() {
    let mut st = Stack::default();
    let mut out = [0u8; 64];
    let (id, _) = st.connect(ME, HOST, 80, 9, 0, &mut out).unwrap();
    assert!(tick_at(&mut st, RTO_INIT - 1).0.is_empty());
    let mut at = 0;
    let mut rto = RTO_INIT;
    for _ in 0..SYN_RETRIES {
        at += rto;
        let (segs, _) = tick_at(&mut st, at);
        assert_eq!(segs.len(), 1, "um SYN por vencimento");
        assert_eq!((segs[0].flags, segs[0].seq), (SYN, 9));
        assert!(tick_at(&mut st, at + 1).0.is_empty());
        rto *= 2;
    }
    at += rto;
    let (segs, evs) = tick_at(&mut st, at);
    assert!(segs.is_empty());
    assert_eq!(evs.len(), 1);
    assert!(evs[0].events & EV_ERROR != 0);
    assert_eq!(st.info(id).unwrap().error, ETIMEDOUT);
}

#[test]
fn rtt_sample_sets_the_rto() {
    let mut st = Stack::default();
    let mut out = [0u8; 64];
    let (id, n) = st.connect(ME, HOST, 80, ISS, 0, &mut out).unwrap();
    let peer = Peer { port: 80, local_port: parse(ME, HOST, &out[..n]).unwrap().sport };
    feed_at(&mut st, &peer.seg(SYN | ACK, PEER_ISS, ISS.wrapping_add(1), 8192, &[], &[]), 100);
    assert_eq!(st.info(id).unwrap().rto_ms, 300, "RTT de 100 ms: SRTT 100 + 4 * 50");
}

#[test]
fn lost_data_is_resent_from_snd_una() {
    let (mut st, id, peer) = connected(65535, 1000);
    let data: StdVec<u8> = (0..3000u32).map(|i| i as u8).collect();
    st.write(id, &data).unwrap();
    let first = drain_at(&mut st, id, 0);
    assert_eq!(first.len(), 3);
    let rto = st.info(id).unwrap().rto_ms as u64;
    assert!(tick_at(&mut st, rto - 1).0.is_empty());
    let (again, _) = tick_at(&mut st, rto);
    assert_eq!(again.len(), 1, "a janela de congestionamento cai para 1 MSS");
    assert_eq!((again[0].seq, again[0].payload.len()), (ISS.wrapping_add(1), 1000));
    assert_eq!(st.info(id).unwrap().rto_ms as u64, 2 * rto, "backoff exponencial");
    feed_at(&mut st, &peer.seg(ACK, PEER_ISS + 1, ISS.wrapping_add(1001), 65535, &[], &[]), rto + 10);
    let rest = drain_at(&mut st, id, rto + 10);
    assert_eq!(rest.len(), 2, "o resto sai de novo, a partir de onde parou");
    assert_eq!(rest[0].seq, ISS.wrapping_add(1001));
    let mut got = again[0].payload.to_vec();
    for s in &rest {
        got.extend_from_slice(s.payload);
    }
    assert_eq!(got, data);
    feed_at(&mut st, &peer.seg(ACK, PEER_ISS + 1, ISS.wrapping_add(3001), 65535, &[], &[]), rto + 20);
    assert_eq!(st.info(id).unwrap().unacked, 0);
    assert!(tick_at(&mut st, 100_000).0.is_empty(), "sem nada em voo nao ha retransmissao");
    assert!(st.stats().retransmits >= 3);
}

#[test]
fn three_duplicate_acks_trigger_fast_retransmit() {
    let (mut st, id, peer) = connected(65535, 1000);
    st.write(id, &vec![7u8; 6000]).unwrap();
    assert_eq!(drain_at(&mut st, id, 0).len(), 6);
    let una = ISS.wrapping_add(1001);
    feed_at(&mut st, &peer.seg(ACK, PEER_ISS + 1, una, 65535, &[], &[]), 10);
    for _ in 0..2 {
        feed_at(&mut st, &peer.seg(ACK, PEER_ISS + 1, una, 65535, &[], &[]), 11);
        assert!(drain_at(&mut st, id, 11).is_empty());
    }
    feed_at(&mut st, &peer.seg(ACK, PEER_ISS + 1, una, 65535, &[], &[]), 12);
    let r = drain_at(&mut st, id, 12);
    assert_eq!(r.len(), 1);
    assert_eq!((r[0].seq, r[0].payload.len()), (una, 1000), "reenvia o segmento perdido sem esperar o RTO");
}

#[test]
fn out_of_order_segments_are_reassembled() {
    let (mut st, id, peer) = connected(8192, 1460);
    let (r, _) = feed_at(&mut st, &peer.seg(ACK, PEER_ISS + 6, ISS.wrapping_add(1), 8192, &[], b"world"), 1);
    assert_eq!(r.unwrap().ack, PEER_ISS + 1, "o buraco ainda nao foi preenchido");
    let (r, _) = feed_at(&mut st, &peer.seg(ACK, PEER_ISS + 11, ISS.wrapping_add(1), 8192, &[], b"!"), 2);
    assert_eq!(r.unwrap().ack, PEER_ISS + 1);
    let (r, _) = feed_at(&mut st, &peer.seg(ACK, PEER_ISS + 1, ISS.wrapping_add(1), 8192, &[], b"hello"), 3);
    assert_eq!(r.unwrap().ack, PEER_ISS + 12, "o ACK pula o que estava guardado");
    let mut buf = [0u8; 32];
    let n = st.read(id, &mut buf).unwrap();
    assert_eq!(&buf[..n], b"helloworld!");
}

#[test]
fn zero_window_is_probed_until_it_opens() {
    let (mut st, id, peer) = connected(0, 1460);
    st.write(id, b"dados").unwrap();
    assert!(drain_at(&mut st, id, 0).is_empty());
    let rto = st.info(id).unwrap().rto_ms as u64;
    let (probe, _) = tick_at(&mut st, rto);
    assert_eq!(probe.len(), 1);
    assert_eq!((probe[0].seq, probe[0].payload.len()), (ISS, 0), "sonda com SEQ = SND.UNA - 1");
    let (later, _) = tick_at(&mut st, rto + 2 * rto);
    assert_eq!(later.len(), 1, "a sonda tambem tem backoff");
    feed_at(&mut st, &peer.seg(ACK, PEER_ISS + 1, ISS.wrapping_add(1), 500, &[], &[]), 5000);
    let segs = drain_at(&mut st, id, 5000);
    assert_eq!(segs.len(), 1);
    assert_eq!(segs[0].payload, b"dados");
}

#[test]
fn fin_is_retransmitted() {
    let (mut st, id, peer) = connected(8192, 1460);
    st.close(id).unwrap();
    let fin = drain_at(&mut st, id, 0);
    assert_eq!(fin.len(), 1);
    let rto = st.info(id).unwrap().rto_ms as u64;
    let (again, _) = tick_at(&mut st, rto);
    assert_eq!(again.len(), 1);
    assert_eq!((again[0].flags, again[0].seq), (FIN | ACK, fin[0].seq));
    feed_at(&mut st, &peer.seg(ACK, PEER_ISS + 1, ISS.wrapping_add(2), 8192, &[], &[]), rto + 1);
    assert_eq!(st.info(id).unwrap().state, State::FinWait2 as u32);
}

#[test]
fn data_gives_up_after_the_retries() {
    let (mut st, id, _) = connected(8192, 1460);
    st.write(id, b"ninguem ouve").unwrap();
    drain_at(&mut st, id, 0);
    let mut at = 0;
    let mut evs = StdVec::new();
    for _ in 0..40 {
        at += RTO_MAX;
        evs.extend(tick_at(&mut st, at).1);
    }
    assert!(evs.iter().any(|e| e.events & EV_ERROR != 0));
    assert_eq!(st.info(id).unwrap().error, ETIMEDOUT);
}

struct Wire(Rng, u64);

impl Wire {
    fn lose(&mut self) -> bool {
        self.0.next() % 100 < self.1
    }
}

#[test]
fn bulk_send_survives_loss() {
    for (seed, loss) in [(1u64, 0u64), (2, 5), (3, 15), (4, 30)] {
        let (mut st, id, peer) = connected(32768, 1000);
        let total = 200_000usize;
        let data: StdVec<u8> = (0..total).map(|i| (i * 31 + i / 997) as u8).collect();
        let mut wire = Wire(Rng(0xABCD_0000 + seed), loss);
        let mut written = 0;
        let mut got: StdVec<u8> = StdVec::new();
        let mut held: StdVec<(u32, StdVec<u8>)> = StdVec::new();
        let base = ISS.wrapping_add(1);
        let mut now = 0u64;
        while got.len() < total && now < 3_600_000 {
            if written < total {
                written += st.write(id, &data[written..]).unwrap();
            }
            let mut segs = drain_at(&mut st, id, now);
            segs.extend(tick_at(&mut st, now).0);
            for s in segs {
                if wire.lose() || s.payload.is_empty() {
                    continue;
                }
                held.push((s.seq, s.payload.to_vec()));
                loop {
                    let want = base.wrapping_add(got.len() as u32);
                    let Some(i) = held.iter().position(|(q, d)| le(*q, want) && lt(want, q.wrapping_add(d.len() as u32))) else { break };
                    let (q, d) = held.swap_remove(i);
                    got.extend_from_slice(&d[want.wrapping_sub(q) as usize..]);
                }
                let ack = base.wrapping_add(got.len() as u32);
                if !wire.lose() {
                    feed_at(&mut st, &peer.seg(ACK, PEER_ISS + 1, ack, 32768, &[], &[]), now);
                }
            }
            now += 10;
        }
        assert_eq!(got.len(), total, "perda de {}%: entregou {} de {}", loss, got.len(), total);
        assert_eq!(got, data, "perda de {}%: conteudo", loss);
        if loss > 0 {
            assert!(st.stats().retransmits > 0);
        }
    }
}

#[test]
fn bulk_receive_survives_loss_and_reordering() {
    for (seed, loss) in [(5u64, 0u64), (6, 10), (7, 25)] {
        let (mut st, id, peer) = connected(8192, 1460);
        let total = 150_000usize;
        let data: StdVec<u8> = (0..total).map(|i| (i * 13 + i / 101) as u8).collect();
        let mut wire = Wire(Rng(0x5EED_0000 + seed), loss);
        let mut acked = 0usize;
        let mut got: StdVec<u8> = StdVec::new();
        let mut now = 0u64;
        while got.len() < total && now < 2_000_000 {
            let window = 8 * 1000;
            let mut burst: StdVec<usize> = (0..)
                .map(|k| acked + k * 1000)
                .take_while(|&off| off < total && off < acked + window)
                .collect();
            if burst.len() > 1 && wire.0.next() % 2 == 0 {
                burst.swap(0, 1);
            }
            for off in burst {
                if wire.lose() {
                    continue;
                }
                let end = (off + 1000).min(total);
                let seq = PEER_ISS + 1 + off as u32;
                let (r, _) = feed_at(&mut st, &peer.seg(ACK, seq, ISS.wrapping_add(1), 8192, &[], &data[off..end]), now);
                if let Some(r) = r {
                    if !wire.lose() {
                        acked = acked.max(r.ack.wrapping_sub(PEER_ISS + 1) as usize);
                    }
                }
            }
            let mut buf = vec![0u8; 4096];
            loop {
                let n = st.read(id, &mut buf).unwrap();
                if n == 0 {
                    break;
                }
                got.extend_from_slice(&buf[..n]);
            }
            for s in drain_at(&mut st, id, now) {
                if !wire.lose() {
                    acked = acked.max(s.ack.wrapping_sub(PEER_ISS + 1) as usize);
                }
            }
            now += 50;
        }
        assert_eq!(got, data, "perda de {}%", loss);
    }
}
