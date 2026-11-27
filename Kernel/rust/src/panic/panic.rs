use core::arch::asm;
use core::ffi::c_char;
use core::fmt::{self, Write};
use core::panic::PanicInfo;
use core::ptr;
use core::slice;
use core::sync::atomic::{AtomicBool, Ordering};

static PANICKING: AtomicBool = AtomicBool::new(false);

unsafe extern "C" {
    fn serial_putc_uart(character: c_char);
    fn vesa_blit_rgb_scaled(pixels: *const u8, source_width: u64, source_height: u64) -> bool;
    fn vesa_draw_text(x: u64, y: u64, color: u32, text: *const c_char, length: u64);
    fn vesa_draw_batch_begin();
    fn vesa_draw_batch_end();
    fn vesa_fill_rect(x: u64, y: u64, width: u64, height: u64, color: u32);
    fn vesa_get_width() -> u64;
    fn vesa_get_height() -> u64;

    static panic_background_start: u8;
    static panic_background_end: u8;
}

struct PanicWriter {
    framebuffer: bool,
    screen_width: u64,
    screen_height: u64,
    left: u64,
    x: u64,
    y: u64,
}

impl PanicWriter {
    fn next_line(&mut self) {
        self.x = self.left;
        self.y = self.y.saturating_add(18);
    }

    fn draw_byte(&mut self, byte: u8) {
        if !self.framebuffer || self.y.saturating_add(16) > self.screen_height {
            return;
        }

        if self.x.saturating_add(8) > self.screen_width {
            self.next_line();
            if self.y.saturating_add(16) > self.screen_height {
                return;
            }
        }

        let character = byte as c_char;
        unsafe {
            vesa_draw_text(self.x, self.y, 0x00FFFFFF, &character, 1);
        }
        self.x = self.x.saturating_add(8);
    }

    fn put_byte(&mut self, byte: u8) {
        let byte =
            if byte == b'\n' || byte == b'\r' || byte == b'\t' || (0x20..=0x7e).contains(&byte) {
                byte
            } else {
                b'?'
            };

        unsafe { serial_putc_uart(byte as c_char) };

        match byte {
            b'\n' => self.next_line(),
            b'\r' => self.x = self.left,
            b'\t' => {
                for _ in 0..4 {
                    self.draw_byte(b' ');
                }
            }
            _ => self.draw_byte(byte),
        }
    }

    fn write_c_string(&mut self, message: *const c_char) {
        if message.is_null() {
            let _ = self.write_str("unspecified fatal error");
            return;
        }

        for index in 0..512 {
            let byte = unsafe { message.add(index).read() as u8 };
            if byte == 0 {
                return;
            }
            self.put_byte(byte);
        }
        let _ = self.write_str(" [message truncated]");
    }
}

impl Write for PanicWriter {
    fn write_str(&mut self, text: &str) -> fmt::Result {
        for byte in text.bytes() {
            self.put_byte(byte);
        }
        Ok(())
    }
}

fn read_u32_le(bytes: &[u8], offset: usize) -> Option<u64> {
    let value = bytes.get(offset..offset.checked_add(4)?)?;
    Some(u32::from_le_bytes([value[0], value[1], value[2], value[3]]) as u64)
}

fn draw_panic_background() -> bool {
    unsafe {
        let start = ptr::addr_of!(panic_background_start);
        let end = ptr::addr_of!(panic_background_end);
        let length = (end as usize).saturating_sub(start as usize);
        if length < 8 {
            return false;
        }

        let image = slice::from_raw_parts(start, length);
        let Some(image_width) = read_u32_le(image, 0) else {
            return false;
        };
        let Some(image_height) = read_u32_le(image, 4) else {
            return false;
        };
        let Some(pixel_bytes) = image_width
            .checked_mul(image_height)
            .and_then(|pixels| pixels.checked_mul(3))
        else {
            return false;
        };
        let Some(expected_length) = usize::try_from(pixel_bytes)
            .ok()
            .and_then(|bytes| bytes.checked_add(8))
        else {
            return false;
        };
        if image_width == 0 || image_height == 0 || expected_length > image.len() {
            return false;
        }

        vesa_blit_rgb_scaled(image.as_ptr().add(8), image_width, image_height)
    }
}

