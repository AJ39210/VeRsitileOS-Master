use core::ptr;

use super::super::spinlock::{
    spinlock_init, spinlock_lock_irqsave, spinlock_unlock_irqrestore, Spinlock,
};

const PAGE_SIZE: u64 = 4096;
const REGION_BASE: u64 = 0xFFFF_C100_0000_0000;
const REGION_LIMIT: u64 = 0xFFFF_C200_0000_0000;
const REGION_PAGES: usize = ((REGION_LIMIT - REGION_BASE) / PAGE_SIZE) as usize;
const BITMAP_WORDS: usize = REGION_PAGES / 64;

static mut REGION_BITMAP: [u64; BITMAP_WORDS] = [0; BITMAP_WORDS];
static mut REGION_LOCK: Spinlock = Spinlock { value: 0 };
static mut INITIALIZED: bool = false;

unsafe fn bit_test(page: u64) -> bool {
    let bitmap = ptr::addr_of!(REGION_BITMAP).cast::<u64>();
    bitmap.add((page / 64) as usize).read() & (1u64 << (page % 64)) != 0
}

unsafe fn bit_set(page: u64) {
    let bitmap = ptr::addr_of_mut!(REGION_BITMAP).cast::<u64>();
    let word = bitmap.add((page / 64) as usize);
    word.write(word.read() | (1u64 << (page % 64)));
}

unsafe fn bit_clear(page: u64) {
    let bitmap = ptr::addr_of_mut!(REGION_BITMAP).cast::<u64>();
    let word = bitmap.add((page / 64) as usize);
    word.write(word.read() & !(1u64 << (page % 64)));
}

fn power_of_two(value: u64) -> bool {
    value != 0 && value & (value - 1) == 0
}

#[no_mangle]
pub unsafe extern "C" fn region_init() -> bool {
    spinlock_init(ptr::addr_of_mut!(REGION_LOCK));
    ptr::write_bytes(
        ptr::addr_of_mut!(REGION_BITMAP).cast::<u64>(),
        0,
        BITMAP_WORDS,
    );
    INITIALIZED = true;
    true
}

#[no_mangle]
pub unsafe extern "C" fn region_alloc(size: u64, mut alignment: u64) -> u64 {
    if !INITIALIZED || size == 0 {
        return 0;
    }
    if alignment < PAGE_SIZE {
        alignment = PAGE_SIZE;
    }
    if !power_of_two(alignment) {
        return 0;
    }
    let Some(rounded_size) = size.checked_add(PAGE_SIZE - 1) else {
        return 0;
    };
    let pages = (rounded_size & !(PAGE_SIZE - 1)) / PAGE_SIZE;
    if pages > REGION_PAGES as u64 {
        return 0;
    }

    let flags = spinlock_lock_irqsave(ptr::addr_of_mut!(REGION_LOCK));
    let mut index = 0;
    while index + pages <= REGION_PAGES as u64 {
        let address = REGION_BASE + index * PAGE_SIZE;
        if address & (alignment - 1) != 0 {
            index += 1;
            continue;
        }
        let mut run = 0;
        while run < pages && !bit_test(index + run) {
            run += 1;
        }
        if run == pages {
            for page in 0..pages {
                bit_set(index + page);
            }
            spinlock_unlock_irqrestore(ptr::addr_of_mut!(REGION_LOCK), flags);
            return address;
        }
        index += run + 1;
    }
    spinlock_unlock_irqrestore(ptr::addr_of_mut!(REGION_LOCK), flags);
    0
}

#[no_mangle]
pub unsafe extern "C" fn region_free(address: u64, size: u64) -> bool {
    if !INITIALIZED
        || size == 0
        || address & (PAGE_SIZE - 1) != 0
        || !(REGION_BASE..REGION_LIMIT).contains(&address)
    {
        return false;
    }
    let Some(rounded_size) = size.checked_add(PAGE_SIZE - 1) else {
        return false;
    };
    let pages = (rounded_size & !(PAGE_SIZE - 1)) / PAGE_SIZE;
    let first_page = (address - REGION_BASE) / PAGE_SIZE;
    if pages > REGION_PAGES as u64 || first_page > REGION_PAGES as u64 - pages {
        return false;
    }

    let flags = spinlock_lock_irqsave(ptr::addr_of_mut!(REGION_LOCK));
    for index in 0..pages {
        if !bit_test(first_page + index) {
            spinlock_unlock_irqrestore(ptr::addr_of_mut!(REGION_LOCK), flags);
            return false;
        }
    }
    for index in 0..pages {
        bit_clear(first_page + index);
    }
    spinlock_unlock_irqrestore(ptr::addr_of_mut!(REGION_LOCK), flags);
    true
}

#[no_mangle]
pub unsafe extern "C" fn region_is_reserved(address: u64) -> bool {
    if !INITIALIZED || !(REGION_BASE..REGION_LIMIT).contains(&address) {
        return false;
    }
    let page = (address - REGION_BASE) / PAGE_SIZE;
    let flags = spinlock_lock_irqsave(ptr::addr_of_mut!(REGION_LOCK));
    let result = bit_test(page);
    spinlock_unlock_irqrestore(ptr::addr_of_mut!(REGION_LOCK), flags);
    result
}
