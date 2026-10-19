pub const CBW_LEN: usize = 31;
pub const CSW_LEN: usize = 13;
const CBW_SIGNATURE: u32 = 0x4342_5355;
const CSW_SIGNATURE: u32 = 0x5342_5355;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CswError {
    Short,
    Signature,
    Tag,
    Failed(u8),
}

pub fn cbw(tag: u32, data_len: u32, data_in: bool, cdb: &[u8]) -> [u8; CBW_LEN] {
    let mut b = [0u8; CBW_LEN];
    b[0..4].copy_from_slice(&CBW_SIGNATURE.to_le_bytes());
    b[4..8].copy_from_slice(&tag.to_le_bytes());
    b[8..12].copy_from_slice(&data_len.to_le_bytes());
    b[12] = if data_in { 0x80 } else { 0 };
    let n = cdb.len().min(16);
    b[14] = n as u8;
    b[15..15 + n].copy_from_slice(&cdb[..n]);
    b
}

pub fn check_csw(b: &[u8], tag: u32) -> Result<u32, CswError> {
    if b.len() < CSW_LEN {
        return Err(CswError::Short);
    }
    if u32::from_le_bytes([b[0], b[1], b[2], b[3]]) != CSW_SIGNATURE {
        return Err(CswError::Signature);
    }
    if u32::from_le_bytes([b[4], b[5], b[6], b[7]]) != tag {
        return Err(CswError::Tag);
    }
    if b[12] != 0 {
        return Err(CswError::Failed(b[12]));
    }
    Ok(u32::from_le_bytes([b[8], b[9], b[10], b[11]]))
}

pub fn test_unit_ready() -> [u8; 6] {
    [0x00, 0, 0, 0, 0, 0]
}

pub fn inquiry(len: u8) -> [u8; 6] {
    [0x12, 0, 0, 0, len, 0]
}

pub fn read_capacity10() -> [u8; 10] {
    [0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0]
}

fn rw10(op: u8, lba: u32, blocks: u16) -> [u8; 10] {
    let l = lba.to_be_bytes();
    let n = blocks.to_be_bytes();
    [op, 0, l[0], l[1], l[2], l[3], 0, n[0], n[1], 0]
}

pub fn read10(lba: u32, blocks: u16) -> [u8; 10] {
    rw10(0x28, lba, blocks)
}

pub fn write10(lba: u32, blocks: u16) -> [u8; 10] {
    rw10(0x2A, lba, blocks)
}

pub fn parse_capacity(b: &[u8]) -> Option<(u64, u32)> {
    if b.len() < 8 {
        return None;
    }
    let last = u32::from_be_bytes([b[0], b[1], b[2], b[3]]) as u64;
    let size = u32::from_be_bytes([b[4], b[5], b[6], b[7]]);
    if size == 0 {
        return None;
    }
    Some((last + 1, size))
}

pub fn inquiry_name(b: &[u8]) -> Option<(alloc::string::String, alloc::string::String)> {
    if b.len() < 32 {
        return None;
    }
    let text = |r: &[u8]| -> alloc::string::String {
        let s: alloc::string::String =
            r.iter().map(|&c| if (0x20..0x7F).contains(&c) { c as char } else { '?' }).collect();
        alloc::string::String::from(s.trim())
    };
    Some((text(&b[8..16]), text(&b[16..32])))
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Sense {
    pub key: u8,
    pub asc: u8,
    pub ascq: u8,
}

pub fn request_sense(len: u8) -> [u8; 6] {
    [0x03, 0, 0, 0, len, 0]
}

pub fn parse_sense(b: &[u8]) -> Option<Sense> {
    match b.first().map(|c| c & 0x7F) {
        Some(0x70) | Some(0x71) if b.len() >= 14 => Some(Sense { key: b[2] & 0x0F, asc: b[12], ascq: b[13] }),
        Some(0x72) | Some(0x73) if b.len() >= 4 => Some(Sense { key: b[1] & 0x0F, asc: b[2], ascq: b[3] }),
        _ => None,
    }
}
