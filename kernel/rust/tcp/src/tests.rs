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
    let n = st.input(HOST, ME, &syn, &mut out).expect("entrada");
    assert_eq!(n, HDR_LEN);
    let r = parse(ME, HOST, &out[..n]).expect("o RST tem checksum certo do nosso lado");
    assert_eq!((r.sport, r.dport, r.seq, r.ack, r.flags), (7, 40000, 0, 4243, RST | ACK));
    assert_eq!(st.input(HOST, ME, &segment(RST, 1, 0, &[]), &mut out), Ok(0));
    let mut broken = syn.clone();
    broken[4] ^= 0x80;
    assert_eq!(st.input(HOST, ME, &broken, &mut out), Err(EINVAL));
    assert_eq!(st.input(HOST, ME, &syn, &mut [0u8; 10]), Err(ENOBUFS));
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
        let _ = st.input(HOST, ME, &seg, &mut out);
    }
    assert!(st.stats().tx_resets > 0);
}
