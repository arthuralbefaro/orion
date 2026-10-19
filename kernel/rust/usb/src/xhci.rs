use crate::desc::{self, Config, DeviceDesc};
use crate::hid;
use alloc::boxed::Box;
use alloc::string::String;
use alloc::vec::Vec;
use core::fmt::Write;
use core::ptr::{read_volatile, write_volatile};
use core::sync::atomic::{fence, AtomicBool, AtomicPtr, Ordering};

extern "C" {
    fn usb_dma_alloc(pages: usize, phys: *mut u64) -> *mut u8;
    fn usb_now_us() -> u64;
    fn usb_sleep_ms(ms: u64);
    fn usb_log(msg: *const u8, len: usize);
    fn usb_key(scancode: u8, extended: bool, pressed: bool);
    fn usb_mouse(dx: i32, dy: i32, dz: i32, buttons: u8);
}

const RING_TRBS: usize = 256;
const HID_TRBS: usize = 8;
const HID_BUF: usize = 64;

const TRB_NORMAL: u32 = 1;
const TRB_SETUP: u32 = 2;
const TRB_DATA: u32 = 3;
const TRB_STATUS: u32 = 4;
const TRB_LINK: u32 = 6;
const TRB_ENABLE_SLOT: u32 = 9;
const TRB_ADDRESS_DEVICE: u32 = 11;
const TRB_CONFIGURE_EP: u32 = 12;
const TRB_TRANSFER_EVENT: u32 = 32;
const TRB_COMMAND_DONE: u32 = 33;

const CC_SUCCESS: u32 = 1;
const CC_SHORT_PACKET: u32 = 13;

const PORTSC_CCS: u32 = 1 << 0;
const PORTSC_PED: u32 = 1 << 1;
const PORTSC_PR: u32 = 1 << 4;
const PORTSC_PRC: u32 = 1 << 21;
const PORTSC_CHANGE: u32 = 0x7F << 17;

fn log(text: &str) {
    unsafe { usb_log(text.as_ptr(), text.len()) }
}

struct Dma {
    virt: *mut u8,
    phys: u64,
}

impl Dma {
    fn new(pages: usize) -> Option<Dma> {
        let mut phys = 0u64;
        let virt = unsafe { usb_dma_alloc(pages, &mut phys) };
        if virt.is_null() {
            None
        } else {
            Some(Dma { virt, phys })
        }
    }

    fn write32(&self, off: usize, v: u32) {
        unsafe { write_volatile(self.virt.add(off) as *mut u32, v) }
    }

    fn read32(&self, off: usize) -> u32 {
        unsafe { read_volatile(self.virt.add(off) as *const u32) }
    }

    fn write64(&self, off: usize, v: u64) {
        unsafe { write_volatile(self.virt.add(off) as *mut u64, v) }
    }

    fn bytes(&self, len: usize) -> &[u8] {
        unsafe { core::slice::from_raw_parts(self.virt, len) }
    }

    fn read64(&self, off: usize) -> u64 {
        unsafe { read_volatile(self.virt.add(off) as *const u64) }
    }

    fn clear(&self, len: usize) {
        unsafe { core::ptr::write_bytes(self.virt, 0, len) }
    }
}

struct Ring {
    mem: Dma,
    index: usize,
    cycle: u32,
}

impl Ring {
    fn new() -> Option<Ring> {
        let mem = Dma::new(1)?;
        let link = (RING_TRBS - 1) * 16;
        mem.write64(link, mem.phys);
        mem.write32(link + 8, 0);
        mem.write32(link + 12, (TRB_LINK << 10) | (1 << 1));
        Some(Ring { mem, index: 0, cycle: 1 })
    }

