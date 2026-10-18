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
    let n = st.input(HOST, ME, &syn, &mut out, &mut TcpEvent::default()).expect("entrada");
    assert_eq!(n, HDR_LEN);
    let r = parse(ME, HOST, &out[..n]).expect("o RST tem checksum certo do nosso lado");
    assert_eq!((r.sport, r.dport, r.seq, r.ack, r.flags), (7, 40000, 0, 4243, RST | ACK));
    assert_eq!(st.input(HOST, ME, &segment(RST, 1, 0, &[]), &mut out, &mut TcpEvent::default()), Ok(0));
    let mut broken = syn.clone();
    broken[4] ^= 0x80;
    assert_eq!(st.input(HOST, ME, &broken, &mut out, &mut TcpEvent::default()), Err(EINVAL));
    assert_eq!(st.input(HOST, ME, &syn, &mut [0u8; 10], &mut TcpEvent::default()), Err(ENOBUFS));
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
        let _ = st.input(HOST, ME, &seg, &mut out, &mut TcpEvent::default());
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
    let n = st.input(HOST, ME, seg, &mut out, &mut ev).expect("entrada");
    out.truncate(n);
    let leaked: &'static [u8] = Box::leak(out.into_boxed_slice());
    let reply = if n == 0 { None } else { Some(parse(ME, HOST, leaked).expect("resposta valida")) };
    (reply, ev)
}

fn drain(st: &mut Stack, id: u32) -> StdVec<Segment<'static>> {
    let mut segs = StdVec::new();
    loop {
        let mut out = vec![0u8; 2048];
        let (n, ip) = st.output(id, &mut out).expect("saida");
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
    let (id, n) = st.connect(ME, HOST, 80, ISS, &mut out).expect("connect");
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
    assert_eq!(st.stats().connections, 0, "conexao terminada e solta sai da tabela");
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
    let (id2, n) = st2.connect(ME, HOST, 81, 77, &mut out).unwrap();
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
        let (id, n) = st.connect(ME, HOST, 80, i, &mut out).unwrap();
        ports.push(parse(ME, HOST, &out[..n]).unwrap().sport);
        assert_eq!(st.info(id).unwrap().local_port, *ports.last().unwrap());
    }
    ports.sort();
    ports.dedup();
    assert_eq!(ports.len(), 50);
    assert_eq!(st.stats().connections, 50);
    assert_eq!(st.connect(0, HOST, 80, 1, &mut out), Err(EINVAL));
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
            let _ = st.input(HOST, ME, &peer.seg(flags, base, ack, rng.next() as u16, &[], &payload), &mut [0u8; 2048], &mut TcpEvent::default());
            let _ = drain(&mut st, id);
            let _ = st.read(id, &mut [0u8; 700]);
            if round % 7 == 0 && i == 30 {
                let _ = st.close(id);
            }
        }
        st.detach(id);
    }
}
