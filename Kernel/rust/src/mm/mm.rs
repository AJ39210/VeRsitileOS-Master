use super::LimineMemmapResponse;

extern "C" {
    fn paging_init(hhdm_offset: u64) -> bool;
    fn pmm_init(memmap: *mut LimineMemmapResponse, hhdm_offset: u64) -> bool;
    fn vmm_init() -> bool;
    fn region_init() -> bool;
    fn kheap_init() -> bool;
}

#[no_mangle]
pub unsafe extern "C" fn mm_init(memmap: *mut LimineMemmapResponse, hhdm_offset: u64) -> bool {
    if memmap.is_null() || !paging_init(hhdm_offset) {
        return false;
    }
    if !pmm_init(memmap, hhdm_offset) {
        return false;
    }
    if !vmm_init() {
        return false;
    }
    if !region_init() {
        return false;
    }
    kheap_init()
}