    fn push(&mut self, param: u64, status: u32, control: u32) -> u64 {
        let off = self.index * 16;
        let at = self.mem.phys + off as u64;
        self.mem.write64(off, param);
        self.mem.write32(off + 8, status);
        fence(Ordering::SeqCst);
        self.mem.write32(off + 12, control | self.cycle);
        self.index += 1;
        if self.index == RING_TRBS - 1 {
            let link = (RING_TRBS - 1) * 16;
            let ctl = self.mem.read32(link + 12) & !1;
            self.mem.write32(link + 12, ctl | self.cycle);
            self.cycle ^= 1;
            self.index = 0;
        }
        at
    }
}

#[derive(Clone, Copy)]
struct Event {
    param: u64,
    status: u32,
    control: u32,
}

impl Event {
    fn kind(&self) -> u32 {
        (self.control >> 10) & 0x3F
    }

    fn code(&self) -> u32 {
        self.status >> 24
    }

    fn slot(&self) -> u32 {
        self.control >> 24
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum HidKind {
    Keyboard,
    Mouse,
}

struct Hid {
    kind: HidKind,
    dci: u32,
    mps: u16,
    ring: Ring,
    bufs: Dma,
    prev: [u8; 8],
}

pub struct Device {
    pub port: u8,
    pub slot: u8,
    pub speed: u8,
    pub desc: DeviceDesc,
    pub config: Option<Config>,
    pub manufacturer: String,
    pub product: String,
    ep0: Ring,
    input: Dma,
    output: Dma,
    hid: Option<Hid>,
}

pub struct Xhci {
    op: *mut u8,
    rt: *mut u8,
    db: *mut u8,
    ctx_size: usize,
    max_ports: u32,
    version: u16,
    dcbaa: Dma,
    cmd: Ring,
    events: Dma,
    ev_index: usize,
    ev_cycle: u32,
    buffer: Dma,
    pub devices: Vec<Device>,
}

unsafe fn rd(base: *mut u8, off: usize) -> u32 {
    read_volatile(base.add(off) as *const u32)
}

unsafe fn wr(base: *mut u8, off: usize, v: u32) {
    write_volatile(base.add(off) as *mut u32, v)
}

unsafe fn wr64(base: *mut u8, off: usize, v: u64) {
    wr(base, off, v as u32);
    wr(base, off + 4, (v >> 32) as u32);
}

fn wait_until(timeout_ms: u64, mut done: impl FnMut() -> bool) -> bool {
    let end = unsafe { usb_now_us() } + timeout_ms * 1000;
    loop {
        if done() {
            return true;
        }
        if unsafe { usb_now_us() } > end {
            return false;
        }
        core::hint::spin_loop();
    }
}

impl Xhci {
    unsafe fn take_from_firmware(cap: *mut u8) {
        let hcc1 = rd(cap, 0x10);
        let mut off = ((hcc1 >> 16) as usize) << 2;
        while off != 0 {
            let head = rd(cap, off);
            if head & 0xFF == 1 {
                wr(cap, off, head | (1 << 24));
                if !wait_until(1000, || rd(cap, off) & (1 << 16) == 0) {
                    log("o firmware nao soltou o controlador; seguindo assim mesmo");
                }
                let ctl = rd(cap, off + 4);
                wr(cap, off + 4, ctl & !0x1F);
                return;
            }
            let next = ((head >> 8) & 0xFF) as usize;
            if next == 0 {
                return;
            }
            off += next << 2;
        }
    }

    unsafe fn start(mmio: *mut u8) -> Result<Box<Xhci>, &'static str> {
        let cap = mmio;
        let head = rd(cap, 0x00);
        let caplen = (head & 0xFF) as usize;
        let version = (head >> 16) as u16;
        let hcs1 = rd(cap, 0x04);
        let hcs2 = rd(cap, 0x08);
        let hcc1 = rd(cap, 0x10);
        let op = cap.add(caplen);
        let rt = cap.add((rd(cap, 0x18) & !0x1F) as usize);
        let db = cap.add((rd(cap, 0x14) & !0x3) as usize);
        let max_slots = (hcs1 & 0xFF).min(32);
        let max_ports = hcs1 >> 24;
        let ctx_size = if hcc1 & (1 << 2) != 0 { 64 } else { 32 };

        Self::take_from_firmware(cap);
        wr(op, 0x00, rd(op, 0x00) & !1);
        if !wait_until(1000, || rd(op, 0x04) & 1 != 0) {
            return Err("o controlador nao parou");
        }
        wr(op, 0x00, 1 << 1);
        if !wait_until(2000, || rd(op, 0x00) & (1 << 1) == 0 && rd(op, 0x04) & (1 << 11) == 0) {
            return Err("o reset do controlador nao terminou");
        }
        wr(op, 0x38, max_slots);

        let dcbaa = Dma::new(1).ok_or("sem memoria")?;
        let scratch = (((hcs2 >> 21) & 0x1F) << 5) | ((hcs2 >> 27) & 0x1F);
        if scratch > 0 {
            let table = Dma::new(1).ok_or("sem memoria")?;
            for i in 0..scratch as usize {
                let page = Dma::new(1).ok_or("sem memoria")?;
                table.write64(i * 8, page.phys);
            }
            dcbaa.write64(0, table.phys);
        }
        wr64(op, 0x30, dcbaa.phys);

        let cmd = Ring::new().ok_or("sem memoria")?;
        wr64(op, 0x18, cmd.mem.phys | 1);

        let events = Dma::new(1).ok_or("sem memoria")?;
        let erst = Dma::new(1).ok_or("sem memoria")?;
        erst.write64(0, events.phys);
        erst.write32(8, RING_TRBS as u32);
        let ir = rt.add(0x20);
        wr(ir, 0x08, 1);
        wr64(ir, 0x18, events.phys);
        wr64(ir, 0x10, erst.phys);

        wr(op, 0x00, rd(op, 0x00) | 1);
        if !wait_until(1000, || rd(op, 0x04) & 1 == 0) {
            return Err("o controlador nao ligou");
        }
        let buffer = Dma::new(1).ok_or("sem memoria")?;
        Ok(Box::new(Xhci {
            op,
            rt,
            db,
            ctx_size,
            max_ports,
            version,
            dcbaa,
            cmd,
            events,
            ev_index: 0,
            ev_cycle: 1,
            buffer,
            devices: Vec::new(),
        }))
    }

    fn next_event(&mut self) -> Option<Event> {
        let off = self.ev_index * 16;
        let control = self.events.read32(off + 12);
        if control & 1 != self.ev_cycle {
            return None;
        }
        fence(Ordering::SeqCst);
        let ev = Event {
            param: self.events.read32(off) as u64 | ((self.events.read32(off + 4) as u64) << 32),
            status: self.events.read32(off + 8),
            control,
        };
        self.ev_index += 1;
        if self.ev_index == RING_TRBS {
            self.ev_index = 0;
            self.ev_cycle ^= 1;
        }
        let dequeue = self.events.phys + (self.ev_index * 16) as u64;
        unsafe { wr64(self.rt.add(0x20), 0x18, dequeue | (1 << 3)) };
        Some(ev)
    }

    fn wait_event(&mut self, timeout_ms: u64, mut want: impl FnMut(&Event) -> bool) -> Option<Event> {
        let end = unsafe { usb_now_us() } + timeout_ms * 1000;
        loop {
            while let Some(ev) = self.next_event() {
                if want(&ev) {
                    return Some(ev);
                }
            }
            if unsafe { usb_now_us() } > end {
                return None;
            }
            core::hint::spin_loop();
        }
    }

    fn command(&mut self, param: u64, control: u32) -> Option<Event> {
        let at = self.cmd.push(param, 0, control);
        unsafe { wr(self.db, 0, 0) };
        self.wait_event(1000, |ev| ev.kind() == TRB_COMMAND_DONE && ev.param == at)
    }

    fn control_in(&mut self, dev: usize, setup: [u8; 8], len: u16) -> Option<usize> {
        let slot = self.devices[dev].slot;
        let buf = self.buffer.phys;
        let ring = &mut self.devices[dev].ep0;
        let trt = if len > 0 { 3 << 16 } else { 0 };
        ring.push(u64::from_le_bytes(setup), 8, (TRB_SETUP << 10) | (1 << 6) | trt);
        if len > 0 {
            ring.push(buf, len as u32, (TRB_DATA << 10) | (1 << 16));
        }
        ring.push(0, 0, (TRB_STATUS << 10) | (1 << 5));
        unsafe { wr(self.db, slot as usize * 4, 1) };
        let ev = self.wait_event(1000, |ev| ev.kind() == TRB_TRANSFER_EVENT && ev.slot() == slot as u32)?;
        if ev.code() != CC_SUCCESS && ev.code() != CC_SHORT_PACKET {
            return None;
        }
        Some(len as usize)
    }

    fn control_out(&mut self, dev: usize, setup: [u8; 8]) -> bool {
        let slot = self.devices[dev].slot;
        let ring = &mut self.devices[dev].ep0;
        ring.push(u64::from_le_bytes(setup), 8, (TRB_SETUP << 10) | (1 << 6));
        ring.push(0, 0, (TRB_STATUS << 10) | (1 << 16) | (1 << 5));
        unsafe { wr(self.db, slot as usize * 4, 1) };
        matches!(
            self.wait_event(1000, |ev| ev.kind() == TRB_TRANSFER_EVENT && ev.slot() == slot as u32),
            Some(ev) if ev.code() == CC_SUCCESS
        )
    }

    fn configure_interrupt_in(&mut self, dev: usize, dci: u32, mps: u16, interval: u32, ring: u64) -> bool {
        let c = self.ctx_size;
        let slot = self.devices[dev].slot as u32;
        let input = &self.devices[dev].input;
        let output = &self.devices[dev].output;
        input.clear(4096);
        input.write32(4, 1 | (1 << dci));
        for off in (0..c).step_by(4) {
            input.write32(c + off, output.read32(off));
        }
        let slot0 = input.read32(c);
        input.write32(c, (slot0 & !(0x1F << 27)) | (dci << 27));
        let ep = (dci as usize + 1) * c;
        input.write32(ep, interval << 16);
        input.write32(ep + 4, (3 << 1) | (7 << 3) | ((mps as u32) << 16));
        input.write64(ep + 8, ring | 1);
        input.write32(ep + 16, mps as u32 | ((mps as u32) << 16));
        let phys = input.phys;
        matches!(self.command(phys, (TRB_CONFIGURE_EP << 10) | (slot << 24)), Some(ev) if ev.code() == CC_SUCCESS)
    }

    fn start_hid(&mut self, dev: usize) -> Result<Option<HidKind>, &'static str> {
        let Some(cfg) = self.devices[dev].config.clone() else { return Ok(None) };
        let Some(iface) = cfg
            .interfaces
            .iter()
            .find(|i| i.class == 3 && i.subclass == 1 && (i.protocol == 1 || i.protocol == 2))
            .copied()
        else {
            return Ok(None);
        };
        let Some(ep) = cfg
            .endpoints
            .iter()
            .find(|e| e.interface == iface.number && e.address & 0x80 != 0 && e.attributes & 3 == 3)
            .copied()
        else {
            return Ok(None);
        };
        let kind = if iface.protocol == 1 { HidKind::Keyboard } else { HidKind::Mouse };
        let mps = ep.max_packet.clamp(1, HID_BUF as u16);
        let dci = ((ep.address & 0x0F) as u32) * 2 + 1;
        let ring = Ring::new().ok_or("sem memoria")?;
        let bufs = Dma::new(1).ok_or("sem memoria")?;
        if !self.control_out(dev, [0x00, 9, cfg.value, 0, 0, 0, 0, 0]) {
            return Err("Set Configuration recusado");
        }
        let interval = hid::interval_for(self.devices[dev].speed, ep.interval);
        if !self.configure_interrupt_in(dev, dci, mps, interval, ring.mem.phys) {
            return Err("Configure Endpoint recusado");
        }
        self.control_out(dev, [0x21, 0x0B, 0, 0, iface.number, 0, 0, 0]);
        self.control_out(dev, [0x21, 0x0A, 0, 0, iface.number, 0, 0, 0]);
        let slot = self.devices[dev].slot as usize;
        let mut hid = Hid { kind, dci, mps, ring, bufs, prev: [0; 8] };
        for i in 0..HID_TRBS {
            let buf = hid.bufs.phys + (i * HID_BUF) as u64;
            hid.ring.push(buf, mps as u32, (TRB_NORMAL << 10) | (1 << 5) | (1 << 2));
        }
        self.devices[dev].hid = Some(hid);
        unsafe { wr(self.db, slot * 4, dci) };
        Ok(Some(kind))
    }