fn begin_panic() -> PanicWriter {
    unsafe {
        asm!("cli", options(nomem, nostack));
    }

    if PANICKING.swap(true, Ordering::Relaxed) {
        for byte in b"\n*** SECONDARY KERNEL PANIC ***\n" {
            unsafe { serial_putc_uart(*byte as c_char) };
        }
        halt_cpu();
    }

    let width = unsafe { vesa_get_width() };
    let height = unsafe { vesa_get_height() };
    let framebuffer = width != 0 && height != 0;
    if framebuffer {
        unsafe { vesa_draw_batch_begin() };
        if !draw_panic_background() {
            unsafe { vesa_fill_rect(0, 0, width, height, 0x170A0A) };
        }
    }

    let left = width.saturating_mul(13) / 100;
    let y = height.saturating_mul(52) / 100;
    PanicWriter {
        framebuffer,
        screen_width: width,
        screen_height: height,
        left,
        x: left,
        y,
    }
}

fn finish_panic(output: &PanicWriter) -> ! {
    if output.framebuffer {
        unsafe { vesa_draw_batch_end() };
    }
    halt_cpu()
}

fn write_hex(output: &mut PanicWriter, value: u64) {
    const DIGITS: &[u8; 16] = b"0123456789ABCDEF";
    let _ = output.write_str("0x");
    for shift in (0..16).rev() {
        output.put_byte(DIGITS[((value >> (shift * 4)) & 0x0f) as usize]);
    }
}

fn write_decimal(output: &mut PanicWriter, mut value: u64) {
    let mut digits = [0u8; 20];
    let mut start = digits.len();
    loop {
        start -= 1;
        digits[start] = b'0' + (value % 10) as u8;
        value /= 10;
        if value == 0 {
            break;
        }
    }
    for digit in &digits[start..] {
        output.put_byte(*digit);
    }
}

fn exception_name(vector: u64) -> &'static str {
    match vector {
        0 => "Divide error",
        1 => "Debug",
        2 => "NMI",
        3 => "Breakpoint",
        4 => "Overflow",
        5 => "BOUND range",
        6 => "Invalid opcode",
        7 => "Device unavailable",
        8 => "Double fault",
        9 => "Coprocessor overrun",
        10 => "Invalid TSS",
        11 => "Segment not present",
        12 => "Stack-segment fault",
        13 => "General protection fault",
        14 => "Page fault",
        15 => "Reserved",
        16 => "x87 floating-point",
        17 => "Alignment check",
        18 => "Machine check",
        19 => "SIMD floating-point",
        20 => "Virtualization",
        21 => "Control protection",
        22..=27 | 31 => "Reserved",
        28 => "Hypervisor injection",
        29 => "VMM communication",
        30 => "Security exception",
        _ => "Unknown exception",
    }
}

fn exception_stop_code(vector: u64) -> &'static str {
    match vector {
        0 => "DIVIDE_ERROR",
        6 => "INVALID_OPCODE",
        8 => "DOUBLE_FAULT",
        10 => "INVALID_TSS",
        11 => "SEGMENT_NOT_PRESENT",
        12 => "STACK_SEGMENT_FAULT",
        13 => "GENERAL_PROTECTION_FAULT",
        14 => "PAGE_FAULT",
        17 => "ALIGNMENT_CHECK",
        18 => "MACHINE_CHECK",
        _ => "CPU_EXCEPTION",
    }
}

#[inline(never)]
fn write_stack_trace(output: &mut PanicWriter, instruction_pointer: Option<u64>) {
    const MAX_FRAMES: usize = 6;
    const MAX_STACK_BYTES: u64 = 64 * 1024;
    let _ = output.write_str("Stack trace:\n");
    let mut count = 0;

    if let Some(address) = instruction_pointer {
        let _ = output.write_str("  #0 ");
        write_hex(output, address);
        let _ = output.write_str("\n");
        count = 1;
    }

    let frame_pointer: u64;
    let stack_pointer: u64;
    unsafe {
        asm!("mov {}, rbp", out(reg) frame_pointer, options(nomem, nostack));
        asm!("mov {}, rsp", out(reg) stack_pointer, options(nomem, nostack));
    }
    let stack_end = stack_pointer.saturating_add(MAX_STACK_BYTES);
    let mut frame = frame_pointer;

    while count < MAX_FRAMES {
        if frame < stack_pointer || frame.saturating_add(16) > stack_end || frame & 7 != 0 {
            break;
        }

        let previous = unsafe { ptr::read_volatile(frame as *const u64) };
        let return_address = unsafe { ptr::read_volatile((frame + 8) as *const u64) };
        if return_address == 0 {
            break;
        }

        let _ = output.write_str("  #");
        write_decimal(output, count as u64);
        let _ = output.write_str(" ");
        write_hex(output, return_address);
        let _ = output.write_str("\n");
        count += 1;

        if previous <= frame {
            break;
        }
        frame = previous;
    }

    if count == 0 {
        let _ = output.write_str("  <frame chain unavailable>\n");
    }
}

