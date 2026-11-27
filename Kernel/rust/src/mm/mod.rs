pub mod heap;
pub mod mm;
pub mod paging;
pub mod pmm;
pub mod region;
pub mod spinlock;
pub mod vmm;

#[repr(C)]
pub struct LimineMemmapEntry {
    pub base: u64,
    pub length: u64,
    pub kind: u64,
}

#[repr(C)]
pub struct LimineMemmapResponse {
    pub revision: u64,
    pub entry_count: u64,
    pub entries: *mut *mut LimineMemmapEntry,
}