    fn handle_transfer(&mut self, ev: &Event) {
        let slot = ev.slot() as u8;
        let dci = (ev.control >> 16) & 0x1F;
        let db = self.db;
        let Some(d) = self.devices.iter_mut().find(|d| d.slot == slot) else { return };
        let Some(h) = d.hid.as_mut() else { return };
        if h.dci != dci || ev.param < h.ring.mem.phys || ev.param >= h.ring.mem.phys + 4096 {
            return;
        }
        let buf = h.ring.mem.read64((ev.param - h.ring.mem.phys) as usize);
        if buf < h.bufs.phys || buf >= h.bufs.phys + (HID_TRBS * HID_BUF) as u64 {
            return;
        }
        let off = (buf - h.bufs.phys) as usize;
        if ev.code() == CC_SUCCESS || ev.code() == CC_SHORT_PACKET {
            let residual = (ev.status & 0x00FF_FFFF) as usize;
            let len = (h.mps as usize).saturating_sub(residual);
            let data = &h.bufs.bytes(off + len)[off..];
            match h.kind {
                HidKind::Keyboard => {
                    let mut now = [0u8; 8];
                    let n = data.len().min(8);
                    now[..n].copy_from_slice(&data[..n]);
                    let mut events = Vec::new();
                    hid::keyboard_diff(&h.prev, &now, &mut events);
                    h.prev = now;
                    for k in events {
                        unsafe { usb_key(k.scancode, k.extended, k.pressed) };
                    }
                }
                HidKind::Mouse => {
                    if let Some(m) = hid::parse_mouse(data) {
                        unsafe { usb_mouse(m.dx, m.dy, m.dz, m.buttons) };
                    }
                }
            }
        }
        h.ring.push(buf, h.mps as u32, (TRB_NORMAL << 10) | (1 << 5) | (1 << 2));
        unsafe { wr(db, slot as usize * 4, dci) };
    }

