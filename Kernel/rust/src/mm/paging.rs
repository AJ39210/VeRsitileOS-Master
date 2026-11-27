use core::arch::asm;
use core::ffi::c_void;
use core::ptr;

const PAGE_SIZE: u64 = 4096;
const PAGE_MASK: u64 = 0x000F_FFFF_FFFF_F000;
const IOREMAP_BASE: u64 = 0xFFFF_C000_0000_0000;
const IOREMAP_LIMIT: u64 = 0xFFFF_C100_0000_0000;
const EFER_MSR: u32 = 0xC000_0080;
const EFER_NXE: u64 = 1 << 11;
const VMM_PRESENT: u64 = 1;
const VMM_WRITABLE: u64 = 1 << 1;
const VMM_WRITE_THROUGH: u64 = 1 << 3;
const VMM_CACHE_DISABLE: u64 = 1 << 4;

static mut HHDM_OFFSET: u64 = 0;
static mut ACTIVE_PML4: *mut u64 = ptr::null_mut();
static mut NEXT_IOREMAP: u64 = IOREMAP_BASE;
static mut NX_SUPPORTED: bool = false;

unsafe fn read_msr(msr: u32) -> u64 {
    let low: u32;
    let high: u32;
    asm!(
        "rdmsr",
        in("ecx") msr,
        out("eax") low,
        out("edx") high,
        options(nomem, nostack)
    );
    ((high as u64) << 32) | low as u64
}

unsafe fn write_msr(msr: u32, value: u64) {
    asm!(
        "wrmsr",
        in("ecx") msr,
        in("eax") value as u32,
        in("edx") (value >> 32) as u32,
        options(nomem, nostack)
    );
}

unsafe fn initialize_nx() {
    let max_extended: u32;
    asm!(
        "push rbx",
        "cpuid",
        "pop rbx",
        inout("eax") 0x8000_0000u32 => max_extended,
        in("ecx") 0u32,
        lateout("edx") _,
    );
    if max_extended < 0x8000_0001 {
        return;
    }

    let features: u32;
    asm!(
        "push rbx",
        "cpuid",
        "pop rbx",
        inout("eax") 0x8000_0001u32 => _,
        in("ecx") 0u32,
        lateout("edx") features,
    );
    if features & (1 << 20) == 0 {
        return;
    }

    write_msr(EFER_MSR, read_msr(EFER_MSR) | EFER_NXE);
    NX_SUPPORTED = true;
}

#[no_mangle]
pub unsafe extern "C" fn paging_init(offset: u64) -> bool {
    HHDM_OFFSET = offset;

    let cr3: u64;
    asm!("mov {}, cr3", out(reg) cr3, options(nomem, nostack));
    let root_physical = cr3 & PAGE_MASK;
    if root_physical == 0 {
        return false;
    }

    ACTIVE_PML4 = paging_phys_to_virt(root_physical).cast();
    if ACTIVE_PML4.is_null() {
        return false;
    }

    NEXT_IOREMAP = IOREMAP_BASE;
    initialize_nx();
    true
}

#[no_mangle]
pub extern "C" fn paging_get_hhdm_offset() -> u64 {
    unsafe { HHDM_OFFSET }
}

#[no_mangle]
pub extern "C" fn paging_phys_to_virt(physical_address: u64) -> *mut c_void {
    unsafe {
        match physical_address.checked_add(HHDM_OFFSET) {
            Some(address) => address as *mut c_void,
            None => ptr::null_mut(),
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn paging_invlpg(virtual_address: u64) {
    asm!(
        "invlpg [{}]",
        in(reg) virtual_address,
        options(nostack, preserves_flags)
    );
}

#[no_mangle]
pub extern "C" fn paging_nx_supported() -> bool {
    unsafe { NX_SUPPORTED }
}

#[no_mangle]
pub unsafe extern "C" fn ioremap(physical_address: u64, size: u64) -> *mut c_void {
    if ACTIVE_PML4.is_null() || size == 0 || physical_address.checked_add(size).is_none() {
        return ptr::null_mut();
    }

    let offset = physical_address & (PAGE_SIZE - 1);
    let physical_start = physical_address & !(PAGE_SIZE - 1);
    let Some(total_size) = size.checked_add(offset) else {
        return ptr::null_mut();
    };
    let Some(rounded_size) = total_size.checked_add(PAGE_SIZE - 1) else {
        return ptr::null_mut();
    };
    let mapped_size = rounded_size & !(PAGE_SIZE - 1);

    if NEXT_IOREMAP > IOREMAP_LIMIT || mapped_size > IOREMAP_LIMIT - NEXT_IOREMAP {
        return ptr::null_mut();
    }

    let virtual_start = NEXT_IOREMAP;
    NEXT_IOREMAP += mapped_size;

    for page_offset in (0..mapped_size).step_by(PAGE_SIZE as usize) {
        let Some(physical) = physical_start.checked_add(page_offset) else {
            return ptr::null_mut();
        };
        if !super::vmm::vmm_map_page(
            virtual_start + page_offset,
            physical,
            VMM_PRESENT | VMM_WRITABLE | VMM_WRITE_THROUGH | VMM_CACHE_DISABLE,
        ) {
            return ptr::null_mut();
        }
    }

    (virtual_start + offset) as *mut c_void
}
