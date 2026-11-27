use core::ffi::c_void;
use core::mem::size_of;
use core::ptr;

use super::super::paging::paging_nx_supported;
use super::super::pmm::{pmm_alloc_page, pmm_free_page, pmm_is_allocated_page};
use super::super::region::{region_alloc, region_free};
use super::super::spinlock::{
    spinlock_init, spinlock_lock_irqsave, spinlock_unlock_irqrestore, Spinlock,
};
use super::super::vmm::{
    vmm_map_page, vmm_translate, vmm_unmap_page, VMM_NO_EXECUTE, VMM_PRESENT, VMM_WRITABLE,
};

const PAGE_SIZE: u64 = 4096;
const HEAP_MAX_SIZE: u64 = 64 * 1024 * 1024;
const HEAP_MAX_PAGES: u64 = HEAP_MAX_SIZE / PAGE_SIZE;
const HEAP_ALIGNMENT: u64 = 16;
const BLOCK_MAGIC: u64 = 0x4845_4150_424C_4F43;
const TAG_MAGIC: u64 = 0x4845_4150_414C_4C4F;

#[repr(C, align(16))]
struct HeapBlock {
    size: u64,
    magic: u64,
    prev: *mut HeapBlock,
    next: *mut HeapBlock,
    free: u64,
}

#[repr(C)]
struct HeapTag {
    magic: u64,
    block: *mut HeapBlock,
    requested: u64,
    alignment: u64,
}

const BLOCK_HEADER_SIZE: u64 = size_of::<HeapBlock>() as u64;
const TAG_SIZE: u64 = size_of::<HeapTag>() as u64;
const MIN_BLOCK_SIZE: u64 = (BLOCK_HEADER_SIZE + TAG_SIZE + HEAP_ALIGNMENT + 15) & !15;

static mut HEAP_BASE: u64 = 0;
static mut HEAP_PAGES: u64 = 0;
static mut FIRST_BLOCK: *mut HeapBlock = ptr::null_mut();
static mut LAST_BLOCK: *mut HeapBlock = ptr::null_mut();
static mut HEAP_LOCK: Spinlock = Spinlock { value: 0 };
static mut INITIALIZED: bool = false;

fn power_of_two(value: u64) -> bool {
    value != 0 && value & (value - 1) == 0
}

fn align_up_pointer(value: usize, alignment: u64) -> usize {
    value
        .checked_add((alignment - 1) as usize)
        .map(|value| value & !((alignment - 1) as usize))
        .unwrap_or(0)
}

fn align_up_size(value: u64) -> u64 {
    value.checked_add(15).map(|value| value & !15).unwrap_or(0)
}

unsafe fn split_block(block: *mut HeapBlock, required_size: u64) {
    if (*block).size < required_size {
        return;
    }
    let remaining = (*block).size - required_size;
    if remaining < MIN_BLOCK_SIZE {
        return;
    }
    let new_block = (block as usize + required_size as usize) as *mut HeapBlock;
    (*new_block).size = remaining;
    (*new_block).magic = BLOCK_MAGIC;
    (*new_block).prev = block;
    (*new_block).next = (*block).next;
    (*new_block).free = 1;
    if !(*new_block).next.is_null() {
        (*(*new_block).next).prev = new_block;
    } else {
        LAST_BLOCK = new_block;
    }
    (*block).next = new_block;
    (*block).size = required_size;
}

unsafe fn find_fit(size: u64, alignment: u64) -> (*mut HeapBlock, usize, u64) {
    let mut block = FIRST_BLOCK;
    while !block.is_null() {
        if (*block).free != 0 {
            let start = block as usize;
            if start <= usize::MAX - BLOCK_HEADER_SIZE as usize - TAG_SIZE as usize {
                let raw = start + BLOCK_HEADER_SIZE as usize + TAG_SIZE as usize;
                let user = align_up_pointer(raw, alignment);
                if user != 0 && user <= usize::MAX - size as usize {
                    let end = user + size as usize;
                    let required = align_up_size((end - start) as u64);
                    if required != 0 && required <= (*block).size {
                        return (block, user, required);
                    }
                }
            }
        }
        block = (*block).next;
    }
    (ptr::null_mut(), 0, 0)
}

unsafe fn heap_grow() -> bool {
    if HEAP_PAGES >= HEAP_MAX_PAGES {
        return false;
    }
    let physical = pmm_alloc_page();
    if physical == 0 {
        return false;
    }
    let virtual_address = HEAP_BASE + HEAP_PAGES * PAGE_SIZE;
    let mut flags = VMM_PRESENT | VMM_WRITABLE;
    if paging_nx_supported() {
        flags |= VMM_NO_EXECUTE;
    }
    if !vmm_map_page(virtual_address, physical, flags) {
        pmm_free_page(physical);
        return false;
    }

    ptr::write_bytes(virtual_address as *mut u8, 0, PAGE_SIZE as usize);
    HEAP_PAGES += 1;
    if !LAST_BLOCK.is_null()
        && (*LAST_BLOCK).free != 0
        && LAST_BLOCK as usize + (*LAST_BLOCK).size as usize == virtual_address as usize
    {
        (*LAST_BLOCK).size += PAGE_SIZE;
    } else {
        let block = virtual_address as *mut HeapBlock;
        (*block).size = PAGE_SIZE;
        (*block).magic = BLOCK_MAGIC;
        (*block).prev = LAST_BLOCK;
        (*block).next = ptr::null_mut();
        (*block).free = 1;
        if !LAST_BLOCK.is_null() {
            (*LAST_BLOCK).next = block;
        } else {
            FIRST_BLOCK = block;
        }
        LAST_BLOCK = block;
    }
    true
}

