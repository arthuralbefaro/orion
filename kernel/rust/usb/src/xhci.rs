use crate::desc::{self, Config, DeviceDesc};
use crate::hid;
use crate::msc;
use alloc::boxed::Box;
use alloc::string::String;
use alloc::vec::Vec;
use core::fmt::Write;
use core::ptr::{read_volatile, write_volatile};
use core::sync::atomic::{fence, AtomicBool, AtomicPtr, Ordering};

extern "C" {
    fn usb_dma_alloc(pages: usize, phys: *mut u64) -> *mut u8;
    fn usb_dma_free(phys: u64, pages: usize);
    fn usb_now_us() -> u64;
    fn usb_sleep_ms(ms: u64);
    fn usb_log(msg: *const u8, len: usize);
    fn usb_key(scancode: u8, extended: bool, pressed: bool);
    fn usb_mouse(dx: i32, dy: i32, dz: i32, buttons: u8);
    fn usb_disk_add(index: u32, blocks: u64, name: *const u8, len: usize);
    fn usb_disk_gone(index: u32);
    fn usb_irq_save() -> u64;
    fn usb_irq_restore(flags: u64);
    fn usb_wait_ms(ms: u64);
    fn usb_wake();
    fn usb_yield();
    fn usb_test_allowed() -> bool;
}

const RING_TRBS: usize = 256;
const HID_TRBS: usize = 8;
const HID_BUF: usize = 64;
const MSC_PAGES: usize = 16;
const MSC_BLOCKS: u32 = (MSC_PAGES * 4096 / 512) as u32;
const WAITERS: usize = 16;
const COMMAND_MS: u64 = 2000;
const CONTROL_MS: u64 = 2000;
const BULK_MS: u64 = 5000;
const EPERM: i32 = 1;
const EIO: i32 = 5;
const ENODEV: i32 = 19;

const TRB_NORMAL: u32 = 1;
const TRB_SETUP: u32 = 2;
const TRB_DATA: u32 = 3;
const TRB_STATUS: u32 = 4;
const TRB_LINK: u32 = 6;
const TRB_ENABLE_SLOT: u32 = 9;
const TRB_DISABLE_SLOT: u32 = 10;
const TRB_ADDRESS_DEVICE: u32 = 11;
const TRB_CONFIGURE_EP: u32 = 12;
const TRB_EVALUATE_CTX: u32 = 13;
const TRB_RESET_EP: u32 = 14;
const TRB_STOP_EP: u32 = 15;
const TRB_SET_DEQUEUE: u32 = 16;
const TRB_TRANSFER_EVENT: u32 = 32;
const TRB_COMMAND_DONE: u32 = 33;
const TRB_PORT_CHANGE: u32 = 34;

const CC_SUCCESS: u32 = 1;
const CC_STALL: u32 = 6;
const CC_SHORT_PACKET: u32 = 13;
const CC_GONE: u32 = 0x100;

const EP_HALTED: u32 = 2;

const PORTSC_CCS: u32 = 1 << 0;
const PORTSC_PED: u32 = 1 << 1;
const PORTSC_PR: u32 = 1 << 4;
const PORTSC_CSC: u32 = 1 << 17;
const PORTSC_PRC: u32 = 1 << 21;
const PORTSC_CHANGE: u32 = 0x7F << 17;

fn log(text: &str) {
    unsafe { usb_log(text.as_ptr(), text.len()) }
}

fn now_us() -> u64 {
    unsafe { usb_now_us() }
}

struct Dma {
    virt: *mut u8,
    phys: u64,
    pages: usize,
}

impl Dma {
    fn new(pages: usize) -> Option<Dma> {
        let mut phys = 0u64;
        let virt = unsafe { usb_dma_alloc(pages, &mut phys) };
        if virt.is_null() {
            None
        } else {
            Some(Dma { virt, phys, pages })
        }
    }