    fn poll(&mut self) {
        while let Some(ev) = self.next_event() {
            if ev.kind() == TRB_TRANSFER_EVENT {
                self.handle_transfer(&ev);
            }
        }
    }

    fn get_descriptor(&mut self, dev: usize, kind: u8, index: u8, lang: u16, len: u16) -> Option<Vec<u8>> {
        let value = ((kind as u16) << 8) | index as u16;
        let setup = [0x80, 6, value as u8, (value >> 8) as u8, lang as u8, (lang >> 8) as u8, len as u8, (len >> 8) as u8];
        unsafe { core::ptr::write_bytes(self.buffer.virt, 0, len as usize) };
        let got = self.control_in(dev, setup, len)?;
        Some(Vec::from(self.buffer.bytes(got)))
    }

    fn read_string(&mut self, dev: usize, index: u8, lang: u16) -> String {
        if index == 0 {
            return String::new();
        }
        self.get_descriptor(dev, desc::DT_STRING, index, lang, 255)
            .and_then(|b| desc::parse_string(&b))
            .unwrap_or_default()
    }

    fn reset_port(&mut self, port: u32) -> Option<u32> {
        let reg = 0x400 + (port as usize - 1) * 0x10;
        let sc = unsafe { rd(self.op, reg) };
        if sc & PORTSC_CCS == 0 {
            return None;
        }
        if sc & PORTSC_PED == 0 {
            unsafe { wr(self.op, reg, (sc & !(PORTSC_PED | PORTSC_CHANGE)) | PORTSC_PR) };
            let op = self.op;
            if !wait_until(500, || unsafe { rd(op, reg) } & PORTSC_PRC != 0) {
                return None;
            }
            unsafe { usb_sleep_ms(20) };
        }
        let sc = unsafe { rd(self.op, reg) };
        unsafe { wr(self.op, reg, (sc & !PORTSC_PED & !PORTSC_CHANGE) | (sc & PORTSC_CHANGE)) };
        if sc & PORTSC_PED == 0 {
            return None;
        }
        Some((sc >> 10) & 0xF)
    }

