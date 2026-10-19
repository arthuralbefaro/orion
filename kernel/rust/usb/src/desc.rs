use alloc::string::String;
use alloc::vec::Vec;

pub const DT_DEVICE: u8 = 1;
pub const DT_CONFIG: u8 = 2;
pub const DT_STRING: u8 = 3;
pub const DT_INTERFACE: u8 = 4;
pub const DT_ENDPOINT: u8 = 5;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct DeviceDesc {
    pub usb: u16,
    pub class: u8,
    pub subclass: u8,
    pub protocol: u8,
    pub max_packet0: u8,
    pub vendor: u16,
    pub product: u16,
    pub release: u16,
    pub i_manufacturer: u8,
    pub i_product: u8,
    pub i_serial: u8,
    pub configs: u8,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Interface {
    pub number: u8,
    pub alternate: u8,
    pub class: u8,
    pub subclass: u8,
    pub protocol: u8,
    pub endpoints: u8,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Endpoint {
    pub interface: u8,
    pub address: u8,
    pub attributes: u8,
    pub max_packet: u16,
    pub interval: u8,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Config {
    pub value: u8,
    pub total: u16,
    pub interfaces: Vec<Interface>,
    pub endpoints: Vec<Endpoint>,
}

fn le16(b: &[u8], at: usize) -> u16 {
    u16::from_le_bytes([b[at], b[at + 1]])
}

pub fn parse_device(b: &[u8]) -> Option<DeviceDesc> {
    if b.len() < 18 || (b[0] as usize) < 18 || b[1] != DT_DEVICE {
        return None;
    }
    Some(DeviceDesc {
        usb: le16(b, 2),
        class: b[4],
        subclass: b[5],
        protocol: b[6],
        max_packet0: b[7],
        vendor: le16(b, 8),
        product: le16(b, 10),
        release: le16(b, 12),
        i_manufacturer: b[14],
        i_product: b[15],
        i_serial: b[16],
        configs: b[17],
    })
}

pub fn config_total(b: &[u8]) -> Option<u16> {
    if b.len() < 9 || (b[0] as usize) < 9 || b[1] != DT_CONFIG {
        return None;
    }
    Some(le16(b, 2))
}

pub fn parse_config(b: &[u8]) -> Option<Config> {
    let total = config_total(b)?;
    let end = b.len().min(total as usize);
    let mut cfg = Config { value: b[5], total, interfaces: Vec::new(), endpoints: Vec::new() };
    let mut at = b[0] as usize;
    let mut current = 0u8;
    while at + 2 <= end {
        let len = b[at] as usize;
        if len < 2 || at + len > end {
            break;
        }
        let d = &b[at..at + len];
        match d[1] {
            DT_INTERFACE if len >= 9 => {
                current = d[2];
                cfg.interfaces.push(Interface {
                    number: d[2],
                    alternate: d[3],
                    endpoints: d[4],
                    class: d[5],
                    subclass: d[6],
                    protocol: d[7],
                });
            }
            DT_ENDPOINT if len >= 7 => {
                cfg.endpoints.push(Endpoint {
                    interface: current,
                    address: d[2],
                    attributes: d[3],
                    max_packet: le16(d, 4),
                    interval: d[6],
                });
            }
            _ => {}
        }
        at += len;
    }
    Some(cfg)
}

pub fn first_langid(b: &[u8]) -> Option<u16> {
    if b.len() < 4 || (b[0] as usize) < 4 || b[1] != DT_STRING {
        return None;
    }
    Some(le16(b, 2))
}

pub fn parse_string(b: &[u8]) -> Option<String> {
    if b.len() < 2 || b[1] != DT_STRING {
        return None;
    }
    let len = (b[0] as usize).min(b.len());
    let mut out = String::new();
    let mut at = 2;
    while at + 1 < len {
        let unit = le16(b, at);
        out.push(if (0x20..0x7F).contains(&unit) { unit as u8 as char } else { '?' });
        at += 2;
    }
    Some(String::from(out.trim_end()))
}

pub fn class_name(class: u8) -> &'static str {
    match class {
        0x00 => "por interface",
        0x01 => "audio",
        0x02 => "comunicacao",
        0x03 => "hid",
        0x06 => "imagem",
        0x07 => "impressora",
        0x08 => "armazenamento",
        0x09 => "hub",
        0x0A => "dados",
        0x0E => "video",
        0xE0 => "sem fio",
        0xEF => "diversos",
        0xFF => "do fabricante",
        _ => "outra",
    }
}

pub fn speed_name(speed: u8) -> &'static str {
    match speed {
        1 => "full speed",
        2 => "low speed",
        3 => "high speed",
        4 => "super speed",
        5 => "super speed plus",
        _ => "velocidade desconhecida",
    }
}

pub fn max_packet0_for(speed: u8) -> u16 {
    match speed {
        2 => 8,
        3 => 64,
        4 | 5 => 512,
        _ => 8,
    }
}