    fn free(&self) {
        unsafe { usb_dma_free(self.phys, self.pages) }
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

    fn read64(&self, off: usize) -> u64 {
        unsafe { read_volatile(self.virt.add(off) as *const u64) }
    }

    fn bytes(&self, len: usize) -> &[u8] {
        unsafe { core::slice::from_raw_parts(self.virt, len) }
    }

    fn clear(&self, len: usize) {
        unsafe { core::ptr::write_bytes(self.virt, 0, len) }
    }

    fn put(&self, off: usize, data: &[u8]) {
        unsafe { core::ptr::copy_nonoverlapping(data.as_ptr(), self.virt.add(off), data.len()) }
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

    fn dequeue(&self) -> u64 {
        (self.mem.phys + (self.index * 16) as u64) | self.cycle as u64
    }

    fn ahead(&self, n: usize) -> u64 {
        let mut i = self.index;
        for _ in 0..n {
            i += 1;
            if i == RING_TRBS - 1 {
                i = 0;
            }
        }
        self.mem.phys + (i * 16) as u64
    }

    fn contains(&self, addr: u64) -> bool {
        addr >= self.mem.phys && addr < self.mem.phys + 4096
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

    fn dci(&self) -> u32 {
        (self.control >> 16) & 0x1F
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum WaitState {
    Free,
    Pending,
    Done,
    Cancelled,
}

#[derive(Clone, Copy)]
struct Waiter {
    state: WaitState,
    owner: u8,
    trb: u64,
    code: u32,
    residual: u32,
    slot: u32,
    since: u64,
}

const NO_WAITER: Waiter =
    Waiter { state: WaitState::Free, owner: 0, trb: 0, code: 0, residual: 0, slot: 0, since: 0 };

#[derive(Clone, Copy, Debug)]
struct Done {
    code: u32,
    residual: u32,
    slot: u32,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum HidKind {
    Keyboard,
    Mouse,
    Hub,
}

struct HidPipe {
    slot: u8,
    dci: u32,
    kind: HidKind,
    mps: u16,
    ring: Ring,
    bufs: Dma,
    prev: [u8; 8],
    keys: Vec<hid::KeyEvent>,
    reports: u64,
}

struct Disp {
    op: *mut u8,
    rt: *mut u8,
    db: *mut u8,
    events: Dma,
    index: usize,
    cycle: u32,
    waiters: [Waiter; WAITERS],
    hids: Vec<HidPipe>,
    ports: u32,
    hub_changes: u64,
    late: u64,
    stray: u64,
}

impl Disp {
    fn register(&mut self, trb: u64, owner: u8) -> Option<usize> {
        let now = now_us();
        for w in self.waiters.iter_mut() {
            if w.state == WaitState::Cancelled && now.saturating_sub(w.since) > 10_000_000 {
                *w = NO_WAITER;
            }
        }
        let i = self.waiters.iter().position(|w| w.state == WaitState::Free)?;
        self.waiters[i] = Waiter { state: WaitState::Pending, owner, trb, code: 0, residual: 0, slot: 0, since: now };
        Some(i)
    }

    fn forget_slot(&mut self, slot: u8, out: &mut Vec<HidPipe>) {
        for w in self.waiters.iter_mut() {
            if w.owner != slot {
                continue;
            }
            if w.state == WaitState::Pending {
                w.state = WaitState::Done;
                w.code = CC_GONE;
                w.residual = 0;
            } else if w.state == WaitState::Cancelled {
                *w = NO_WAITER;
            }
        }
        let mut i = 0;
        while i < self.hids.len() {
            if self.hids[i].slot == slot {
                out.push(self.hids.swap_remove(i));
            } else {
                i += 1;
            }
        }
    }

    fn take(&mut self, i: usize) -> Option<Done> {
        let w = self.waiters[i];
        if w.state != WaitState::Done {
            return None;
        }
        self.waiters[i] = NO_WAITER;
        Some(Done { code: w.code, residual: w.residual, slot: w.slot })
    }

    fn cancel(&mut self, i: usize) -> Option<Done> {
        if self.waiters[i].state == WaitState::Done {
            return self.take(i);
        }
        if self.waiters[i].state == WaitState::Pending {
            self.waiters[i].state = WaitState::Cancelled;
            self.waiters[i].since = now_us();
        }
        None
    }

    fn next_event(&mut self) -> Option<Event> {
        let off = self.index * 16;
        let control = self.events.read32(off + 12);
        if control & 1 != self.cycle {
            return None;
        }
        fence(Ordering::SeqCst);
        let ev = Event { param: self.events.read64(off), status: self.events.read32(off + 8), control };
        self.index += 1;
        if self.index == RING_TRBS {
            self.index = 0;
            self.cycle ^= 1;
        }
        Some(ev)
    }

    fn hid_report(&mut self, ev: &Event) -> bool {
        let db = self.db;
        let Some(h) = self.hids.iter_mut().find(|h| h.slot as u32 == ev.slot() && h.dci == ev.dci()) else {
            return false;
        };
        if !h.ring.contains(ev.param) {
            return false;
        }
        let buf = h.ring.mem.read64((ev.param - h.ring.mem.phys) as usize);
        if buf < h.bufs.phys || buf >= h.bufs.phys + (HID_TRBS * HID_BUF) as u64 {
            return true;
        }
        let off = (buf - h.bufs.phys) as usize;
        let mut hub_hit = None;
        if ev.code() == CC_SUCCESS || ev.code() == CC_SHORT_PACKET {
            h.reports += 1;
            let len = (h.mps as usize).saturating_sub((ev.status & 0x00FF_FFFF) as usize);
            let data = &h.bufs.bytes(off + len)[off..];
            match h.kind {
                HidKind::Keyboard => {
                    let mut now = [0u8; 8];
                    let n = data.len().min(8);
                    now[..n].copy_from_slice(&data[..n]);
                    h.keys.clear();
                    hid::keyboard_diff(&h.prev, &now, &mut h.keys);
                    h.prev = now;
                    for k in h.keys.iter() {
                        unsafe { usb_key(k.scancode, k.extended, k.pressed) };
                    }
                }
                HidKind::Mouse => {
                    if let Some(m) = hid::parse_mouse(data) {
                        unsafe { usb_mouse(m.dx, m.dy, m.dz, m.buttons) };
                    }
                }
                HidKind::Hub => {
                    if data.iter().any(|&b| b != 0) {
                        hub_hit = Some(h.slot);
                    }
                }
            }
        }
        h.ring.push(buf, h.mps as u32, (TRB_NORMAL << 10) | (1 << 5) | (1 << 2));
        unsafe { wr(db, h.slot as usize * 4, h.dci) };
        if let Some(slot) = hub_hit {
            self.hub_changes |= 1u64 << (slot & 63);
        }
        true
    }

    fn dispatch(&mut self) -> bool {
        let mut woke = false;
        while let Some(ev) = self.next_event() {
            match ev.kind() {
                TRB_COMMAND_DONE | TRB_TRANSFER_EVENT => {
                    if let Some(w) =
                        self.waiters.iter_mut().find(|w| w.state != WaitState::Free && w.trb != 0 && w.trb == ev.param)
                    {
                        if w.state == WaitState::Cancelled {
                            *w = NO_WAITER;
                            self.late += 1;
                        } else {
                            w.state = WaitState::Done;
                            w.code = ev.code();
                            w.residual = ev.status & 0x00FF_FFFF;
                            w.slot = ev.slot();
                            woke = true;
                        }
                    } else if !(ev.kind() == TRB_TRANSFER_EVENT && self.hid_report(&ev)) {
                        self.stray += 1;
                    }
                }
                TRB_PORT_CHANGE => {
                    let port = ((ev.param >> 24) & 0xFF) as u32;
                    if (1..32).contains(&port) {
                        self.ports |= 1 << port;
                        woke = true;
                    }
                }
                _ => {}
            }
        }
        let ir = unsafe { self.rt.add(0x20) };
        let dequeue = self.events.phys + (self.index * 16) as u64;
        unsafe {
            wr(self.op, 0x04, 1 << 3);
            wr(ir, 0x00, rd(ir, 0x00) | 1);
            wr64(ir, 0x18, dequeue | (1 << 3));
        }
        woke
    }
}

static DISP: AtomicPtr<Disp> = AtomicPtr::new(core::ptr::null_mut());
static EVLOCK: AtomicBool = AtomicBool::new(false);

fn with_disp<R>(f: impl FnOnce(&mut Disp) -> R) -> Option<R> {
    let d = DISP.load(Ordering::Acquire);
    if d.is_null() {
        return None;
    }
    let flags = unsafe { usb_irq_save() };
    while EVLOCK.compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed).is_err() {
        core::hint::spin_loop();
    }
    let r = f(unsafe { &mut *d });
    EVLOCK.store(false, Ordering::Release);
    unsafe { usb_irq_restore(flags) };
    Some(r)
}

fn pump() {
    if with_disp(|d| d.dispatch()).unwrap_or(false) {
        unsafe { usb_wake() };
    }
}

fn wait_for(i: usize, timeout_ms: u64) -> Option<Done> {
    let end = now_us() + timeout_ms * 1000;
    loop {
        pump();
        if let Some(done) = with_disp(|d| d.take(i)).flatten() {
            return Some(done);
        }
        if now_us() > end {
            return with_disp(|d| d.cancel(i)).flatten();
        }
        unsafe { usb_wait_ms(2) };
    }
}

struct Storage {
    iface: u8,
    ep_in: u8,
    ep_out: u8,
    dci_in: u32,
    dci_out: u32,
    mps_in: u16,
    mps_out: u16,
    ring_in: Ring,
    ring_out: Ring,
    data: Dma,
    cmd: Dma,
    tag: u32,
    dead: bool,
    skip_doorbell: bool,
}

#[derive(Clone, Copy)]
struct EpSetup {
    dci: u32,
    kind: u32,
    mps: u16,
    interval: u32,
    dequeue: u64,
}

#[derive(Clone, Copy)]
struct Loc {
    root_port: u8,
    route: u32,
    parent: Option<(usize, u8)>,
    tt_slot: u8,
    tt_port: u8,
    depth: u8,
}

impl Loc {
    fn root(port: u32) -> Loc {
        Loc { root_port: port as u8, route: 0, parent: None, tt_slot: 0, tt_port: 0, depth: 0 }
    }
}

pub struct Device {
    pub port: u8,
    pub slot: u8,
    pub speed: u8,
    pub desc: DeviceDesc,
    pub config: Option<Config>,
    pub manufacturer: String,
    pub product: String,
    hid: bool,
    gone: bool,
    loc: Loc,
    hub_ports: u8,
    ep0: Ring,
    input: Dma,
    output: Dma,
    msc: Option<Storage>,
}

pub struct Xhci {
    op: *mut u8,
    db: *mut u8,
    ctx_size: usize,
    max_ports: u32,
    version: u16,
    dcbaa: Dma,
    cmd: Ring,
    buffer: Dma,
    pub devices: Vec<Device>,
    disks: Vec<usize>,
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
    let end = now_us() + timeout_ms * 1000;
    loop {
        if done() {
            return true;
        }
        if now_us() > end {
            return false;
        }
        core::hint::spin_loop();
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Fail {
    Timeout,
    Stall,
    Error,
    BadCsw,
    Check,
    Gone,
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

    unsafe fn start(mmio: *mut u8, irq: bool) -> Result<Box<Xhci>, &'static str> {
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
        let disp = Box::new(Disp {
            op,
            rt,
            db,
            events,
            index: 0,
            cycle: 1,
            waiters: [NO_WAITER; WAITERS],
            hids: Vec::with_capacity(8),
            ports: 0,
            hub_changes: 0,
            late: 0,
            stray: 0,
        });
        DISP.store(Box::into_raw(disp), Ordering::Release);
        if irq {
            wr(ir, 0x04, 0);
            wr(ir, 0x00, (1 << 1) | 1);
        }

        wr(op, 0x00, rd(op, 0x00) | 1 | if irq { 1 << 2 } else { 0 });
        if !wait_until(1000, || rd(op, 0x04) & 1 == 0) {
            return Err("o controlador nao ligou");
        }
        let buffer = Dma::new(1).ok_or("sem memoria")?;
        Ok(Box::new(Xhci {
            op,
            db,
            ctx_size,
            max_ports,
            version,
            dcbaa,
            cmd,
            buffer,
            devices: Vec::new(),
            disks: Vec::new(),
        }))
    }

    fn command(&mut self, param: u64, control: u32) -> Option<Done> {
        let at = self.cmd.dequeue() & !1;
        let i = with_disp(|d| d.register(at, 0)).flatten()?;
        self.cmd.push(param, 0, control);
        unsafe { wr(self.db, 0, 0) };
        let done = wait_for(i, COMMAND_MS);
        if done.is_none() {
            log("comando do xHCI sem resposta no prazo");
        }
        done
    }

    fn command_ok(&mut self, param: u64, control: u32) -> bool {
        matches!(self.command(param, control), Some(d) if d.code == CC_SUCCESS)
    }

    fn control(&mut self, dev: usize, setup: [u8; 8], len: u16) -> Option<usize> {
        if self.devices[dev].gone {
            return None;
        }
        let slot = self.devices[dev].slot;
        let buf = self.buffer.phys;
        let data_in = setup[0] & 0x80 != 0;
        let trt = if len == 0 { 0 } else if data_in { 3 << 16 } else { 2 << 16 };
        let status_in = len == 0 || !data_in;
        let at = self.devices[dev].ep0.ahead(if len > 0 { 2 } else { 1 });
        let i = with_disp(|d| d.register(at, slot)).flatten()?;
        let ring = &mut self.devices[dev].ep0;
        ring.push(u64::from_le_bytes(setup), 8, (TRB_SETUP << 10) | (1 << 6) | trt);
        if len > 0 {
            ring.push(buf, len as u32, (TRB_DATA << 10) | if data_in { 1 << 16 } else { 0 });
        }
        ring.push(0, 0, (TRB_STATUS << 10) | (1 << 5) | if status_in { 1 << 16 } else { 0 });
        unsafe { wr(self.db, slot as usize * 4, 1) };
        let Some(done) = wait_for(i, CONTROL_MS) else {
            log("transferencia de controle sem resposta no prazo");
            self.restart_ep0(dev, true);
            return None;
        };
        if done.code == CC_STALL {
            self.restart_ep0(dev, false);
            return None;
        }
        if done.code != CC_SUCCESS && done.code != CC_SHORT_PACKET {
            return None;
        }
        Some(len as usize)
    }

    fn restart_ep0(&mut self, dev: usize, stop: bool) {
        let slot = self.devices[dev].slot as u32;
        let first = if stop { TRB_STOP_EP } else { TRB_RESET_EP };
        self.command_ok(0, (first << 10) | (1 << 16) | (slot << 24));
        let dequeue = self.devices[dev].ep0.dequeue();
        self.command_ok(dequeue, (TRB_SET_DEQUEUE << 10) | (1 << 16) | (slot << 24));
    }

    fn control_out(&mut self, dev: usize, setup: [u8; 8]) -> bool {
        self.control(dev, setup, 0).is_some()
    }

    fn configure_endpoints(&mut self, dev: usize, eps: &[EpSetup], drop: bool) -> bool {
        self.configure_endpoints_ex(dev, eps, drop, [0; 3])
    }

    fn configure_endpoints_ex(&mut self, dev: usize, eps: &[EpSetup], drop: bool, slot_or: [u32; 3]) -> bool {
        let c = self.ctx_size;
        let slot = self.devices[dev].slot as u32;
        let input = &self.devices[dev].input;
        let output = &self.devices[dev].output;
        input.clear(4096);
        let mut add = 1u32;
        let mut last = 1u32;
        for e in eps {
            add |= 1 << e.dci;
            last = last.max(e.dci);
        }
        if drop {
            input.write32(0, add & !3);
        }
        input.write32(4, add);
        for off in (0..c).step_by(4) {
            input.write32(c + off, output.read32(off));
        }
        let slot0 = input.read32(c);
        let entries = ((slot0 >> 27) & 0x1F).max(last);
        input.write32(c, (slot0 & !(0x1F << 27)) | (entries << 27) | slot_or[0]);
        input.write32(c + 4, input.read32(c + 4) | slot_or[1]);
        input.write32(c + 8, input.read32(c + 8) | slot_or[2]);
        for e in eps {
            let ep = (e.dci as usize + 1) * c;
            input.write32(ep, e.interval << 16);
            input.write32(ep + 4, (3 << 1) | (e.kind << 3) | ((e.mps as u32) << 16));
            input.write64(ep + 8, e.dequeue);
            let esit = if e.kind == 7 { e.mps as u32 } else { 0 };
            input.write32(ep + 16, e.mps as u32 | (esit << 16));
        }
        let phys = input.phys;
        self.command_ok(phys, (TRB_CONFIGURE_EP << 10) | (slot << 24))
    }

    fn ep_state(&self, dev: usize, dci: u32) -> u32 {
        self.devices[dev].output.read32(dci as usize * self.ctx_size) & 7
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
        let mut ring = Ring::new().ok_or("sem memoria")?;
        let bufs = Dma::new(1).ok_or("sem memoria")?;
        if !self.control_out(dev, [0x00, 9, cfg.value, 0, 0, 0, 0, 0]) {
            return Err("Set Configuration recusado");
        }
        let interval = hid::interval_for(self.devices[dev].speed, ep.interval);
        let setup = EpSetup { dci, kind: 7, mps, interval, dequeue: ring.dequeue() };
        if !self.configure_endpoints(dev, &[setup], false) {
            return Err("Configure Endpoint recusado");
        }
        self.control_out(dev, [0x21, 0x0B, 0, 0, iface.number, 0, 0, 0]);
        self.control_out(dev, [0x21, 0x0A, 0, 0, iface.number, 0, 0, 0]);
        let slot = self.devices[dev].slot;
        for i in 0..HID_TRBS {
            let buf = bufs.phys + (i * HID_BUF) as u64;
            ring.push(buf, mps as u32, (TRB_NORMAL << 10) | (1 << 5) | (1 << 2));
        }
        let pipe = HidPipe { slot, dci, kind, mps, ring, bufs, prev: [0; 8], keys: Vec::with_capacity(64), reports: 0 };
        with_disp(|d| d.hids.push(pipe));
        self.devices[dev].hid = true;
        unsafe { wr(self.db, slot as usize * 4, dci) };
        Ok(Some(kind))
    }

    fn submit_bulk(&mut self, dev: usize, data_in: bool, phys: u64, len: usize) -> Option<(usize, u32)> {
        let slot = self.devices[dev].slot;
        let db = self.db;
        let st = self.devices[dev].msc.as_mut()?;
        if st.dead {
            return None;
        }
        let skip = core::mem::take(&mut st.skip_doorbell);
        let (ring, dci) = if data_in { (&mut st.ring_in, st.dci_in) } else { (&mut st.ring_out, st.dci_out) };
        let at = ring.dequeue() & !1;
        let i = with_disp(|d| d.register(at, slot)).flatten()?;
        ring.push(phys, len as u32, (TRB_NORMAL << 10) | (1 << 5) | (1 << 2));
        if !skip {
            unsafe { wr(db, slot as usize * 4, dci) };
        }
        Some((i, dci))
    }

    fn cancel_endpoint(&mut self, dev: usize, dci: u32) {
        if self.devices[dev].gone {
            return;
        }
        let slot = self.devices[dev].slot as u32;
        let stopped = self.command_ok(0, (TRB_STOP_EP << 10) | (dci << 16) | (slot << 24));
        let dequeue = match self.devices[dev].msc.as_ref() {
            Some(st) if dci == st.dci_in => st.ring_in.dequeue(),
            Some(st) => st.ring_out.dequeue(),
            None => return,
        };
        let moved = self.command_ok(dequeue, (TRB_SET_DEQUEUE << 10) | (dci << 16) | (slot << 24));
        if let Some(st) = self.devices[dev].msc.as_mut() {
            st.dead = true;
        }
        let mut msg = String::new();
        let _ = write!(
            msg,
            "slot {}: transferencia sem resposta no prazo, endpoint {} {} e fila {}; o disco passa a responder EIO",
            slot,
            dci,
            if stopped { "parado" } else { "nao parou" },
            if moved { "avancada" } else { "nao avancada" }
        );
        log(&msg);
    }

    fn reset_bulk_endpoint(&mut self, dev: usize, dci: u32) -> bool {
        let slot = self.devices[dev].slot as u32;
        let Some(st) = self.devices[dev].msc.as_ref() else { return false };
        let (dequeue, mps, kind) = if dci == st.dci_in {
            (st.ring_in.dequeue(), st.mps_in, 6)
        } else {
            (st.ring_out.dequeue(), st.mps_out, 2)
        };
        if self.ep_state(dev, dci) == EP_HALTED {
            self.command_ok(0, (TRB_RESET_EP << 10) | (dci << 16) | (slot << 24))
                && self.command_ok(dequeue, (TRB_SET_DEQUEUE << 10) | (dci << 16) | (slot << 24))
        } else {
            self.command_ok(0, (TRB_STOP_EP << 10) | (dci << 16) | (slot << 24));
            let setup = EpSetup { dci, kind, mps, interval: 0, dequeue };
            self.configure_endpoints(dev, &[setup], true)
        }
    }

    fn bot_recover(&mut self, dev: usize) -> bool {
        if self.devices[dev].gone {
            return false;
        }
        let Some(st) = self.devices[dev].msc.as_ref() else { return false };
        let (iface, ep_in, ep_out, dci_in, dci_out) = (st.iface, st.ep_in, st.ep_out, st.dci_in, st.dci_out);
        let reset = self.control_out(dev, [0x21, 0xFF, 0, 0, iface, 0, 0, 0]);
        let clear_in = self.control_out(dev, [0x02, 0x01, 0, 0, ep_in, 0, 0, 0]);
        let clear_out = self.control_out(dev, [0x02, 0x01, 0, 0, ep_out, 0, 0, 0]);
        let xin = self.reset_bulk_endpoint(dev, dci_in);
        let xout = self.reset_bulk_endpoint(dev, dci_out);
        let mut msg = String::new();
        let _ = write!(
            msg,
            "slot {}: recuperacao do Bulk-Only: reset {}, CLEAR_FEATURE {} e {}, endpoints do xHCI {} e {}",
            self.devices[dev].slot,
            ok(reset),
            ok(clear_in),
            ok(clear_out),
            ok(xin),
            ok(xout)
        );
        log(&msg);
        reset && xin && xout
    }

    fn get_descriptor(&mut self, dev: usize, kind: u8, index: u8, lang: u16, len: u16) -> Option<Vec<u8>> {
        let value = ((kind as u16) << 8) | index as u16;
        let setup = [0x80, 6, value as u8, (value >> 8) as u8, lang as u8, (lang >> 8) as u8, len as u8, (len >> 8) as u8];
        self.buffer.clear(len as usize);
        let got = self.control(dev, setup, len)?;
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

    fn port_name(&self, dev: usize) -> String {
        let d = &self.devices[dev];
        let mut out = String::new();
        match d.loc.parent {
            Some((hub, p)) => {
                let _ = write!(out, "{}.{}", self.port_name(hub), p);
            }
            None => {
                let _ = write!(out, "{}", d.loc.root_port);
            }
        }
        out
    }

    fn attach(&mut self, loc: Loc, speed: u32) -> Result<(), &'static str> {
        let port = loc.root_port as u32;
        let done = self.command(0, TRB_ENABLE_SLOT << 10).ok_or("Enable Slot sem resposta")?;
        if done.code != CC_SUCCESS {
            return Err("Enable Slot recusado");
        }
        let slot = done.slot as u8;
        let input = Dma::new(1).ok_or("sem memoria")?;
        let output = Dma::new(1).ok_or("sem memoria")?;
        let ep0 = Ring::new().ok_or("sem memoria")?;
        self.dcbaa.write64(slot as usize * 8, output.phys);
        let c = self.ctx_size;
        input.write32(4, 0b11);
        input.write32(c, (loc.route & 0xFFFFF) | (speed << 20) | (1 << 27));
        input.write32(c + 4, port << 16);
        input.write32(c + 8, loc.tt_slot as u32 | ((loc.tt_port as u32) << 8));
        let mps = desc::max_packet0_for(speed as u8) as u32;
        input.write32(2 * c + 4, (3 << 1) | (4 << 3) | (mps << 16));
        input.write64(2 * c + 8, ep0.dequeue());
        input.write32(2 * c + 16, 8);
        if !self.command_ok(input.phys, (TRB_ADDRESS_DEVICE << 10) | ((slot as u32) << 24)) {
            return Err("Address Device recusado ou sem resposta");
        }
        self.devices.push(Device {
            port: port as u8,
            slot,
            speed: speed as u8,
            desc: DeviceDesc::default(),
            config: None,
            manufacturer: String::new(),
            product: String::new(),
            hid: false,
            gone: false,
            loc,
            hub_ports: 0,
            ep0,
            input,
            output,
            msc: None,
        });
        let dev = self.devices.len() - 1;
        let name = self.port_name(dev);
        if speed < 3 {
            let head = self.get_descriptor(dev, desc::DT_DEVICE, 0, 0, 8).ok_or("descritor do dispositivo nao veio")?;
            let real = head.get(7).copied().unwrap_or(8) as u32;
            if real != mps && (real == 8 || real == 16 || real == 32 || real == 64) {
                let input = &self.devices[dev].input;
                input.clear(4096);
                input.write32(4, 0b10);
                input.write32(2 * c + 4, (3 << 1) | (4 << 3) | (real << 16));
                let phys = input.phys;
                if !self.command_ok(phys, (TRB_EVALUATE_CTX << 10) | ((slot as u32) << 24)) {
                    return Err("Evaluate Context recusado");
                }
            }
        }
        let raw = self.get_descriptor(dev, desc::DT_DEVICE, 0, 0, 18).ok_or("descritor do dispositivo nao veio")?;
        let d = desc::parse_device(&raw).ok_or("descritor do dispositivo invalido")?;
        self.devices[dev].desc = d;
        if let Some(head) = self.get_descriptor(dev, desc::DT_CONFIG, 0, 0, 9) {
            if let Some(total) = desc::config_total(&head) {
                if let Some(full) = self.get_descriptor(dev, desc::DT_CONFIG, 0, 0, total.min(4096)) {
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
                    name,
                    if kind == HidKind::Keyboard { "teclado" } else { "mouse" }
                );
                log(&msg);
            }
            Ok(None) => {}
            Err(why) => {
                let mut msg = String::new();
                let _ = write!(msg, "porta {}: HID nao iniciou: {}", name, why);
                log(&msg);
            }
        }
        if Self::device_class(&self.devices[dev]) == 9 {
            if let Err(why) = self.setup_hub(dev) {
                let mut msg = String::new();
                let _ = write!(msg, "porta {}: hub nao iniciou: {}", name, why);
                log(&msg);
            }
        }
        Ok(())
    }

    fn setup_hub(&mut self, dev: usize) -> Result<(), &'static str> {
        let d = &self.devices[dev];
        if d.speed >= 4 {
            return Err("hub USB 3 (lado SuperSpeed) nao suportado");
        }
        if d.loc.depth >= 5 {
            return Err("hubs demais em cascata");
        }
        let speed = d.speed;
        let protocol = d.desc.protocol;
        let cfg = d.config.clone().ok_or("sem configuracao")?;
        let iface = cfg.interfaces.iter().find(|i| i.class == 9).copied().ok_or("sem interface de hub")?;
        let ep = cfg
            .endpoints
            .iter()
            .find(|e| e.interface == iface.number && e.address & 0x80 != 0 && e.attributes & 3 == 3)
            .copied()
            .ok_or("sem endpoint de mudanca de estado")?;
        self.buffer.clear(71);
        self.control(dev, [0xA0, 6, 0, 0x29, 0, 0, 71, 0], 71).ok_or("descritor do hub nao veio")?;
        let hd = self.buffer.bytes(9);
        if hd[1] != 0x29 {
            return Err("descritor do hub invalido");
        }
        let ports = hd[2].min(15);
        let chars = u16::from_le_bytes([hd[3], hd[4]]);
        let power_ms = (hd[5] as u64 * 2).max(100);
        let ttt = if speed == 3 { ((chars >> 5) & 3) as u32 } else { 0 };
        let mtt = speed == 3 && protocol == 2;
        if !self.control_out(dev, [0x00, 9, cfg.value, 0, 0, 0, 0, 0]) {
            return Err("Set Configuration recusado");
        }
        let mps = ep.max_packet.clamp(1, HID_BUF as u16);
        let dci = ((ep.address & 0x0F) as u32) * 2 + 1;
        let mut ring = Ring::new().ok_or("sem memoria")?;
        let bufs = Dma::new(1).ok_or("sem memoria")?;
        let interval = hid::interval_for(speed, ep.interval);
        let setup = EpSetup { dci, kind: 7, mps, interval, dequeue: ring.dequeue() };
        let slot_or = [(1 << 26) | if mtt { 1 << 25 } else { 0 }, (ports as u32) << 24, ttt << 16];
        if !self.configure_endpoints_ex(dev, &[setup], false, slot_or) {
            return Err("Configure Endpoint do hub recusado");
        }
        let slot = self.devices[dev].slot;
        for i in 0..HID_TRBS {
            let buf = bufs.phys + (i * HID_BUF) as u64;
            ring.push(buf, mps as u32, (TRB_NORMAL << 10) | (1 << 5) | (1 << 2));
        }
        let pipe = HidPipe { slot, dci, kind: HidKind::Hub, mps, ring, bufs, prev: [0; 8], keys: Vec::new(), reports: 0 };
        with_disp(|d| d.hids.push(pipe));
        unsafe { wr(self.db, slot as usize * 4, dci) };
        self.devices[dev].hub_ports = ports;
        for p in 1..=ports {
            self.control_out(dev, [0x23, 3, 8, 0, p, 0, 0, 0]);
        }
        unsafe { usb_sleep_ms(power_ms) };
        let name = self.port_name(dev);
        let mut msg = String::new();
        let _ = write!(
            msg,
            "porta {}: hub {} com {} porta(s){}{}",
            name,
            desc::speed_name(speed),
            ports,
            if speed == 3 { if mtt { ", TT por porta" } else { ", um TT" } } else { "" },
            if self.devices[dev].loc.depth > 0 { ", em cascata" } else { "" }
        );
        log(&msg);
        for p in 1..=ports {
            self.hub_port_attach(dev, p);
        }
        Ok(())
    }

    fn hub_port_status(&mut self, hub: usize, port: u8) -> Option<(u16, u16)> {
        self.buffer.clear(4);
        self.control(hub, [0xA3, 0, 0, 0, port, 0, 4, 0], 4)?;
        let b = self.buffer.bytes(4);
        Some((u16::from_le_bytes([b[0], b[1]]), u16::from_le_bytes([b[2], b[3]])))
    }

    fn hub_clear(&mut self, hub: usize, port: u8, feature: u8) {
        self.control_out(hub, [0x23, 1, feature, 0, port, 0, 0, 0]);
    }

    fn hub_port_attach(&mut self, hub: usize, port: u8) -> Option<usize> {
        let (status, _) = self.hub_port_status(hub, port)?;
        self.hub_clear(hub, port, 16);
        if status & 1 == 0 {
            return None;
        }
        self.control_out(hub, [0x23, 3, 4, 0, port, 0, 0, 0]);
        let end = now_us() + 500_000;
        let mut st = 0;
        while now_us() < end {
            unsafe { usb_sleep_ms(10) };
            let (s, change) = self.hub_port_status(hub, port)?;
            st = s;
            if change & (1 << 4) != 0 || (s & (1 << 4) == 0 && s & 2 != 0) {
                break;
            }
        }
        self.hub_clear(hub, port, 20);
        self.hub_clear(hub, port, 16);
        let name = self.port_name(hub);
        if st & 2 == 0 {
            let mut msg = String::new();
            let _ = write!(msg, "porta {}.{}: o hub nao habilitou a porta depois do reset", name, port);
            log(&msg);
            return None;
        }
        let speed = if st & (1 << 9) != 0 { 2 } else if st & (1 << 10) != 0 { 3 } else { 1 };
        unsafe { usb_sleep_ms(10) };
        let h = &self.devices[hub];
        let (tt_slot, tt_port) = if h.speed == 3 && speed < 3 { (h.slot, port) } else { (h.loc.tt_slot, h.loc.tt_port) };
        let loc = Loc {
            root_port: h.loc.root_port,
            route: h.loc.route | ((port.min(15) as u32) << (4 * h.loc.depth as u32)),
            parent: Some((hub, port)),
            tt_slot,
            tt_port,
            depth: h.loc.depth + 1,
        };
        if let Err(why) = self.attach(loc, speed) {
            let mut msg = String::new();
            let _ = write!(msg, "porta {}.{}: {}", name, port, why);
            log(&msg);
            return None;
        }
        Some(self.devices.len() - 1)
    }

    fn enumerate(&mut self) {
        for port in 1..=self.max_ports {
            let Some(speed) = self.reset_port(port) else { continue };
            if let Err(why) = self.attach(Loc::root(port), speed) {
                let mut msg = String::new();
                let _ = write!(msg, "porta {}: {}", port, why);
                log(&msg);
            }
        }
    }

    fn prepare_msc(&mut self, dev: usize) -> Result<bool, &'static str> {
        let Some(cfg) = self.devices[dev].config.clone() else { return Ok(false) };
        let Some(iface) = cfg.interfaces.iter().find(|i| i.class == 8 && i.subclass == 6 && i.protocol == 0x50).copied()
        else {
            return Ok(false);
        };
        let bulk = |dir_in: bool| {
            cfg.endpoints
                .iter()
                .find(|e| e.interface == iface.number && e.attributes & 3 == 2 && (e.address & 0x80 != 0) == dir_in)
                .copied()
        };
        let (Some(ep_in), Some(ep_out)) = (bulk(true), bulk(false)) else { return Ok(false) };
        let st = Storage {
            iface: iface.number,
            ep_in: ep_in.address,
            ep_out: ep_out.address,
            dci_in: ((ep_in.address & 0x0F) as u32) * 2 + 1,
            dci_out: ((ep_out.address & 0x0F) as u32) * 2,
            mps_in: ep_in.max_packet,
            mps_out: ep_out.max_packet,
            ring_in: Ring::new().ok_or("sem memoria")?,
            ring_out: Ring::new().ok_or("sem memoria")?,
            data: Dma::new(MSC_PAGES).ok_or("sem memoria")?,
            cmd: Dma::new(1).ok_or("sem memoria")?,
            tag: 0,
            dead: false,
            skip_doorbell: false,
        };
        if !self.control_out(dev, [0x00, 9, cfg.value, 0, 0, 0, 0, 0]) {
            return Err("Set Configuration recusado");
        }
        let eps = [
            EpSetup { dci: st.dci_out, kind: 2, mps: st.mps_out, interval: 0, dequeue: st.ring_out.dequeue() },
            EpSetup { dci: st.dci_in, kind: 6, mps: st.mps_in, interval: 0, dequeue: st.ring_in.dequeue() },
        ];
        if !self.configure_endpoints(dev, &eps, false) {
            return Err("Configure Endpoint recusado");
        }
        self.devices[dev].msc = Some(st);
        Ok(true)
    }

    fn ack_port(&self, port: u32) -> u32 {
        let reg = 0x400 + (port as usize - 1) * 0x10;
        let sc = unsafe { rd(self.op, reg) };
        unsafe { wr(self.op, reg, (sc & !(PORTSC_PED | PORTSC_PR | PORTSC_CHANGE)) | (sc & PORTSC_CHANGE)) };
        sc
    }

    fn detach(&mut self, dev: usize, gone: &mut Vec<u32>) {
        let children: Vec<usize> = (0..self.devices.len())
            .filter(|&i| !self.devices[i].gone && self.devices[i].loc.parent.map(|p| p.0) == Some(dev))
            .collect();
        for child in children {
            self.detach(child, gone);
        }
        let slot = self.devices[dev].slot;
        let port = self.port_name(dev);
        self.devices[dev].gone = true;
        if let Some(st) = self.devices[dev].msc.as_mut() {
            st.dead = true;
        }
        if let Some(i) = self.disks.iter().position(|&d| d == dev) {
            gone.push(i as u32);
        }
        let mut pipes = Vec::with_capacity(4);
        with_disp(|d| d.forget_slot(slot, &mut pipes));
        unsafe { usb_wake() };
        let disabled = self.command_ok(0, (TRB_DISABLE_SLOT << 10) | ((slot as u32) << 24));
        if disabled {
            self.dcbaa.write64(slot as usize * 8, 0);
            for p in &pipes {
                p.ring.mem.free();
                p.bufs.free();
            }
            let d = &self.devices[dev];
            d.ep0.mem.free();
            d.input.free();
            d.output.free();
        }
        let mut msg = String::new();
        let _ = write!(
            msg,
            "porta {}: dispositivo removido, Disable Slot do slot {} {}",
            port,
            slot,
            if disabled { "ok" } else { "falhou" }
        );
        log(&msg);
    }

    fn port_changed(&mut self, port: u32, gone: &mut Vec<u32>, fresh: &mut Vec<(usize, String)>) {
        let sc = self.ack_port(port);
        let live = self.devices.iter().position(|d| !d.gone && d.loc.parent.is_none() && d.port as u32 == port);
        if let Some(dev) = live {
            if sc & PORTSC_CCS != 0 && sc & PORTSC_CSC == 0 {
                return;
            }
            self.detach(dev, gone);
        }
        if sc & PORTSC_CCS == 0 {
            return;
        }
        let Some(speed) = self.reset_port(port) else { return };
        let first = self.devices.len();
        if let Err(why) = self.attach(Loc::root(port), speed) {
            let mut msg = String::new();
            let _ = write!(msg, "porta {}: {}", port, why);
            log(&msg);
            return;
        }
        self.adopt(first, fresh);
    }

    fn hub_changed(&mut self, bits: u64, gone: &mut Vec<u32>, fresh: &mut Vec<(usize, String)>) {
        let count = self.devices.len();
        for dev in 0..count {
            let d = &self.devices[dev];
            if d.gone || d.hub_ports == 0 || bits & (1u64 << (d.slot & 63)) == 0 {
                continue;
            }
            let ports = d.hub_ports;
            self.buffer.clear(4);
            if self.control(dev, [0xA0, 0, 0, 0, 0, 0, 4, 0], 4).is_some() {
                let hub_change = u16::from_le_bytes([self.buffer.bytes(4)[2], self.buffer.bytes(4)[3]]);
                for feature in 0..2u8 {
                    if hub_change & (1 << feature) != 0 {
                        self.control_out(dev, [0x20, 1, feature, 0, 0, 0, 0, 0]);
                    }
                }
            }
            for port in 1..=ports {
                if self.devices[dev].gone {
                    break;
                }
                let Some((status, change)) = self.hub_port_status(dev, port) else { continue };
                for bit in 0..5u8 {
                    if change & (1 << bit) != 0 {
                        self.hub_clear(dev, port, 16 + bit);
                    }
                }
                if change & 1 == 0 {
                    continue;
                }
                let child = (0..self.devices.len())
                    .find(|&i| !self.devices[i].gone && self.devices[i].loc.parent == Some((dev, port)));
                if let Some(child) = child {
                    self.detach(child, gone);
                }
                if status & 1 != 0 {
                    let first = self.devices.len();
                    self.hub_port_attach(dev, port);
                    self.adopt(first, fresh);
                }
            }
        }
    }

    fn adopt(&mut self, first: usize, fresh: &mut Vec<(usize, String)>) {
        for dev in first..self.devices.len() {
            if self.devices[dev].gone {
                continue;
            }
            let name = self.port_name(dev);
            let mut msg = String::new();
            let d = &self.devices[dev];
            let _ = write!(msg, "porta {}: conectado {:04x}:{:04x} {} ({})", name, d.desc.vendor, d.desc.product, d.product, desc::speed_name(d.speed));
            log(&msg);
            match self.prepare_msc(dev) {
                Ok(true) => fresh.push((dev, name.clone())),
                Ok(false) => {}
                Err(why) => {
                    let mut m = String::new();
                    let _ = write!(m, "porta {}: armazenamento nao iniciou: {}", name, why);
                    log(&m);
                }
            }
        }
    }

    fn msc_release(&mut self, disk: usize) {
        let Some(&dev) = self.disks.get(disk) else { return };
        if !self.devices[dev].gone {
            return;
        }
        if let Some(st) = self.devices[dev].msc.take() {
            st.ring_in.mem.free();
            st.ring_out.mem.free();
            st.data.free();
            st.cmd.free();
        }
    }

    fn device_class(d: &Device) -> u8 {
        if d.desc.class != 0 {
            return d.desc.class;
        }
        d.config.as_ref().and_then(|c| c.interfaces.first()).map(|i| i.class).unwrap_or(0)
    }

    pub fn report(&self, out: &mut String) {
        for (i, d) in self.devices.iter().enumerate().filter(|(_, d)| !d.gone) {
            let _ = writeln!(
                out,
                "Porta {} Slot {}: ID {:04x}:{:04x} {} {} (USB {:x}.{:02x}, {}, classe {})",
                self.port_name(i),
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

fn ok(v: bool) -> &'static str {
    if v {
        "ok"
    } else {
        "falhou"
    }
}

static CONTROLLER: AtomicPtr<Xhci> = AtomicPtr::new(core::ptr::null_mut());
static BUSY: AtomicBool = AtomicBool::new(false);

fn with_controller<R>(f: impl FnOnce(Option<&mut Xhci>) -> R) -> R {
    while BUSY.compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed).is_err() {
        unsafe { usb_yield() };
    }
    let hc = CONTROLLER.load(Ordering::Acquire);
    let r = f(unsafe { hc.as_mut() });
    BUSY.store(false, Ordering::Release);
    r
}

fn bulk(dev: usize, data_in: bool, phys: u64, len: usize) -> Result<usize, Fail> {
    let (i, dci) = with_controller(|hc| hc.and_then(|hc| hc.submit_bulk(dev, data_in, phys, len))).ok_or(Fail::Gone)?;
    match wait_for(i, BULK_MS) {
        Some(done) if done.code == CC_SUCCESS || done.code == CC_SHORT_PACKET => {
            Ok(len.saturating_sub(done.residual as usize))
        }
        Some(done) if done.code == CC_GONE => Err(Fail::Gone),
        Some(done) if done.code == CC_STALL => Err(Fail::Stall),
        Some(_) => Err(Fail::Error),
        None => {
            with_controller(|hc| {
                if let Some(hc) = hc {
                    hc.cancel_endpoint(dev, dci)
                }
            });
            Err(Fail::Timeout)
        }
    }
}

fn scsi_once(dev: usize, cdb: &[u8], data_in: bool, len: usize) -> Result<usize, Fail> {
    let (cmd, data, tag) = with_controller(|hc| {
        let st = hc?.devices.get_mut(dev)?.msc.as_mut()?;
        st.tag = st.tag.wrapping_add(1);
        st.cmd.put(0, &msc::cbw(st.tag, len as u32, data_in, cdb));
        st.cmd.put(512, &[0u8; msc::CSW_LEN]);
        Some((st.cmd.phys, st.data.phys, st.tag))
    })
    .ok_or(Fail::Gone)?;
    bulk(dev, false, cmd, msc::CBW_LEN)?;
    let got = if len > 0 { bulk(dev, data_in, data, len)? } else { 0 };
    bulk(dev, true, cmd + 512, msc::CSW_LEN)?;
    let csw = with_controller(|hc| {
        let st = hc?.devices.get(dev)?.msc.as_ref()?;
        Some(msc::check_csw(&st.cmd.bytes(512 + msc::CSW_LEN)[512..], tag))
    })
    .ok_or(Fail::Gone)?;
    match csw {
        Ok(_) => Ok(got),
        Err(msc::CswError::Failed(1)) => Err(Fail::Check),
        Err(_) => Err(Fail::BadCsw),
    }
}

fn request_sense(dev: usize, cdb0: u8) {
    let mut msg = String::new();
    match scsi_once(dev, &msc::request_sense(18), true, 18) {
        Ok(_) => {
            let sense = with_controller(|hc| {
                let st = hc?.devices.get(dev)?.msc.as_ref()?;
                msc::parse_sense(st.data.bytes(18))
            });
            match sense {
                Some(s) => {
                    let _ = write!(
                        msg,
                        "comando SCSI 0x{:02x} falhou: sense chave 0x{:x}, ASC 0x{:02x}, ASCQ 0x{:02x}",
                        cdb0, s.key, s.asc, s.ascq
                    );
                }
                None => {
                    let _ = write!(msg, "comando SCSI 0x{:02x} falhou; o sense veio invalido", cdb0);
                }
            }
        }
        Err(e) => {
            let _ = write!(msg, "comando SCSI 0x{:02x} falhou e o REQUEST SENSE tambem ({:?})", cdb0, e);
        }
    }
    log(&msg);
}

fn scsi(dev: usize, cdb: &[u8], data_in: bool, len: usize) -> Result<usize, Fail> {
    for attempt in 0..2 {
        match scsi_once(dev, cdb, data_in, len) {
            Ok(n) => return Ok(n),
            Err(Fail::Check) => {
                request_sense(dev, cdb[0]);
                return Err(Fail::Check);
            }
            Err(Fail::Stall) | Err(Fail::BadCsw) | Err(Fail::Error) if attempt == 0 => {
                if !with_controller(|hc| hc.map(|hc| hc.bot_recover(dev)).unwrap_or(false)) {
                    return Err(Fail::Error);
                }
            }
            Err(e) => return Err(e),
        }
    }
    Err(Fail::Error)
}

fn start_msc(dev: usize, port: String) -> Option<(u32, u64, String)> {
    let mut name = String::new();
    if scsi(dev, &msc::inquiry(36), true, 36).is_ok() {
        if let Some((v, m)) =
            with_controller(|hc| msc::inquiry_name(hc?.devices.get(dev)?.msc.as_ref()?.data.bytes(36)))
        {
            let _ = write!(name, "{} {}", v, m);
        }
    }
    let mut ready = false;
    for _ in 0..5 {
        if scsi(dev, &msc::test_unit_ready(), false, 0).is_ok() {
            ready = true;
            break;
        }
        unsafe { usb_sleep_ms(50) };
    }
    let mut msg = String::new();
    if !ready {
        let _ = write!(msg, "porta {}: o pendrive nao ficou pronto", port);
        log(&msg);
        return None;
    }
    let cap = scsi(dev, &msc::read_capacity10(), true, 8)
        .ok()
        .and_then(|_| with_controller(|hc| msc::parse_capacity(hc?.devices.get(dev)?.msc.as_ref()?.data.bytes(8))));
    let Some((blocks, size)) = cap else {
        let _ = write!(msg, "porta {}: READ CAPACITY falhou", port);
        log(&msg);
        return None;
    };
    if size != 512 {
        let _ = write!(msg, "porta {}: pendrive com blocos de {} bytes recusado; so 512 e suportado", port, size);
        log(&msg);
        return None;
    }
    let index = with_controller(|hc| {
        hc.map(|hc| {
            hc.disks.push(dev);
            hc.disks.len() as u32 - 1
        })
    })?;
    let _ = write!(msg, "porta {}: armazenamento '{}', {} blocos de 512 bytes ({} MiB)", port, name, blocks, blocks / 2048);
    log(&msg);
    Some((index, blocks, name))
}

fn msc_rw(disk: usize, mut lba: u64, mut count: u32, mut buf: *mut u8, write: bool) -> bool {
    let Some((dev, data)) = with_controller(|hc| {
        let hc = hc?;
        let dev = *hc.disks.get(disk)?;
        let st = hc.devices.get(dev)?.msc.as_ref()?;
        if st.dead {
            return None;
        }
        Some((dev, st.data.virt))
    }) else {
        return false;
    };
    if lba + count as u64 > u32::MAX as u64 {
        return false;
    }
    while count > 0 {
        let n = count.min(MSC_BLOCKS);
        let bytes = n as usize * 512;
        if write {
            unsafe { core::ptr::copy_nonoverlapping(buf, data, bytes) };
        }
        let cdb = if write { msc::write10(lba as u32, n as u16) } else { msc::read10(lba as u32, n as u16) };
        if scsi(dev, &cdb, !write, bytes) != Ok(bytes) {
            return false;
        }
        if !write {
            unsafe { core::ptr::copy_nonoverlapping(data, buf, bytes) };
        }
        buf = unsafe { buf.add(bytes) };
        lba += n as u64;
        count -= n;
    }
    true
}

#[no_mangle]
pub unsafe extern "C" fn xhci_start(mmio: *mut u8, irq: bool) -> i32 {
    if !CONTROLLER.load(Ordering::Acquire).is_null() {
        return -1;
    }
    let mut hc = match Xhci::start(mmio, irq) {
        Ok(hc) => hc,
        Err(why) => {
            log(why);
            return -1;
        }
    };
    let mut msg = String::new();
    let _ = write!(
        msg,
        "xHCI {:x}.{:02x}, {} porta(s), eventos {}",
        hc.version >> 8,
        hc.version & 0xFF,
        hc.max_ports,
        if irq { "por interrupcao" } else { "por consulta" }
    );
    log(&msg);
    hc.enumerate();
    let mut storages = Vec::new();
    for dev in 0..hc.devices.len() {
        match hc.prepare_msc(dev) {
            Ok(true) => storages.push((dev, hc.port_name(dev))),
            Ok(false) => {}
            Err(why) => {
                let mut m = String::new();
                let _ = write!(m, "porta {}: armazenamento nao iniciou: {}", hc.devices[dev].port, why);
                log(&m);
            }
        }
    }
    let n = hc.devices.len() as i32;
    for (i, d) in hc.devices.iter().enumerate() {
        let mut line = String::new();
        let _ = write!(line, "porta {}: {:04x}:{:04x} {} ({})", hc.port_name(i), d.desc.vendor, d.desc.product, d.product, desc::speed_name(d.speed));
        log(&line);
    }
    for port in 1..=hc.max_ports {
        hc.ack_port(port);
    }
    CONTROLLER.store(Box::into_raw(hc), Ordering::Release);
    for (dev, port) in storages {
        if let Some((index, blocks, name)) = start_msc(dev, port) {
            usb_disk_add(index, blocks, name.as_ptr(), name.len());
        }
    }
    n
}

fn hotplug() {
    let changed = with_disp(|d| core::mem::take(&mut d.ports)).unwrap_or(0);
    let hubs = with_disp(|d| core::mem::take(&mut d.hub_changes)).unwrap_or(0);
    if changed == 0 && hubs == 0 {
        return;
    }
    let mut gone = Vec::new();
    let mut fresh = Vec::new();
    with_controller(|hc| {
        if let Some(hc) = hc {
            for port in 1..=hc.max_ports.min(31) {
                if changed & (1 << port) != 0 {
                    hc.port_changed(port, &mut gone, &mut fresh);
                }
            }
            if hubs != 0 {
                hc.hub_changed(hubs, &mut gone, &mut fresh);
            }
        }
    });
    for index in gone {
        unsafe { usb_disk_gone(index) };
    }
    for (dev, port) in fresh {
        if let Some((index, blocks, name)) = start_msc(dev, port) {
            unsafe { usb_disk_add(index, blocks, name.as_ptr(), name.len()) };
        }
    }
}

#[no_mangle]
pub extern "C" fn xhci_irq() {
    pump();
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
    let mut pipes = [(0u8, HidKind::Hub, 0u64); 16];
    let count = with_disp(|d| {
        let n = d.hids.len().min(pipes.len());
        for (i, h) in d.hids.iter().take(n).enumerate() {
            pipes[i] = (h.slot, h.kind, h.reports);
        }
        n
    })
    .unwrap_or(0);
    for &(slot, kind, reports) in pipes.iter().take(count) {
        let what = match kind {
            HidKind::Keyboard => "teclado",
            HidKind::Mouse => "mouse",
            HidKind::Hub => "hub",
        };
        let _ = writeln!(text, "HID: slot {} ({}): {} relatorio(s)", slot, what, reports);
    }
    if let Some((late, stray)) = with_disp(|d| (d.late, d.stray)) {
        let _ = writeln!(text, "eventos: {} conclusao(oes) tardia(s) ignorada(s), {} sem dono", late, stray);
    }
    let n = text.len().min(cap);
    core::ptr::copy_nonoverlapping(text.as_ptr(), buf, n);
    n
}

#[no_mangle]
pub extern "C" fn xhci_poll() {
    pump();
    hotplug();
}

#[no_mangle]
pub extern "C" fn xhci_hid_count() -> i32 {
    with_controller(|hc| hc.map(|hc| hc.devices.iter().filter(|d| d.hid && !d.gone).count() as i32).unwrap_or(0))
}

#[no_mangle]
pub unsafe extern "C" fn xhci_msc_read(disk: u32, lba: u64, count: u32, buf: *mut u8) -> bool {
    msc_rw(disk as usize, lba, count, buf, false)
}

#[no_mangle]
pub unsafe extern "C" fn xhci_msc_write(disk: u32, lba: u64, count: u32, buf: *const u8) -> bool {
    msc_rw(disk as usize, lba, count, buf as *mut u8, true)
}

#[no_mangle]
pub extern "C" fn xhci_msc_test_stall(disk: u32) -> i32 {
    if !unsafe { usb_test_allowed() } {
        return -EPERM;
    }
    let Some(dev) = with_controller(|hc| hc.and_then(|hc| hc.disks.get(disk as usize).copied())) else {
        return -ENODEV;
    };
    let mut before = [0u8; 512];
    if !msc_rw(disk as usize, 0, 1, before.as_mut_ptr(), false) {
        log("teste: a leitura do setor 0 antes do STALL falhou");
        return -EIO;
    }
    let cmd = with_controller(|hc| {
        let st = hc?.devices.get_mut(dev)?.msc.as_mut()?;
        let mut bad = msc::cbw(0xBAD0_0001, 0, false, &msc::test_unit_ready());
        bad[0] = b'X';
        st.cmd.put(0, &bad);
        Some(st.cmd.phys)
    });
    let Some(cmd) = cmd else { return -ENODEV };
    let first = bulk(dev, false, cmd, msc::CBW_LEN);
    let mut msg = String::new();
    let _ = write!(msg, "teste: CBW com assinatura errada respondeu {:?}", first);
    log(&msg);
    if !with_controller(|hc| hc.map(|hc| hc.bot_recover(dev)).unwrap_or(false)) {
        return -EIO;
    }
    let mut sector = [0u8; 512];
    with_controller(|hc| {
        if let Some(st) = hc.and_then(|hc| hc.devices.get(dev)).and_then(|d| d.msc.as_ref()) {
            st.data.clear(MSC_PAGES * 4096);
        }
    });
    let unit = scsi(dev, &msc::test_unit_ready(), false, 0).is_ok();
    let read = msc_rw(disk as usize, 0, 1, sector.as_mut_ptr(), false);
    let same = read && sector == before;
    let mut msg = String::new();
    let _ = write!(
        msg,
        "teste: depois da recuperacao TEST UNIT READY {} e READ(10) do setor 0 {}; os 512 bytes {} aos lidos antes do STALL (assinatura {:02x}{:02x})",
        ok(unit),
        ok(read),
        if same { "iguais" } else { "diferentes" },
        sector[510],
        sector[511]
    );
    log(&msg);
    let outside = scsi(dev, &msc::read10(0xFFFF_FFF0, 1), true, 512);
    let mut msg = String::new();
    let _ = write!(msg, "teste: READ(10) fora do disco respondeu {:?}", outside);
    log(&msg);
    if matches!(first, Err(Fail::Stall)) && unit && same && outside == Err(Fail::Check) {
        0
    } else {
        -EIO
    }
}

#[no_mangle]
pub extern "C" fn xhci_msc_release(disk: u32) {
    with_controller(|hc| {
        if let Some(hc) = hc {
            hc.msc_release(disk as usize)
        }
    });
}

#[no_mangle]
pub extern "C" fn xhci_msc_test_timeout(disk: u32) -> i32 {
    if !unsafe { usb_test_allowed() } {
        return -EPERM;
    }
    let armed = with_controller(|hc| {
        let hc = hc?;
        let dev = *hc.disks.get(disk as usize)?;
        let st = hc.devices.get_mut(dev)?.msc.as_mut()?;
        st.skip_doorbell = true;
        Some(())
    });
    if armed.is_none() {
        return -ENODEV;
    }
    let mut sector = [0u8; 512];
    let t0 = now_us();
    let first = msc_rw(disk as usize, 0, 1, sector.as_mut_ptr(), false);
    let waited = (now_us() - t0) / 1000;
    let t1 = now_us();
    let second = msc_rw(disk as usize, 0, 1, sector.as_mut_ptr(), false);
    let after = (now_us() - t1) / 1000;
    let mut msg = String::new();
    let _ = write!(
        msg,
        "teste: com a campainha calada a leitura {} em {} ms; a seguinte {} em {} ms",
        if first { "voltou" } else { "falhou" },
        waited,
        if second { "voltou" } else { "falhou" },
        after
    );
    log(&msg);
    if !first && !second && waited + 500 >= BULK_MS && after < 100 {
        0
    } else {
        -EIO
    }
}