fn write_page_fault_details(output: &mut PanicWriter, error_code: u64) {
    let fault_address: u64;
    unsafe {
        asm!("mov {}, cr2", out(reg) fault_address, options(nomem, nostack));
    }

    let _ = output.write_str("Fault address: ");
    write_hex(output, fault_address);
    let _ = output.write_str("\nFault access: ");
    let _ = output.write_str(if error_code & 1 != 0 {
        "protection violation"
    } else {
        "not-present page"
    });
    let _ = output.write_str(", ");
    let _ = output.write_str(if error_code & 2 != 0 { "write" } else { "read" });
    let _ = output.write_str(", ");
    let _ = output.write_str(if error_code & 4 != 0 {
        "user mode"
    } else {
        "supervisor mode"
    });
    if error_code & (1 << 3) != 0 {
        let _ = output.write_str(", reserved-bit violation");
    }
    if error_code & (1 << 4) != 0 {
        let _ = output.write_str(", instruction fetch");
    }
    if error_code & (1 << 5) != 0 {
        let _ = output.write_str(", protection-key violation");
    }
    if error_code & (1 << 6) != 0 {
        let _ = output.write_str(", shadow-stack access");
    }
    if error_code & (1 << 15) != 0 {
        let _ = output.write_str(", SGX access");
    }
    let _ = output.write_str("\n");
}

fn halt_cpu() -> ! {
    unsafe {
        asm!("cli", options(nomem, nostack));
        loop {
            asm!("hlt", options(nomem, nostack));
        }
    }
}

#[no_mangle]
pub extern "C" fn kernel_panic(message: *const c_char) -> ! {
    let mut output = begin_panic();
    let _ = output.write_str("*** KERNEL PANIC ***\nSTOP CODE: KERNEL_PANIC\nReason: ");
    output.write_c_string(message);
    let _ = output.write_str("\n");
    write_stack_trace(&mut output, None);
    let _ = output.write_str("System halted.\n");
    finish_panic(&output)
}

#[no_mangle]
pub extern "C" fn kernel_panic_exception(
    vector: u64,
    error_code: u64,
    instruction_pointer: u64,
) -> ! {
    let mut output = begin_panic();
    let _ = output.write_str("*** KERNEL PANIC ***\nSTOP CODE: ");
    let _ = output.write_str(exception_stop_code(vector));
    let _ = output.write_str("\nReason: ");
    let _ = output.write_str(exception_name(vector));
    let _ = output.write_str("\nVector: ");
    write_decimal(&mut output, vector);
    let _ = output.write_str("\nError code: ");
    write_hex(&mut output, error_code);
    let _ = output.write_str("\nInstruction pointer: ");
    write_hex(&mut output, instruction_pointer);
    let _ = output.write_str("\n");

    if vector == 14 {
        write_page_fault_details(&mut output, error_code);
    }
    write_stack_trace(&mut output, Some(instruction_pointer));
    let _ = output.write_str("System halted.\n");
    finish_panic(&output)
}

pub(crate) fn security_write_protection_panic(
    fault_address: u64,
    error_code: u64,
    instruction_pointer: u64,
) -> ! {
    let mut output = begin_panic();
    let _ = output.write_str(
        "*** SECURITY PANIC ***\n\
         STOP CODE: KERNEL_WRITE_PROTECTION\n\
         Reason: The CPU blocked a supervisor-mode write to a read-only page.\n\
         Fault address: ",
    );
    write_hex(&mut output, fault_address);
    let _ = output.write_str("\nInstruction pointer: ");
    write_hex(&mut output, instruction_pointer);
    let _ = output.write_str("\nError code: ");
    write_hex(&mut output, error_code);
    let _ = output
        .write_str("\nPossible cause: a kernel/driver bug or an attempted memory modification.\n");
    write_stack_trace(&mut output, Some(instruction_pointer));
    let _ = output.write_str(
        "Tips:\n\
         - Record the stop code, addresses, and stack trace.\n\
         - Undo recent kernel or driver changes and boot a known-good build.\n\
         - Check serial logs before retrying; do not repeatedly boot the same build.\n\
         System halted.\n",
    );
    finish_panic(&output)
}

#[panic_handler]
fn rust_panic(info: &PanicInfo<'_>) -> ! {
    let mut output = begin_panic();
    let _ = output.write_str("*** RUST PANIC ***\nSTOP CODE: RUST_PANIC\n");
    if let Some(location) = info.location() {
        let _ = write!(
            output,
            "Location: {}:{}:{}\n",
            location.file(),
            location.line(),
            location.column()
        );
    }
    let _ = write!(output, "Reason: {}\n", info.message());
    write_stack_trace(&mut output, None);
    let _ = output.write_str("System halted.\n");
    finish_panic(&output)
}