unsafe fn heap_alloc_locked(size: u64, alignment: u64) -> *mut c_void {
    if !INITIALIZED || size == 0 || !power_of_two(alignment) || alignment < HEAP_ALIGNMENT {
        return ptr::null_mut();
    }
    loop {
        let (block, user, required) = find_fit(size, alignment);
        if !block.is_null() {
            split_block(block, required);
            (*block).free = 0;
            let tag = (user - TAG_SIZE as usize) as *mut HeapTag;
            (*tag).magic = TAG_MAGIC;
            (*tag).block = block;
            (*tag).requested = size;
            (*tag).alignment = alignment;
            return user as *mut c_void;
        }
        if !heap_grow() {
            return ptr::null_mut();
        }
    }
}

unsafe fn heap_free_locked(pointer: *mut c_void) -> bool {
    if pointer.is_null() {
        return true;
    }
    let address = pointer as usize;
    let heap_end = HEAP_BASE + HEAP_PAGES * PAGE_SIZE;
    if address < HEAP_BASE as usize + BLOCK_HEADER_SIZE as usize + TAG_SIZE as usize
        || address >= heap_end as usize
    {
        return false;
    }
    let tag = (address - TAG_SIZE as usize) as *mut HeapTag;
    if (*tag).magic != TAG_MAGIC {
        return false;
    }
    let mut block = (*tag).block;
    if block.is_null()
        || (block as usize) < HEAP_BASE as usize
        || (block as usize) >= heap_end as usize
        || (*block).magic != BLOCK_MAGIC
        || (*block).free != 0
    {
        return false;
    }
    if address < block as usize + BLOCK_HEADER_SIZE as usize + TAG_SIZE as usize
        || address >= block as usize + (*block).size as usize
    {
        return false;
    }

    (*tag).magic = 0;
    (*block).free = 1;

    let previous = (*block).prev;
    if !previous.is_null()
        && (*previous).free != 0
        && previous as usize + (*previous).size as usize == block as usize
    {
        (*previous).size += (*block).size;
        (*previous).next = (*block).next;
        if !(*block).next.is_null() {
            (*(*block).next).prev = previous;
        } else {
            LAST_BLOCK = previous;
        }
        block = previous;
    }

    let next = (*block).next;
    if !next.is_null()
        && (*next).free != 0
        && block as usize + (*block).size as usize == next as usize
    {
        (*block).size += (*next).size;
        (*block).next = (*next).next;
        if !(*next).next.is_null() {
            (*(*next).next).prev = block;
        } else {
            LAST_BLOCK = block;
        }
    }
    true
}

unsafe fn heap_trim() {
    let block = LAST_BLOCK;
    if block.is_null() || (*block).free == 0 {
        return;
    }
    while HEAP_PAGES > 1 && (*block).size >= PAGE_SIZE + MIN_BLOCK_SIZE {
        let virtual_address = HEAP_BASE + (HEAP_PAGES - 1) * PAGE_SIZE;
        let physical = vmm_translate(virtual_address);
        if physical == 0 || !pmm_is_allocated_page(physical) {
            break;
        }
        if !vmm_unmap_page(virtual_address) {
            break;
        }
        if !pmm_free_page(physical) {
            break;
        }
        (*block).size -= PAGE_SIZE;
        HEAP_PAGES -= 1;
    }
}

#[no_mangle]
pub unsafe extern "C" fn kheap_init() -> bool {
    if INITIALIZED {
        return true;
    }
    spinlock_init(ptr::addr_of_mut!(HEAP_LOCK));
    HEAP_BASE = region_alloc(HEAP_MAX_SIZE, PAGE_SIZE);
    if HEAP_BASE == 0 {
        return false;
    }
    let physical = pmm_alloc_page();
    if physical == 0 {
        region_free(HEAP_BASE, HEAP_MAX_SIZE);
        HEAP_BASE = 0;
        return false;
    }
    let mut flags = VMM_PRESENT | VMM_WRITABLE;
    if paging_nx_supported() {
        flags |= VMM_NO_EXECUTE;
    }
    if !vmm_map_page(HEAP_BASE, physical, flags) {
        pmm_free_page(physical);
        region_free(HEAP_BASE, HEAP_MAX_SIZE);
        HEAP_BASE = 0;
        return false;
    }
    ptr::write_bytes(HEAP_BASE as *mut u8, 0, PAGE_SIZE as usize);
    FIRST_BLOCK = HEAP_BASE as *mut HeapBlock;
    (*FIRST_BLOCK).size = PAGE_SIZE;
    (*FIRST_BLOCK).magic = BLOCK_MAGIC;
    (*FIRST_BLOCK).prev = ptr::null_mut();
    (*FIRST_BLOCK).next = ptr::null_mut();
    (*FIRST_BLOCK).free = 1;
    LAST_BLOCK = FIRST_BLOCK;
    HEAP_PAGES = 1;
    INITIALIZED = true;
    true
}

