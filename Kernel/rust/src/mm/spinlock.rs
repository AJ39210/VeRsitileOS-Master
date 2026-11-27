use core::arch::asm;
use core::sync::atomic::{AtomicU32, Ordering};

#[repr(C)]
pub struct Spinlock {
    pub(crate) value: u32,
}

#[no_mangle]
pub unsafe extern "C" fn spinlock_init(lock: *mut Spinlock) {
    if !lock.is_null() {
        core::ptr::addr_of_mut!((*lock).value).write(0);
    }
}

#[no_mangle]
pub unsafe extern "C" fn spinlock_lock_irqsave(lock: *mut Spinlock) -> u64 {
    let flags: u64;
    asm!(
        "pushfq",
        "pop {}",
        "cli",
        out(reg) flags,
        options(nomem)
    );

    let value = &*core::ptr::addr_of!((*lock).value).cast::<AtomicU32>();
    while value
        .compare_exchange_weak(0, 1, Ordering::Acquire, Ordering::Relaxed)
        .is_err()
    {
        while value.load(Ordering::Relaxed) != 0 {
            asm!("pause", options(nomem, nostack));
        }
    }

    flags
}

#[no_mangle]
pub unsafe extern "C" fn spinlock_unlock_irqrestore(lock: *mut Spinlock, flags: u64) {
    let value = &*core::ptr::addr_of!((*lock).value).cast::<AtomicU32>();
    value.store(0, Ordering::Release);
    if flags & (1 << 9) != 0 {
        asm!("sti", options(nomem, nostack));
    }
}
