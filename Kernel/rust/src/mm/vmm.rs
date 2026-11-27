use core::arch::asm;
use core::ptr;

use super::paging::{
    paging_get_hhdm_offset, paging_invlpg, paging_nx_supported, paging_phys_to_virt,
};
use super::pmm::{pmm_alloc_page, pmm_free_page, pmm_is_allocated_page};
use super::spinlock::{spinlock_init, spinlock_lock_irqsave, spinlock_unlock_irqrestore, Spinlock};

const PAGE_MASK: u64 = 0x000F_FFFF_FFFF_F000;
const PAGE_HUGE: u64 = 1 << 7;
const ENTRY_ADDRESS: u64 = PAGE_MASK;
const VMM_PAGE_SIZE: u64 = 4096;
pub(super) const VMM_PRESENT: u64 = 1 << 0;
pub(super) const VMM_WRITABLE: u64 = 1 << 1;
const VMM_USER: u64 = 1 << 2;
const VMM_WRITE_THROUGH: u64 = 1 << 3;
const VMM_CACHE_DISABLE: u64 = 1 << 4;
const VMM_GLOBAL: u64 = 1 << 8;
pub(super) const VMM_NO_EXECUTE: u64 = 1 << 63;

pub type AddressSpace = u64;

static mut PML4: *mut u64 = ptr::null_mut();
static mut KERNEL_ROOT_PHYSICAL: u64 = 0;
static mut INITIALIZED: bool = false;
static mut VMM_LOCK: Spinlock = Spinlock { value: 0 };

unsafe fn canonical_address(address: u64) -> bool {
    let upper = address >> 47;
    upper == 0 || upper == 0x1_FFFF
}

unsafe fn table_pointer(entry: u64) -> *mut u64 {
    paging_phys_to_virt(entry & ENTRY_ADDRESS).cast()
}

unsafe fn table_physical(table: *mut u64) -> u64 {
    (table as u64).wrapping_sub(paging_get_hhdm_offset())
}

unsafe fn allocate_table() -> *mut u64 {
    let physical = pmm_alloc_page();
    if physical == 0 {
        return ptr::null_mut();
    }
    let table = paging_phys_to_virt(physical).cast::<u64>();
    if table.is_null() {
        pmm_free_page(physical);
        return ptr::null_mut();
    }
    ptr::write_bytes(table, 0, 512);
    table
}

unsafe fn get_next_table(parent: *mut u64, index: usize, create: bool, user: bool) -> *mut u64 {
    let entry = parent.add(index).read();
    if entry & VMM_PRESENT == 0 {
        if !create {
            return ptr::null_mut();
        }
        let new_table = allocate_table();
        if new_table.is_null() {
            return ptr::null_mut();
        }
        let mut new_entry = table_physical(new_table) | VMM_PRESENT | VMM_WRITABLE;
        if user {
            new_entry |= VMM_USER;
        }
        parent.add(index).write(new_entry);
        return new_table;
    }
    if entry & PAGE_HUGE != 0 {
        return ptr::null_mut();
    }
    let mut entry = entry | VMM_WRITABLE;
    if user {
        entry |= VMM_USER;
    }
    parent.add(index).write(entry);
    table_pointer(entry)
}

unsafe fn table_is_empty(table: *mut u64) -> bool {
    for index in 0..512 {
        if table.add(index).read() & VMM_PRESENT != 0 {
            return false;
        }
    }
    true
}

unsafe fn reclaim_table(parent: *mut u64, index: usize) {
    let entry = parent.add(index).read();
    if entry & VMM_PRESENT == 0 || entry & PAGE_HUGE != 0 {
        return;
    }
    let physical = entry & ENTRY_ADDRESS;
    let table = paging_phys_to_virt(physical).cast::<u64>();
    if table.is_null() || !table_is_empty(table) || !pmm_is_allocated_page(physical) {
        return;
    }
    parent.add(index).write(0);
    pmm_free_page(physical);
}