#[no_mangle]
pub unsafe extern "C" fn kmalloc(size: u64) -> *mut c_void {
    let flags = spinlock_lock_irqsave(ptr::addr_of_mut!(HEAP_LOCK));
    let result = heap_alloc_locked(size, HEAP_ALIGNMENT);
    spinlock_unlock_irqrestore(ptr::addr_of_mut!(HEAP_LOCK), flags);
    result
}

#[no_mangle]
pub unsafe extern "C" fn kmemalign(alignment: u64, size: u64) -> *mut c_void {
    let flags = spinlock_lock_irqsave(ptr::addr_of_mut!(HEAP_LOCK));
    let result = heap_alloc_locked(size, alignment);
    spinlock_unlock_irqrestore(ptr::addr_of_mut!(HEAP_LOCK), flags);
    result
}

#[no_mangle]
pub unsafe extern "C" fn kcalloc(count: u64, size: u64) -> *mut c_void {
    if count != 0 && size > u64::MAX / count {
        return ptr::null_mut();
    }
    let total = count * size;
    let result = kmalloc(total);
    if !result.is_null() {
        ptr::write_bytes(result.cast::<u8>(), 0, total as usize);
    }
    result
}

#[no_mangle]
pub unsafe extern "C" fn kfree(pointer: *mut c_void) {
    if pointer.is_null() || !INITIALIZED {
        return;
    }
    let flags = spinlock_lock_irqsave(ptr::addr_of_mut!(HEAP_LOCK));
    if heap_free_locked(pointer) {
        heap_trim();
    }
    spinlock_unlock_irqrestore(ptr::addr_of_mut!(HEAP_LOCK), flags);
}

#[no_mangle]
pub unsafe extern "C" fn krealloc(pointer: *mut c_void, new_size: u64) -> *mut c_void {
    if pointer.is_null() {
        return kmalloc(new_size);
    }
    if new_size == 0 {
        kfree(pointer);
        return ptr::null_mut();
    }

    let flags = spinlock_lock_irqsave(ptr::addr_of_mut!(HEAP_LOCK));
    let address = pointer as usize;
    if address < HEAP_BASE as usize + BLOCK_HEADER_SIZE as usize + TAG_SIZE as usize
        || address >= (HEAP_BASE + HEAP_PAGES * PAGE_SIZE) as usize
    {
        spinlock_unlock_irqrestore(ptr::addr_of_mut!(HEAP_LOCK), flags);
        return ptr::null_mut();
    }
    let tag = (address - TAG_SIZE as usize) as *mut HeapTag;
    if (*tag).magic != TAG_MAGIC {
        spinlock_unlock_irqrestore(ptr::addr_of_mut!(HEAP_LOCK), flags);
        return ptr::null_mut();
    }
    let old_size = (*tag).requested;
    let alignment = (*tag).alignment;
    let new_pointer = heap_alloc_locked(new_size, alignment);
    if new_pointer.is_null() {
        spinlock_unlock_irqrestore(ptr::addr_of_mut!(HEAP_LOCK), flags);
        return ptr::null_mut();
    }
    ptr::copy_nonoverlapping(
        pointer.cast::<u8>(),
        new_pointer.cast::<u8>(),
        old_size.min(new_size) as usize,
    );
    heap_free_locked(pointer);
    heap_trim();
    spinlock_unlock_irqrestore(ptr::addr_of_mut!(HEAP_LOCK), flags);
    new_pointer
}

unsafe fn heap_statistic(select: u8) -> u64 {
    if !INITIALIZED {
        return 0;
    }
    let flags = spinlock_lock_irqsave(ptr::addr_of_mut!(HEAP_LOCK));
    let mut result = 0;
    if select == 2 {
        result = HEAP_PAGES * PAGE_SIZE;
    } else {
        let mut block = FIRST_BLOCK;
        while !block.is_null() {
            if (select == 0 && (*block).free == 0) || (select == 1 && (*block).free != 0) {
                result += (*block).size;
            }
            block = (*block).next;
        }
    }
    spinlock_unlock_irqrestore(ptr::addr_of_mut!(HEAP_LOCK), flags);
    result
}

#[no_mangle]
pub unsafe extern "C" fn kheap_get_used() -> u64 {
    heap_statistic(0)
}

#[no_mangle]
pub unsafe extern "C" fn kheap_get_free() -> u64 {
    heap_statistic(1)
}

#[no_mangle]
pub unsafe extern "C" fn kheap_get_committed() -> u64 {
    heap_statistic(2)
}
