use core::alloc::{GlobalAlloc, Layout};
use core::ffi::{c_char, c_void};
use core::fmt::Write;
use core::panic::PanicInfo;

extern "C" {
    fn kmalloc(size: usize) -> *mut c_void;
    fn kfree(ptr: *mut c_void);
    fn panic(fmt: *const c_char, ...) -> !;
}

const NATURAL_ALIGN: usize = 16;

struct KernelHeap;

unsafe impl GlobalAlloc for KernelHeap {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        if layout.align() <= NATURAL_ALIGN {
            return kmalloc(layout.size().max(1)) as *mut u8;
        }
        let total = layout.size() + layout.align() + core::mem::size_of::<usize>();
        let raw = kmalloc(total) as *mut u8;
        if raw.is_null() {
            return raw;
        }
        let start = raw as usize + core::mem::size_of::<usize>();
        let aligned = (start + layout.align() - 1) & !(layout.align() - 1);
        *((aligned - core::mem::size_of::<usize>()) as *mut usize) = raw as usize;
        aligned as *mut u8
    }

    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        if ptr.is_null() {
            return;
        }
        if layout.align() <= NATURAL_ALIGN {
            kfree(ptr as *mut c_void);
            return;
        }
        let raw = *((ptr as usize - core::mem::size_of::<usize>()) as *const usize);
        kfree(raw as *mut c_void);
    }
}

#[global_allocator]
static HEAP: KernelHeap = KernelHeap;

struct Msg {
    buf: [u8; 256],
    len: usize,
}

impl Write for Msg {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        for &b in s.as_bytes() {
            if self.len + 1 >= self.buf.len() {
                break;
            }
            self.buf[self.len] = b;
            self.len += 1;
        }
        Ok(())
    }
}

#[panic_handler]
fn on_panic(info: &PanicInfo) -> ! {
    let mut msg = Msg { buf: [0; 256], len: 0 };
    let _ = write!(msg, "{}", info);
    msg.buf[msg.len] = 0;
    unsafe { panic(c"rust: %s".as_ptr() as *const c_char, msg.buf.as_ptr()) }
}