unsafe fn prune_empty_path(root: *mut u64, pml4_index: usize, pdpt_index: usize, pd_index: usize) {
    let pml4_entry = root.add(pml4_index).read();
    if pml4_entry & VMM_PRESENT == 0 || pml4_entry & PAGE_HUGE != 0 {
        return;
    }
    let pdpt = table_pointer(pml4_entry);
    if pdpt.is_null() {
        return;
    }
    let pdpt_entry = pdpt.add(pdpt_index).read();
    if pdpt_entry & VMM_PRESENT == 0 || pdpt_entry & PAGE_HUGE != 0 {
        reclaim_table(root, pml4_index);
        return;
    }
    let pd = table_pointer(pdpt_entry);
    if pd.is_null() {
        return;
    }
    let pd_entry = pd.add(pd_index).read();
    if pd_entry & VMM_PRESENT == 0 || pd_entry & PAGE_HUGE != 0 {
        reclaim_table(pdpt, pdpt_index);
        reclaim_table(root, pml4_index);
        return;
    }
    let pt = table_pointer(pd_entry);
    if pt.is_null() {
        return;
    }
    reclaim_table(pd, pd_index);
    reclaim_table(pdpt, pdpt_index);
    reclaim_table(root, pml4_index);
}

#[no_mangle]
pub unsafe extern "C" fn vmm_init() -> bool {
    spinlock_init(ptr::addr_of_mut!(VMM_LOCK));
    let cr3: u64;
    asm!("mov {}, cr3", out(reg) cr3, options(nomem, nostack));
    let root_physical = cr3 & PAGE_MASK;
    if root_physical == 0 {
        return false;
    }
    PML4 = paging_phys_to_virt(root_physical).cast();
    if PML4.is_null() {
        return false;
    }
    KERNEL_ROOT_PHYSICAL = root_physical;
    INITIALIZED = true;
    true
}

#[no_mangle]
pub unsafe extern "C" fn vmm_create_address_space() -> AddressSpace {
    if !INITIALIZED {
        return 0;
    }
    let flags = spinlock_lock_irqsave(ptr::addr_of_mut!(VMM_LOCK));
    let root = allocate_table();
    if root.is_null() {
        spinlock_unlock_irqrestore(ptr::addr_of_mut!(VMM_LOCK), flags);
        return 0;
    }
    for index in 256..512 {
        root.add(index).write(PML4.add(index).read());
    }
    let physical = table_physical(root);
    spinlock_unlock_irqrestore(ptr::addr_of_mut!(VMM_LOCK), flags);
    physical
}

unsafe fn clone_user_table(source: *mut u64, destination: *mut u64, level: u8) -> bool {
    let entries = if level == 4 { 256 } else { 512 };
    for index in 0..entries {
        let entry = source.add(index).read();
        if entry & VMM_PRESENT == 0 || entry & VMM_USER == 0 {
            continue;
        }
        if level == 1 {
            let physical = pmm_alloc_page();
            if physical == 0 {
                return false;
            }
            let source_page = paging_phys_to_virt(entry & PAGE_MASK).cast::<u8>();
            let destination_page = paging_phys_to_virt(physical).cast::<u8>();
            if source_page.is_null() || destination_page.is_null() {
                pmm_free_page(physical);
                return false;
            }
            ptr::copy_nonoverlapping(source_page, destination_page, VMM_PAGE_SIZE as usize);
            destination
                .add(index)
                .write(physical | (entry & !PAGE_MASK));
            continue;
        }
        if entry & PAGE_HUGE != 0 {
            return false;
        }
        let source_child = table_pointer(entry);
        let destination_child = allocate_table();
        if source_child.is_null() || destination_child.is_null() {
            if !destination_child.is_null() {
                pmm_free_page(table_physical(destination_child));
            }
            return false;
        }
        destination
            .add(index)
            .write(table_physical(destination_child) | (entry & !PAGE_MASK));
        if !clone_user_table(source_child, destination_child, level - 1) {
            return false;
        }
    }
    true
}

