use alloc::vec::Vec;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct KeyEvent {
    pub scancode: u8,
    pub extended: bool,
    pub pressed: bool,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct MouseReport {
    pub buttons: u8,
    pub dx: i32,
    pub dy: i32,
    pub dz: i32,
}

const MODIFIERS: [(u8, bool); 8] = [
    (0x1D, false),
    (0x2A, false),
    (0x38, false),
    (0x5B, true),
    (0x1D, true),
    (0x36, false),
    (0x38, true),
    (0x5C, true),
];

const LETTERS: [u8; 26] = [
    0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13,
    0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C,
];

pub fn usage_to_scancode(usage: u8) -> Option<(u8, bool)> {
    let plain = |sc: u8| Some((sc, false));
    let ext = |sc: u8| Some((sc, true));
    match usage {
        0x04..=0x1D => plain(LETTERS[(usage - 0x04) as usize]),
        0x1E..=0x27 => plain(usage - 0x1E + 0x02),
        0x28 => plain(0x1C),
        0x29 => plain(0x01),
        0x2A => plain(0x0E),
        0x2B => plain(0x0F),
        0x2C => plain(0x39),
        0x2D => plain(0x0C),
        0x2E => plain(0x0D),
        0x2F => plain(0x1A),
        0x30 => plain(0x1B),
        0x31 => plain(0x2B),
        0x33 => plain(0x27),
        0x34 => plain(0x28),
        0x35 => plain(0x29),
        0x36 => plain(0x33),
        0x37 => plain(0x34),
        0x38 => plain(0x35),
        0x39 => plain(0x3A),
        0x3A..=0x43 => plain(usage - 0x3A + 0x3B),
        0x44 => plain(0x57),
        0x45 => plain(0x58),
        0x49 => ext(0x52),
        0x4A => ext(0x47),
        0x4B => ext(0x49),
        0x4C => ext(0x53),
        0x4D => ext(0x4F),
        0x4E => ext(0x51),
        0x4F => ext(0x4D),
        0x50 => ext(0x4B),
        0x51 => ext(0x50),
        0x52 => ext(0x48),
        0x58 => ext(0x1C),
        _ => None,
    }
}

fn is_key(usage: u8) -> bool {
    usage > 3
}

pub fn keyboard_diff(prev: &[u8; 8], now: &[u8; 8], out: &mut Vec<KeyEvent>) {
    if now[2..].contains(&1) {
        return;
    }
    let (pm, nm) = (prev[0], now[0]);
    for (bit, &(scancode, extended)) in MODIFIERS.iter().enumerate() {
        if nm & (1 << bit) != 0 && pm & (1 << bit) == 0 {
            out.push(KeyEvent { scancode, extended, pressed: true });
        }
    }
    for &u in prev[2..].iter().filter(|&&u| is_key(u)) {
        if !now[2..].contains(&u) {
            if let Some((scancode, extended)) = usage_to_scancode(u) {
                out.push(KeyEvent { scancode, extended, pressed: false });
            }
        }
    }
    for &u in now[2..].iter().filter(|&&u| is_key(u)) {
        if !prev[2..].contains(&u) {
            if let Some((scancode, extended)) = usage_to_scancode(u) {
                out.push(KeyEvent { scancode, extended, pressed: true });
            }
        }
    }
    for (bit, &(scancode, extended)) in MODIFIERS.iter().enumerate() {
        if pm & (1 << bit) != 0 && nm & (1 << bit) == 0 {
            out.push(KeyEvent { scancode, extended, pressed: false });
        }
    }
}

pub fn parse_mouse(b: &[u8]) -> Option<MouseReport> {
    if b.len() < 3 {
        return None;
    }
    Some(MouseReport {
        buttons: b[0] & 0x07,
        dx: b[1] as i8 as i32,
        dy: b[2] as i8 as i32,
        dz: if b.len() > 3 { -(b[3] as i8 as i32) } else { 0 },
    })
}

pub fn interval_for(speed: u8, b_interval: u8) -> u32 {
    if speed >= 3 {
        return (b_interval.clamp(1, 16) - 1) as u32;
    }
    let frames = (b_interval.max(1) as u32) * 8;
    (31 - frames.leading_zeros()).clamp(3, 10)
}
