use core::ptr;

use super::paging::paging_phys_to_virt;
use super::spinlock::{spinlock_init, spinlock_lock_irqsave, spinlock_unlock_irqrestore, Spinlock};
use super::LimineMemmapResponse;

const PAGE_SIZE: u64 = 4096;
const LIMINE_MEMMAP_USABLE: u64 = 0;

static mut USED_BITMAP: *mut u64 = ptr::null_mut();
static mut ALLOCATED_BITMAP: *mut u64 = ptr::null_mut();
static mut MAX_PAGES: u64 = 0;
static mut TOTAL_PAGES: u64 = 0;
static mut FREE_PAGES: u64 = 0;
static mut BITMAP_WORDS: u64 = 0;
static mut BITMAP_PHYSICAL: u64 = 0;
static mut BITMAP_STORAGE_PAGES: u64 = 0;
static mut SEARCH_HINT: u64 = 0;
static mut INITIALIZED: bool = false;
static mut PMM_LOCK: Spinlock = Spinlock { value: 0 };

unsafe fn bit_set(bitmap: *mut u64, index: u64) {
    let word = bitmap.add((index / 64) as usize);
    word.write(word.read() | (1u64 << (index % 64)));
}

unsafe fn bit_clear(bitmap: *mut u64, index: u64) {
    let word = bitmap.add((index / 64) as usize);
    word.write(word.read() & !(1u64 << (index % 64)));
}

unsafe fn bit_test(bitmap: *const u64, index: u64) -> bool {
    bitmap.add((index / 64) as usize).read() & (1u64 << (index % 64)) != 0
}

fn align_up(value: u64) -> u64 {
    value
        .checked_add(PAGE_SIZE - 1)
        .map(|value| value & !(PAGE_SIZE - 1))
        .unwrap_or(0)
}

fn align_down(value: u64) -> u64 {
    value & !(PAGE_SIZE - 1)
}

unsafe fn find_bitmap_storage(
    memmap: *mut LimineMemmapResponse,
    required_size: u64,
) -> Option<u64> {
    for index in 0..(*memmap).entry_count {
        let Some(entry) = (*(*memmap).entries.add(index as usize)).as_ref() else {
            continue;
        };
        if entry.kind != LIMINE_MEMMAP_USABLE {
            continue;
        }
        let Some(entry_end) = entry.base.checked_add(entry.length) else {
            continue;
        };
        let mut start = align_up(entry.base);
        let end = align_down(entry_end);
        if start < PAGE_SIZE {
            start = PAGE_SIZE;
        }
        if end >= start && end - start >= required_size {
            return Some(start);
        }
    }
    None
}