    fn attach(&mut self, port: u32, speed: u32) -> Result<(), &'static str> {
        let done = self.command(0, TRB_ENABLE_SLOT << 10).ok_or("Enable Slot sem resposta")?;
        if done.code() != CC_SUCCESS {
            return Err("Enable Slot recusado");
        }
        let slot = done.slot() as u8;
        let input = Dma::new(1).ok_or("sem memoria")?;
        let output = Dma::new(1).ok_or("sem memoria")?;
        let ep0 = Ring::new().ok_or("sem memoria")?;
        self.dcbaa.write64(slot as usize * 8, output.phys);
        let c = self.ctx_size;
        input.write32(4, 0b11);
        input.write32(c, (speed << 20) | (1 << 27));
        input.write32(c + 4, port << 16);
        let mps = desc::max_packet0_for(speed as u8) as u32;
        input.write32(2 * c + 4, (3 << 1) | (4 << 3) | (mps << 16));
        input.write64(2 * c + 8, ep0.mem.phys | 1);
        input.write32(2 * c + 16, 8);
        let addressed = self
            .command(input.phys, (TRB_ADDRESS_DEVICE << 10) | ((slot as u32) << 24))
            .ok_or("Address Device sem resposta")?;
        if addressed.code() != CC_SUCCESS {
            return Err("Address Device recusado");
        }
        self.devices.push(Device {
            port: port as u8,
            slot,
            speed: speed as u8,
            desc: DeviceDesc::default(),
            config: None,
            manufacturer: String::new(),
            product: String::new(),
            ep0,
            input,
            output,
            hid: None,
        });
        let dev = self.devices.len() - 1;
        let raw = self.get_descriptor(dev, desc::DT_DEVICE, 0, 0, 18).ok_or("descritor do dispositivo nao veio")?;
        let d = desc::parse_device(&raw).ok_or("descritor do dispositivo invalido")?;
        self.devices[dev].desc = d;
        if let Some(head) = self.get_descriptor(dev, desc::DT_CONFIG, 0, 0, 9) {
            if let Some(total) = desc::config_total(&head) {
                let total = total.min(4096);
                if let Some(full) = self.get_descriptor(dev, desc::DT_CONFIG, 0, 0, total) {
                    self.devices[dev].config = desc::parse_config(&full);
                }
            }
        }
        let lang = self
            .get_descriptor(dev, desc::DT_STRING, 0, 0, 4)
            .and_then(|b| desc::first_langid(&b))
            .unwrap_or(0x0409);
        self.devices[dev].manufacturer = self.read_string(dev, d.i_manufacturer, lang);
        self.devices[dev].product = self.read_string(dev, d.i_product, lang);
        match self.start_hid(dev) {
            Ok(Some(kind)) => {
                let mut msg = String::new();
                let _ = write!(
                    msg,
                    "porta {}: {} HID pronto, protocolo de boot",
                    port,
                    if kind == HidKind::Keyboard { "teclado" } else { "mouse" }
                );
                log(&msg);
            }
            Ok(None) => {}
            Err(why) => {
                let mut msg = String::new();
                let _ = write!(msg, "porta {}: HID nao iniciou: {}", port, why);
                log(&msg);
            }
        }
        Ok(())
    }

    fn enumerate(&mut self) {
        for port in 1..=self.max_ports {
            let Some(speed) = self.reset_port(port) else { continue };
            if let Err(why) = self.attach(port, speed) {
                let mut msg = String::new();
                let _ = write!(msg, "porta {}: {}", port, why);
                log(&msg);
            }
        }
    }

    fn device_class(d: &Device) -> u8 {
        if d.desc.class != 0 {
            return d.desc.class;
        }
        d.config.as_ref().and_then(|c| c.interfaces.first()).map(|i| i.class).unwrap_or(0)
    }

    pub fn report(&self, out: &mut String) {
        for d in &self.devices {
            let _ = writeln!(
                out,
                "Porta {} Slot {}: ID {:04x}:{:04x} {} {} (USB {:x}.{:02x}, {}, classe {})",
                d.port,
                d.slot,
                d.desc.vendor,
                d.desc.product,
                if d.manufacturer.is_empty() { "?" } else { &d.manufacturer },
                if d.product.is_empty() { "?" } else { &d.product },
                d.desc.usb >> 8,
                d.desc.usb & 0xFF,
                desc::speed_name(d.speed),
                desc::class_name(Self::device_class(d))
            );
        }
    }
}

