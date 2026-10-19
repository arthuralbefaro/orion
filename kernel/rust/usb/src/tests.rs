use crate::desc::*;

const KEYBOARD_DEVICE: [u8; 18] = [
    0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x08, 0x27, 0x06, 0x01, 0x00, 0x00, 0x00, 0x01, 0x04, 0x0b,
    0x01,
];

const KEYBOARD_CONFIG: [u8; 34] = [
    0x09, 0x02, 0x22, 0x00, 0x01, 0x01, 0x08, 0xa0, 0x32, 0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x01, 0x01,
    0x00, 0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x3f, 0x00, 0x07, 0x05, 0x81, 0x03, 0x08, 0x00, 0x07,
];

#[test]
fn device_descriptor_of_the_qemu_keyboard() {
    let d = parse_device(&KEYBOARD_DEVICE).unwrap();
    assert_eq!(d.usb, 0x0200);
    assert_eq!(d.vendor, 0x0627);
    assert_eq!(d.product, 0x0001);
    assert_eq!(d.max_packet0, 8);
    assert_eq!(d.i_product, 4);
    assert_eq!(d.configs, 1);
}

#[test]
fn device_descriptor_rejects_short_or_wrong_type() {
    assert!(parse_device(&KEYBOARD_DEVICE[..17]).is_none());
    let mut wrong = KEYBOARD_DEVICE;
    wrong[1] = DT_CONFIG;
    assert!(parse_device(&wrong).is_none());
    let mut short_len = KEYBOARD_DEVICE;
    short_len[0] = 8;
    assert!(parse_device(&short_len).is_none());
}

#[test]
fn config_of_the_qemu_keyboard() {
    assert_eq!(config_total(&KEYBOARD_CONFIG), Some(34));
    let c = parse_config(&KEYBOARD_CONFIG).unwrap();
    assert_eq!(c.value, 1);
    assert_eq!(c.interfaces.len(), 1);
    assert_eq!(c.interfaces[0].class, 3);
    assert_eq!(c.interfaces[0].protocol, 1);
    assert_eq!(c.endpoints.len(), 1);
    assert_eq!(c.endpoints[0].address, 0x81);
    assert_eq!(c.endpoints[0].attributes & 3, 3);
    assert_eq!(c.endpoints[0].max_packet, 8);
    assert_eq!(c.endpoints[0].interval, 7);
}

#[test]
fn config_stops_at_zero_length_without_looping() {
    let mut b = KEYBOARD_CONFIG;
    b[9] = 0;
    let c = parse_config(&b).unwrap();
    assert!(c.interfaces.is_empty());
    assert!(c.endpoints.is_empty());
}

#[test]
fn config_ignores_descriptor_past_the_end() {
    let mut b = KEYBOARD_CONFIG;
    b[27] = 0x40;
    let c = parse_config(&b).unwrap();
    assert_eq!(c.interfaces.len(), 1);
    assert!(c.endpoints.is_empty());
}

#[test]
fn config_trusts_the_shorter_of_total_and_buffer() {
    let c = parse_config(&KEYBOARD_CONFIG[..20]).unwrap();
    assert_eq!(c.interfaces.len(), 1);
    assert!(c.endpoints.is_empty());
    let mut big = KEYBOARD_CONFIG;
    big[2] = 0xff;
    big[3] = 0xff;
    assert_eq!(parse_config(&big).unwrap().endpoints.len(), 1);
}

#[test]
fn every_truncation_and_byte_flip_is_safe() {
    for cut in 0..KEYBOARD_CONFIG.len() {
        let _ = parse_config(&KEYBOARD_CONFIG[..cut]);
        let _ = parse_device(&KEYBOARD_DEVICE[..cut.min(KEYBOARD_DEVICE.len())]);
    }
    for i in 0..KEYBOARD_CONFIG.len() {
        for v in [0u8, 1, 2, 9, 0x7f, 0xff] {
            let mut b = KEYBOARD_CONFIG;
            b[i] = v;
            let _ = parse_config(&b);
        }
    }
}

#[test]
fn strings_and_langid() {
    let lang = [0x04, 0x03, 0x09, 0x04];
    assert_eq!(first_langid(&lang), Some(0x0409));
    assert!(first_langid(&lang[..3]).is_none());
    let s = [0x0e, 0x03, b'Q', 0, b'E', 0, b'M', 0, b'U', 0, 0xe9, 0x00, b' ', 0];
    assert_eq!(parse_string(&s).unwrap(), "QEMU?");
    let lying = [0xff, 0x03, b'A', 0];
    assert_eq!(parse_string(&lying).unwrap(), "A");
    assert!(parse_string(&[0x02, 0x01]).is_none());
}