#[no_mangle]
pub unsafe extern "C" fn pmm_init(memmap: *mut LimineMemmapResponse, hhdm_offset: u64) -> bool {
    let _ = hhdm_offset;
    INITIALIZED = false;
    spinlock_init(ptr::addr_of_mut!(PMM_LOCK));

    if memmap.is_null() || (*memmap).entries.is_null() || (*memmap).entry_count == 0 {
        return false;
    }

    let mut highest_address = 0u64;
    for index in 0..(*memmap).entry_count {
        let Some(entry) = (*(*memmap).entries.add(index as usize)).as_ref() else {
            return false;
        };
        let Some(end) = entry.base.checked_add(entry.length) else {
            return false;
        };
        highest_address = highest_address.max(end);
    }
    if highest_address == 0 {
        return false;
    }

    MAX_PAGES = highest_address / PAGE_SIZE;
    if highest_address % PAGE_SIZE != 0 {
        MAX_PAGES += 1;
    }
    if MAX_PAGES == 0 {
        return false;
    }

    BITMAP_WORDS = MAX_PAGES / 64;
    if MAX_PAGES % 64 != 0 {
        BITMAP_WORDS += 1;
    }
    let Some(bitmap_bytes) = BITMAP_WORDS.checked_mul(core::mem::size_of::<u64>() as u64) else {
        return false;
    };
    let one_bitmap_size = align_up(bitmap_bytes);
    if one_bitmap_size == 0 {
        return false;
    }
    let Some(storage_size) = one_bitmap_size.checked_mul(2) else {
        return false;
    };
    let Some(bitmap_physical) = find_bitmap_storage(memmap, storage_size) else {
        return false;
    };

    BITMAP_PHYSICAL = bitmap_physical;
    BITMAP_STORAGE_PAGES = storage_size / PAGE_SIZE;
    USED_BITMAP = paging_phys_to_virt(bitmap_physical).cast();
    ALLOCATED_BITMAP = paging_phys_to_virt(bitmap_physical + one_bitmap_size).cast();
    if USED_BITMAP.is_null() || ALLOCATED_BITMAP.is_null() {
        return false;
    }

    for index in 0..BITMAP_WORDS as usize {
        USED_BITMAP.add(index).write(u64::MAX);
        ALLOCATED_BITMAP.add(index).write(0);
    }

    TOTAL_PAGES = 0;
    FREE_PAGES = 0;
    for index in 0..(*memmap).entry_count {
        let entry = &*(*(*memmap).entries.add(index as usize));
        if entry.kind != LIMINE_MEMMAP_USABLE {
            continue;
        }
        let mut start = align_up(entry.base);
        let end = align_down(entry.base + entry.length);
        if start < PAGE_SIZE {
            start = PAGE_SIZE;
        }
        if end <= start {
            continue;
        }
        let first_page = start / PAGE_SIZE;
        let end_page = end / PAGE_SIZE;
        for page in first_page..end_page.min(MAX_PAGES) {
            if bit_test(USED_BITMAP, page) {
                bit_clear(USED_BITMAP, page);
                TOTAL_PAGES += 1;
                FREE_PAGES += 1;
            }
        }
    }

    let first_bitmap_page = BITMAP_PHYSICAL / PAGE_SIZE;
    for index in 0..BITMAP_STORAGE_PAGES {
        let page = first_bitmap_page + index;
        if page < MAX_PAGES && !bit_test(USED_BITMAP, page) {
            bit_set(USED_BITMAP, page);
            FREE_PAGES -= 1;
        }
    }

    SEARCH_HINT = 1;
    INITIALIZED = true;
    true
}

#[no_mangle]
pub unsafe extern "C" fn pmm_alloc_page() -> u64 {
    let flags = spinlock_lock_irqsave(ptr::addr_of_mut!(PMM_LOCK));
    if !INITIALIZED || FREE_PAGES == 0 {
        spinlock_unlock_irqrestore(ptr::addr_of_mut!(PMM_LOCK), flags);
        return 0;
    }

    for pass in 0..2 {
        let start = if pass == 0 { SEARCH_HINT } else { 1 };
        let end = if pass == 0 { MAX_PAGES } else { SEARCH_HINT };
        for page in start..end {
            if !bit_test(USED_BITMAP, page) {
                bit_set(USED_BITMAP, page);
                bit_set(ALLOCATED_BITMAP, page);
                FREE_PAGES -= 1;
                SEARCH_HINT = page + 1;
                if SEARCH_HINT >= MAX_PAGES {
                    SEARCH_HINT = 1;
                }
                spinlock_unlock_irqrestore(ptr::addr_of_mut!(PMM_LOCK), flags);
                return page * PAGE_SIZE;
            }
        }
    }

    spinlock_unlock_irqrestore(ptr::addr_of_mut!(PMM_LOCK), flags);
    0
}

#[no_mangle]
pub unsafe extern "C" fn pmm_alloc_contiguous_pages(page_count: u64) -> u64 {
    if page_count == 0 {
        return 0;
    }
    let flags = spinlock_lock_irqsave(ptr::addr_of_mut!(PMM_LOCK));
    if !INITIALIZED || page_count > FREE_PAGES {
        spinlock_unlock_irqrestore(ptr::addr_of_mut!(PMM_LOCK), flags);
        return 0;
    }

    let mut allocation_start = 0;
    let mut candidate = 1;
    while candidate < MAX_PAGES && page_count <= MAX_PAGES - candidate {
        let mut run = 0;
        while run < page_count && !bit_test(USED_BITMAP, candidate + run) {
            run += 1;
        }
        if run == page_count {
            allocation_start = candidate;
            break;
        }
        candidate += run + 1;
    }

    if allocation_start != 0 {
        for index in 0..page_count {
            bit_set(USED_BITMAP, allocation_start + index);
            bit_set(ALLOCATED_BITMAP, allocation_start + index);
        }
        FREE_PAGES -= page_count;
        SEARCH_HINT = allocation_start + page_count;
        if SEARCH_HINT >= MAX_PAGES {
            SEARCH_HINT = 1;
        }
    }
    spinlock_unlock_irqrestore(ptr::addr_of_mut!(PMM_LOCK), flags);
    allocation_start * PAGE_SIZE
}