static CONTROLLER: AtomicPtr<Xhci> = AtomicPtr::new(core::ptr::null_mut());
static BUSY: AtomicBool = AtomicBool::new(false);

fn with_controller<R>(f: impl FnOnce(Option<&mut Xhci>) -> R) -> R {
    while BUSY.compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed).is_err() {
        core::hint::spin_loop();
    }
    let hc = CONTROLLER.load(Ordering::Acquire);
    let r = f(unsafe { hc.as_mut() });
    BUSY.store(false, Ordering::Release);
    r
}

#[no_mangle]
pub unsafe extern "C" fn xhci_start(mmio: *mut u8) -> i32 {
    if !CONTROLLER.load(Ordering::Acquire).is_null() {
        return -1;
    }
    match Xhci::start(mmio) {
        Ok(mut hc) => {
            let mut msg = String::new();
            let _ = write!(msg, "xHCI {:x}.{:02x}, {} porta(s)", hc.version >> 8, hc.version & 0xFF, hc.max_ports);
            log(&msg);
            hc.enumerate();
            let n = hc.devices.len() as i32;
            for d in &hc.devices {
                let mut line = String::new();
                let _ = write!(
                    line,
                    "porta {}: {:04x}:{:04x} {} ({})",
                    d.port,
                    d.desc.vendor,
                    d.desc.product,
                    d.product,
                    desc::speed_name(d.speed)
                );
                log(&line);
            }
            CONTROLLER.store(Box::into_raw(hc), Ordering::Release);
            n
        }
        Err(why) => {
            log(why);
            -1
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn xhci_report(buf: *mut u8, cap: usize) -> usize {
    let mut text = String::new();
    with_controller(|hc| match hc {
        None => text.push_str("sem controlador USB\n"),
        Some(hc) => {
            hc.report(&mut text);
            if hc.devices.is_empty() {
                text.push_str("nenhum dispositivo USB\n");
            }
        }
    });
    let n = text.len().min(cap);
    core::ptr::copy_nonoverlapping(text.as_ptr(), buf, n);
    n
}

#[no_mangle]
pub extern "C" fn xhci_poll() {
    with_controller(|hc| {
        if let Some(hc) = hc {
            hc.poll();
        }
    });
}

#[no_mangle]
pub extern "C" fn xhci_hid_count() -> i32 {
    with_controller(|hc| hc.map(|hc| hc.devices.iter().filter(|d| d.hid.is_some()).count() as i32).unwrap_or(0))
}