#[test]
fn names() {
    assert_eq!(class_name(3), "hid");
    assert_eq!(class_name(8), "armazenamento");
    assert_eq!(speed_name(3), "high speed");
    assert_eq!(max_packet0_for(4), 512);
    assert_eq!(max_packet0_for(1), 8);
}

use crate::hid::*;

fn diff(prev: [u8; 8], now: [u8; 8]) -> Vec<KeyEvent> {
    let mut v = Vec::new();
    keyboard_diff(&prev, &now, &mut v);
    v
}

fn ev(scancode: u8, extended: bool, pressed: bool) -> KeyEvent {
    KeyEvent { scancode, extended, pressed }
}

#[test]
fn usages_map_to_set1_scancodes() {
    assert_eq!(usage_to_scancode(0x04), Some((0x1E, false)));
    assert_eq!(usage_to_scancode(0x1D), Some((0x2C, false)));
    assert_eq!(usage_to_scancode(0x1E), Some((0x02, false)));
    assert_eq!(usage_to_scancode(0x27), Some((0x0B, false)));
    assert_eq!(usage_to_scancode(0x28), Some((0x1C, false)));
    assert_eq!(usage_to_scancode(0x3A), Some((0x3B, false)));
    assert_eq!(usage_to_scancode(0x43), Some((0x44, false)));
    assert_eq!(usage_to_scancode(0x52), Some((0x48, true)));
    assert_eq!(usage_to_scancode(0x4C), Some((0x53, true)));
    assert_eq!(usage_to_scancode(0xE8), None);
}

#[test]
fn press_and_release_one_key() {
    let a = [0, 0, 0x04, 0, 0, 0, 0, 0];
    assert_eq!(diff([0; 8], a), vec![ev(0x1E, false, true)]);
    assert_eq!(diff(a, [0; 8]), vec![ev(0x1E, false, false)]);
    assert!(diff(a, a).is_empty());
}

#[test]
fn ctrl_c_presses_ctrl_before_the_letter_and_releases_it_after() {
    let ctrl_c = [0x01, 0, 0x06, 0, 0, 0, 0, 0];
    assert_eq!(diff([0; 8], ctrl_c), vec![ev(0x1D, false, true), ev(0x2E, false, true)]);
    assert_eq!(diff(ctrl_c, [0; 8]), vec![ev(0x2E, false, false), ev(0x1D, false, false)]);
}

#[test]
fn right_modifiers_and_arrows_are_extended() {
    let r = [0x50, 0, 0x52, 0, 0, 0, 0, 0];
    assert_eq!(diff([0; 8], r), vec![ev(0x1D, true, true), ev(0x38, true, true), ev(0x48, true, true)]);
}

#[test]
fn rollover_error_report_is_ignored() {
    assert!(diff([0, 0, 0x04, 0, 0, 0, 0, 0], [0, 0, 1, 1, 1, 1, 1, 1]).is_empty());
}

#[test]
fn second_key_keeps_the_first_down() {
    let a = [0, 0, 0x04, 0, 0, 0, 0, 0];
    let ab = [0, 0, 0x04, 0x05, 0, 0, 0, 0];
    assert_eq!(diff(a, ab), vec![ev(0x30, false, true)]);
}

#[test]
fn mouse_reports() {
    assert_eq!(parse_mouse(&[0x01, 10, 0xFB]), Some(MouseReport { buttons: 1, dx: 10, dy: -5, dz: 0 }));
    assert_eq!(parse_mouse(&[0xFF, 0x80, 0x7F, 0x01]), Some(MouseReport { buttons: 7, dx: -128, dy: 127, dz: -1 }));
    assert!(parse_mouse(&[0x01, 2]).is_none());
}

#[test]
fn polling_interval_in_xhci_units() {
    assert_eq!(interval_for(1, 10), 6);
    assert_eq!(interval_for(1, 1), 3);
    assert_eq!(interval_for(2, 255), 10);
    assert_eq!(interval_for(3, 4), 3);
    assert_eq!(interval_for(3, 0), 0);
    assert_eq!(interval_for(4, 40), 15);
}