unsafe fn destroy_user_table(table: *mut u64, level: u8) {
    for index in 0..512 {
        let entry = table.add(index).read();
        if entry & VMM_PRESENT == 0 || entry & VMM_USER == 0 {
            continue;
        }
        let physical = entry & PAGE_MASK;
        if level == 1 {
            if pmm_is_allocated_page(physical) {
                pmm_free_page(physical);
            }
            table.add(index).write(0);
            continue;
        }
        if entry & PAGE_HUGE != 0 {
            table.add(index).write(0);
            continue;
        }
        let child = table_pointer(entry);
        if !child.is_null() {
            destroy_user_table(child, level - 1);
            table.add(index).write(0);
            if pmm_is_allocated_page(physical) {
                pmm_free_page(physical);
            }
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn vmm_clone_address_space(source: AddressSpace) -> AddressSpace {
    if !INITIALIZED || source == 0 || source & (VMM_PAGE_SIZE - 1) != 0 {
        return 0;
    }
    let flags = spinlock_lock_irqsave(ptr::addr_of_mut!(VMM_LOCK));
    let source_root = paging_phys_to_virt(source).cast::<u64>();
    let destination_root = allocate_table();
    if source_root.is_null() || destination_root.is_null() {
        if !destination_root.is_null() {
            pmm_free_page(table_physical(destination_root));
        }
        spinlock_unlock_irqrestore(ptr::addr_of_mut!(VMM_LOCK), flags);
        return 0;
    }
    for index in 256..512 {
        destination_root
            .add(index)
            .write(source_root.add(index).read());
    }
    let ok = clone_user_table(source_root, destination_root, 4);
    let destination_physical = table_physical(destination_root);
    spinlock_unlock_irqrestore(ptr::addr_of_mut!(VMM_LOCK), flags);
    if !ok {
        vmm_destroy_address_space(destination_physical);
        return 0;
    }
    destination_physical
}

#[no_mangle]
pub unsafe extern "C" fn vmm_destroy_address_space(address_space: AddressSpace) -> bool {
    if !INITIALIZED
        || address_space == 0
        || address_space & (VMM_PAGE_SIZE - 1) != 0
        || address_space == KERNEL_ROOT_PHYSICAL
        || address_space == vmm_current_address_space()
    {
        return false;
    }
    let flags = spinlock_lock_irqsave(ptr::addr_of_mut!(VMM_LOCK));
    let root = paging_phys_to_virt(address_space).cast::<u64>();
    if root.is_null() || !pmm_is_allocated_page(address_space) {
        spinlock_unlock_irqrestore(ptr::addr_of_mut!(VMM_LOCK), flags);
        return false;
    }
    for index in 0..256 {
        let entry = root.add(index).read();
        if entry & VMM_PRESENT == 0 || entry & VMM_USER == 0 || entry & PAGE_HUGE != 0 {
            continue;
        }
        let physical = entry & PAGE_MASK;
        let child = table_pointer(entry);
        if !child.is_null() {
            destroy_user_table(child, 3);
            root.add(index).write(0);
            if pmm_is_allocated_page(physical) {
                pmm_free_page(physical);
            }
        }
    }
    spinlock_unlock_irqrestore(ptr::addr_of_mut!(VMM_LOCK), flags);
    pmm_free_page(address_space)
}

#[no_mangle]
pub unsafe extern "C" fn vmm_switch_address_space(address_space: AddressSpace) -> bool {
    if !INITIALIZED || address_space == 0 || address_space & (VMM_PAGE_SIZE - 1) != 0 {
        return false;
    }
    let root = paging_phys_to_virt(address_space).cast::<u64>();
    if root.is_null() {
        return false;
    }
    asm!("mov cr3, {}", in(reg) address_space, options(nostack));
    PML4 = root;
    true
}

#[no_mangle]
pub unsafe extern "C" fn vmm_current_address_space() -> AddressSpace {
    if INITIALIZED {
        table_physical(PML4)
    } else {
        0
    }
}

#[no_mangle]
pub unsafe extern "C" fn vmm_map_page_in(
    address_space: AddressSpace,
    virtual_address: u64,
    physical_address: u64,
    flags: u64,
) -> bool {
    if !INITIALIZED || address_space == 0 || !canonical_address(virtual_address) {
        return false;
    }
    if virtual_address & (VMM_PAGE_SIZE - 1) != 0
        || physical_address & (VMM_PAGE_SIZE - 1) != 0
        || physical_address & !PAGE_MASK != 0
    {
        return false;
    }
    let allowed_flags = VMM_PRESENT
        | VMM_WRITABLE
        | VMM_USER
        | VMM_WRITE_THROUGH
        | VMM_CACHE_DISABLE
        | VMM_GLOBAL
        | VMM_NO_EXECUTE;
    if flags & !allowed_flags != 0 || flags & VMM_NO_EXECUTE != 0 && !paging_nx_supported() {
        return false;
    }

    let lock_flags = spinlock_lock_irqsave(ptr::addr_of_mut!(VMM_LOCK));
    let root = paging_phys_to_virt(address_space).cast::<u64>();
    if root.is_null() {
        spinlock_unlock_irqrestore(ptr::addr_of_mut!(VMM_LOCK), lock_flags);
        return false;
    }
    let user = flags & VMM_USER != 0;
    let pml4_index = ((virtual_address >> 39) & 0x1FF) as usize;
    let pdpt_index = ((virtual_address >> 30) & 0x1FF) as usize;
    let pd_index = ((virtual_address >> 21) & 0x1FF) as usize;
    let pt_index = ((virtual_address >> 12) & 0x1FF) as usize;

    let pdpt = get_next_table(root, pml4_index, true, user);
    if pdpt.is_null() {
        prune_empty_path(root, pml4_index, pdpt_index, pd_index);
        spinlock_unlock_irqrestore(ptr::addr_of_mut!(VMM_LOCK), lock_flags);
        return false;
    }
    let pd = get_next_table(pdpt, pdpt_index, true, user);
    if pd.is_null() {
        prune_empty_path(root, pml4_index, pdpt_index, pd_index);
        spinlock_unlock_irqrestore(ptr::addr_of_mut!(VMM_LOCK), lock_flags);
        return false;
    }
    let pt = get_next_table(pd, pd_index, true, user);
    if pt.is_null() {
        prune_empty_path(root, pml4_index, pdpt_index, pd_index);
        spinlock_unlock_irqrestore(ptr::addr_of_mut!(VMM_LOCK), lock_flags);
        return false;
    }
    if pt.add(pt_index).read() & VMM_PRESENT != 0 {
        spinlock_unlock_irqrestore(ptr::addr_of_mut!(VMM_LOCK), lock_flags);
        return false;
    }
    pt.add(pt_index)
        .write(physical_address | VMM_PRESENT | (flags & !VMM_PRESENT));
    paging_invlpg(virtual_address);
    spinlock_unlock_irqrestore(ptr::addr_of_mut!(VMM_LOCK), lock_flags);
    true
}

#[no_mangle]
pub unsafe extern "C" fn vmm_map_page(
    virtual_address: u64,
    physical_address: u64,
    flags: u64,
) -> bool {
    vmm_map_page_in(
        vmm_current_address_space(),
        virtual_address,
        physical_address,
        flags,
    )
}

#[no_mangle]
pub unsafe extern "C" fn vmm_unmap_page_in(
    address_space: AddressSpace,
    virtual_address: u64,
) -> bool {
    if !INITIALIZED
        || address_space == 0
        || !canonical_address(virtual_address)
        || virtual_address & (VMM_PAGE_SIZE - 1) != 0
    {
        return false;
    }
    let lock_flags = spinlock_lock_irqsave(ptr::addr_of_mut!(VMM_LOCK));
    let root = paging_phys_to_virt(address_space).cast::<u64>();
    if root.is_null() {
        spinlock_unlock_irqrestore(ptr::addr_of_mut!(VMM_LOCK), lock_flags);
        return false;
    }
    let pml4_index = ((virtual_address >> 39) & 0x1FF) as usize;
    let pdpt_index = ((virtual_address >> 30) & 0x1FF) as usize;
    let pd_index = ((virtual_address >> 21) & 0x1FF) as usize;
    let pt_index = ((virtual_address >> 12) & 0x1FF) as usize;

    let pml4_entry = root.add(pml4_index).read();
    if pml4_entry & VMM_PRESENT == 0 {
        spinlock_unlock_irqrestore(ptr::addr_of_mut!(VMM_LOCK), lock_flags);
        return false;
    }
    let pdpt = table_pointer(pml4_entry);
    if pdpt.is_null() {
        spinlock_unlock_irqrestore(ptr::addr_of_mut!(VMM_LOCK), lock_flags);
        return false;
    }
    let pdpt_entry = pdpt.add(pdpt_index).read();
    if pdpt_entry & VMM_PRESENT == 0 || pdpt_entry & PAGE_HUGE != 0 {
        spinlock_unlock_irqrestore(ptr::addr_of_mut!(VMM_LOCK), lock_flags);
        return false;
    }
    let pd = table_pointer(pdpt_entry);
    if pd.is_null() {
        spinlock_unlock_irqrestore(ptr::addr_of_mut!(VMM_LOCK), lock_flags);
        return false;
    }
    let pd_entry = pd.add(pd_index).read();
    if pd_entry & VMM_PRESENT == 0 || pd_entry & PAGE_HUGE != 0 {
        spinlock_unlock_irqrestore(ptr::addr_of_mut!(VMM_LOCK), lock_flags);
        return false;
    }
    let pt = table_pointer(pd_entry);
    if pt.is_null() || pt.add(pt_index).read() & VMM_PRESENT == 0 {
        spinlock_unlock_irqrestore(ptr::addr_of_mut!(VMM_LOCK), lock_flags);
        return false;
    }
    pt.add(pt_index).write(0);
    paging_invlpg(virtual_address);
    reclaim_table(pd, pd_index);
    reclaim_table(pdpt, pdpt_index);
    reclaim_table(root, pml4_index);
    spinlock_unlock_irqrestore(ptr::addr_of_mut!(VMM_LOCK), lock_flags);
    true
}

#[no_mangle]
pub unsafe extern "C" fn vmm_unmap_page(virtual_address: u64) -> bool {
    vmm_unmap_page_in(vmm_current_address_space(), virtual_address)
}

#[no_mangle]
pub unsafe extern "C" fn vmm_protect_page_in(
    address_space: AddressSpace,
    virtual_address: u64,
    flags: u64,
) -> bool {
    let allowed_flags = VMM_WRITABLE | VMM_USER | VMM_NO_EXECUTE;
    if !INITIALIZED
        || address_space == 0
        || !canonical_address(virtual_address)
        || virtual_address & (VMM_PAGE_SIZE - 1) != 0
        || flags & !allowed_flags != 0
        || flags & VMM_NO_EXECUTE != 0 && !paging_nx_supported()
    {
        return false;
    }
    let lock_flags = spinlock_lock_irqsave(ptr::addr_of_mut!(VMM_LOCK));
    let root = paging_phys_to_virt(address_space).cast::<u64>();
    if root.is_null() {
        spinlock_unlock_irqrestore(ptr::addr_of_mut!(VMM_LOCK), lock_flags);
        return false;
    }
    let pml4_index = ((virtual_address >> 39) & 0x1FF) as usize;
    let pdpt_index = ((virtual_address >> 30) & 0x1FF) as usize;
    let pd_index = ((virtual_address >> 21) & 0x1FF) as usize;
    let pt_index = ((virtual_address >> 12) & 0x1FF) as usize;
    let pml4_entry = root.add(pml4_index).read();
    if pml4_entry & VMM_PRESENT == 0 {
        spinlock_unlock_irqrestore(ptr::addr_of_mut!(VMM_LOCK), lock_flags);
        return false;
    }
    let pdpt = table_pointer(pml4_entry);
    if pdpt.is_null() {
        spinlock_unlock_irqrestore(ptr::addr_of_mut!(VMM_LOCK), lock_flags);
        return false;
    }
    let pdpt_entry = pdpt.add(pdpt_index).read();
    if pdpt_entry & VMM_PRESENT == 0 || pdpt_entry & PAGE_HUGE != 0 {
        spinlock_unlock_irqrestore(ptr::addr_of_mut!(VMM_LOCK), lock_flags);
        return false;
    }
    let pd = table_pointer(pdpt_entry);
    if pd.is_null() {
        spinlock_unlock_irqrestore(ptr::addr_of_mut!(VMM_LOCK), lock_flags);
        return false;
    }
    let pd_entry = pd.add(pd_index).read();
    if pd_entry & VMM_PRESENT == 0 || pd_entry & PAGE_HUGE != 0 {
        spinlock_unlock_irqrestore(ptr::addr_of_mut!(VMM_LOCK), lock_flags);
        return false;
    }
    let pt = table_pointer(pd_entry);
    if pt.is_null() {
        spinlock_unlock_irqrestore(ptr::addr_of_mut!(VMM_LOCK), lock_flags);
        return false;
    }
    let leaf = pt.add(pt_index);
    let entry = leaf.read();
    if entry & VMM_PRESENT == 0 {
        spinlock_unlock_irqrestore(ptr::addr_of_mut!(VMM_LOCK), lock_flags);
        return false;
    }
    leaf.write((entry & !(VMM_WRITABLE | VMM_USER | VMM_NO_EXECUTE)) | flags);
    paging_invlpg(virtual_address);
    spinlock_unlock_irqrestore(ptr::addr_of_mut!(VMM_LOCK), lock_flags);
    true
}

#[no_mangle]
pub unsafe extern "C" fn vmm_translate_in(
    address_space: AddressSpace,
    virtual_address: u64,
) -> u64 {
    if !INITIALIZED || address_space == 0 || !canonical_address(virtual_address) {
        return 0;
    }
    let lock_flags = spinlock_lock_irqsave(ptr::addr_of_mut!(VMM_LOCK));
    let root = paging_phys_to_virt(address_space).cast::<u64>();
    if root.is_null() {
        spinlock_unlock_irqrestore(ptr::addr_of_mut!(VMM_LOCK), lock_flags);
        return 0;
    }
    let pml4_index = ((virtual_address >> 39) & 0x1FF) as usize;
    let pdpt_index = ((virtual_address >> 30) & 0x1FF) as usize;
    let pd_index = ((virtual_address >> 21) & 0x1FF) as usize;
    let pt_index = ((virtual_address >> 12) & 0x1FF) as usize;
    let mut result = 0;

    let pml4_entry = root.add(pml4_index).read();
    if pml4_entry & VMM_PRESENT != 0 {
        let pdpt = table_pointer(pml4_entry);
        if !pdpt.is_null() {
            let pdpt_entry = pdpt.add(pdpt_index).read();
            if pdpt_entry & VMM_PRESENT != 0 {
                if pdpt_entry & PAGE_HUGE != 0 {
                    result = (pdpt_entry & 0x000F_FFFF_C000_0000) | (virtual_address & 0x3FFF_FFFF);
                } else {
                    let pd = table_pointer(pdpt_entry);
                    if !pd.is_null() {
                        let pd_entry = pd.add(pd_index).read();
                        if pd_entry & VMM_PRESENT != 0 {
                            if pd_entry & PAGE_HUGE != 0 {
                                result = (pd_entry & 0x000F_FFFF_FFE0_0000)
                                    | (virtual_address & 0x1F_FFFF);
                            } else {
                                let pt = table_pointer(pd_entry);
                                if !pt.is_null() {
                                    let entry = pt.add(pt_index).read();
                                    if entry & VMM_PRESENT != 0 {
                                        result = (entry & PAGE_MASK)
                                            | (virtual_address & (VMM_PAGE_SIZE - 1));
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    spinlock_unlock_irqrestore(ptr::addr_of_mut!(VMM_LOCK), lock_flags);
    result
}

#[no_mangle]
pub unsafe extern "C" fn vmm_translate(virtual_address: u64) -> u64 {
    vmm_translate_in(vmm_current_address_space(), virtual_address)
}

#[no_mangle]
pub unsafe extern "C" fn vmm_user_range_valid(
    address_space: AddressSpace,
    address: u64,
    size: u64,
    writable: bool,
) -> bool {
    const USER_LIMIT: u64 = 0x0000_8000_0000_0000;
    if size == 0 {
        return true;
    }
    if address == 0 || address >= USER_LIMIT || size > USER_LIMIT - address {
        return false;
    }
    let last = address + size - 1;
    let mut page = address & !(VMM_PAGE_SIZE - 1);
    let last_page = last & !(VMM_PAGE_SIZE - 1);
    loop {
        let lock_flags = spinlock_lock_irqsave(ptr::addr_of_mut!(VMM_LOCK));
        let mut table = paging_phys_to_virt(address_space).cast::<u64>();
        let mut valid = !table.is_null();
        let mut user = true;
        let mut page_writable = true;
        if valid {
            for level in 0..4 {
                let shift = [39u32, 30, 21, 12][level];
                let index = ((page >> shift) & 0x1FF) as usize;
                let entry = table.add(index).read();
                if entry & VMM_PRESENT == 0 {
                    valid = false;
                    break;
                }
                user &= entry & VMM_USER != 0;
                page_writable &= entry & VMM_WRITABLE != 0;
                if level < 3 {
                    if entry & PAGE_HUGE != 0 && level > 0 {
                        valid = false;
                        break;
                    }
                    table = table_pointer(entry);
                    if table.is_null() {
                        valid = false;
                        break;
                    }
                }
            }
        }
        spinlock_unlock_irqrestore(ptr::addr_of_mut!(VMM_LOCK), lock_flags);
        if !valid || !user || writable && !page_writable {
            return false;
        }
        if page == last_page {
            return true;
        }
        page += VMM_PAGE_SIZE;
    }
}

#[no_mangle]
pub unsafe extern "C" fn vmm_user_address_executable(
    address_space: AddressSpace,
    address: u64,
) -> bool {
    const USER_LIMIT: u64 = 0x0000_8000_0000_0000;
    if address_space == 0 || address == 0 || address >= USER_LIMIT {
        return false;
    }
    let lock_flags = spinlock_lock_irqsave(ptr::addr_of_mut!(VMM_LOCK));
    let mut table = paging_phys_to_virt(address_space).cast::<u64>();
    if table.is_null() {
        spinlock_unlock_irqrestore(ptr::addr_of_mut!(VMM_LOCK), lock_flags);
        return false;
    }
    let mut valid = true;
    for level in 0..4 {
        let shift = [39u32, 30, 21, 12][level];
        let index = ((address >> shift) & 0x1FF) as usize;
        let entry = table.add(index).read();
        if entry & VMM_PRESENT == 0 || entry & VMM_USER == 0 || entry & VMM_NO_EXECUTE != 0 {
            valid = false;
            break;
        }
        if level < 3 {
            if entry & PAGE_HUGE != 0 {
                valid = false;
                break;
            }
            table = table_pointer(entry);
            if table.is_null() {
                valid = false;
                break;
            }
        }
    }
    spinlock_unlock_irqrestore(ptr::addr_of_mut!(VMM_LOCK), lock_flags);
    valid
}