#[no_mangle]
pub unsafe extern "C" fn pmm_free_page(physical_address: u64) -> bool {
    if physical_address == 0 || physical_address & (PAGE_SIZE - 1) != 0 {
        return false;
    }
    let flags = spinlock_lock_irqsave(ptr::addr_of_mut!(PMM_LOCK));
    if !INITIALIZED {
        spinlock_unlock_irqrestore(ptr::addr_of_mut!(PMM_LOCK), flags);
        return false;
    }
    let page = physical_address / PAGE_SIZE;
    if page >= MAX_PAGES || !bit_test(ALLOCATED_BITMAP, page) {
        spinlock_unlock_irqrestore(ptr::addr_of_mut!(PMM_LOCK), flags);
        return false;
    }
    bit_clear(ALLOCATED_BITMAP, page);
    bit_clear(USED_BITMAP, page);
    FREE_PAGES += 1;
    if page < SEARCH_HINT {
        SEARCH_HINT = page;
    }
    spinlock_unlock_irqrestore(ptr::addr_of_mut!(PMM_LOCK), flags);
    true
}

#[no_mangle]
pub unsafe extern "C" fn pmm_is_allocated_page(physical_address: u64) -> bool {
    if physical_address == 0 || physical_address & (PAGE_SIZE - 1) != 0 {
        return false;
    }
    let flags = spinlock_lock_irqsave(ptr::addr_of_mut!(PMM_LOCK));
    let result = INITIALIZED
        && physical_address / PAGE_SIZE < MAX_PAGES
        && bit_test(ALLOCATED_BITMAP, physical_address / PAGE_SIZE);
    spinlock_unlock_irqrestore(ptr::addr_of_mut!(PMM_LOCK), flags);
    result
}

#[no_mangle]
pub unsafe extern "C" fn pmm_get_total_memory() -> u64 {
    let flags = spinlock_lock_irqsave(ptr::addr_of_mut!(PMM_LOCK));
    let result = TOTAL_PAGES * PAGE_SIZE;
    spinlock_unlock_irqrestore(ptr::addr_of_mut!(PMM_LOCK), flags);
    result
}

#[no_mangle]
pub unsafe extern "C" fn pmm_get_free_memory() -> u64 {
    let flags = spinlock_lock_irqsave(ptr::addr_of_mut!(PMM_LOCK));
    let result = FREE_PAGES * PAGE_SIZE;
    spinlock_unlock_irqrestore(ptr::addr_of_mut!(PMM_LOCK), flags);
    result
}

#[no_mangle]
pub unsafe extern "C" fn pmm_get_used_memory() -> u64 {
    let flags = spinlock_lock_irqsave(ptr::addr_of_mut!(PMM_LOCK));
    let result = (TOTAL_PAGES - FREE_PAGES) * PAGE_SIZE;
    spinlock_unlock_irqrestore(ptr::addr_of_mut!(PMM_LOCK), flags);
    result
}

#[no_mangle]
pub unsafe extern "C" fn pmm_get_total_pages() -> u64 {
    let flags = spinlock_lock_irqsave(ptr::addr_of_mut!(PMM_LOCK));
    let result = TOTAL_PAGES;
    spinlock_unlock_irqrestore(ptr::addr_of_mut!(PMM_LOCK), flags);
    result
}

#[no_mangle]
pub unsafe extern "C" fn pmm_get_free_pages() -> u64 {
    let flags = spinlock_lock_irqsave(ptr::addr_of_mut!(PMM_LOCK));
    let result = FREE_PAGES;
    spinlock_unlock_irqrestore(ptr::addr_of_mut!(PMM_LOCK), flags);
    result
}
