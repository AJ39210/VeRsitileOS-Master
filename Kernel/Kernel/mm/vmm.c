#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "vmm.h"
#include "pmm.h"
#include "paging.h"
#include "spinlock.h"

#define PAGE_MASK  0x000FFFFFFFFFF000ULL
#define PAGE_HUGE  (1ULL << 7)

#define ENTRY_ADDRESS PAGE_MASK

static uint64_t *pml4;
static uint64_t kernel_root_physical;

static bool initialized;

static spinlock_t vmm_lock;

/* ---------------------------------------------------------
   Address helpers
   --------------------------------------------------------- */

static bool canonical_address(uint64_t address)
{
    uint64_t upper = address >> 47;

    return upper == 0 || upper == 0x1FFFF;
}

static void *table_pointer(uint64_t entry)
{
    uint64_t physical = entry & ENTRY_ADDRESS;

    return paging_phys_to_virt(physical);
}

static uint64_t table_physical(uint64_t *table)
{
    return (uint64_t)table - paging_get_hhdm_offset();
}

/* ---------------------------------------------------------
   Page-table allocation
   --------------------------------------------------------- */

static uint64_t *allocate_table(void)
{
    uint64_t physical = pmm_alloc_page();

    if (!physical)
        return NULL;

    uint64_t *table = paging_phys_to_virt(physical);

    if (!table)
    {
        pmm_free_page(physical);
        return NULL;
    }

    for (uint64_t i = 0; i < 512; i++)
        table[i] = 0;

    return table;
}

static uint64_t *get_next_table(
    uint64_t *parent,
    uint64_t index,
    bool create,
    bool user
)
{
    uint64_t entry = parent[index];

    if (!(entry & VMM_PRESENT))
    {
        if (!create)
            return NULL;

        uint64_t *new_table = allocate_table();

        if (!new_table)
            return NULL;

        uint64_t physical = table_physical(new_table);

        parent[index] = physical |
                        VMM_PRESENT |
                        VMM_WRITABLE;

        if (user)
            parent[index] |= VMM_USER;

        return new_table;
    }

    if (entry & PAGE_HUGE)
        return NULL;

    if (user)
        parent[index] |= VMM_USER;

    parent[index] |= VMM_WRITABLE;

    return table_pointer(entry);
}

/* ---------------------------------------------------------
   Empty-table detection
   --------------------------------------------------------- */

static bool table_is_empty(uint64_t *table)
{
    for (uint64_t i = 0; i < 512; i++)
    {
        if (table[i] & VMM_PRESENT)
            return false;
    }

    return true;
}

/*
 * Release a child table only if:
 * - It is empty.
 * - PMM confirms that we allocated it.
 */

static void reclaim_table(
    uint64_t *parent,
    uint64_t index
)
{
    uint64_t entry = parent[index];

    if (!(entry & VMM_PRESENT) || (entry & PAGE_HUGE))
        return;

    uint64_t physical = entry & ENTRY_ADDRESS;

    uint64_t *table = paging_phys_to_virt(physical);

    if (!table || !table_is_empty(table))
        return;

    if (!pmm_is_allocated_page(physical))
        return;

    parent[index] = 0;

    pmm_free_page(physical);
}

/* Clean up empty tables left by an unsuccessful mapping. */

static void prune_empty_path(
    uint64_t *root,
    uint64_t pml4_index,
    uint64_t pdpt_index,
    uint64_t pd_index
)
{
    uint64_t pml4_entry = root[pml4_index];

    if (!(pml4_entry & VMM_PRESENT) ||
        (pml4_entry & PAGE_HUGE))
        return;

    uint64_t *pdpt = table_pointer(pml4_entry);

    if (!pdpt)
        return;

    uint64_t pdpt_entry = pdpt[pdpt_index];

    if (!(pdpt_entry & VMM_PRESENT) ||
        (pdpt_entry & PAGE_HUGE))
    {
        reclaim_table(root, pml4_index);
        return;
    }

    uint64_t *pd = table_pointer(pdpt_entry);

    if (!pd)
        return;

    uint64_t pd_entry = pd[pd_index];

    if (!(pd_entry & VMM_PRESENT) ||
        (pd_entry & PAGE_HUGE))
    {
        reclaim_table(pdpt, pdpt_index);
        reclaim_table(root, pml4_index);
        return;
    }

    uint64_t *pt = table_pointer(pd_entry);

    if (!pt)
        return;

    reclaim_table(pd, pd_index);
    reclaim_table(pdpt, pdpt_index);
    reclaim_table(root, pml4_index);
}

/* ---------------------------------------------------------
   Initialization
   --------------------------------------------------------- */

bool vmm_init(void)
{
    spinlock_init(&vmm_lock);

    uint64_t cr3;

    __asm__ volatile (
        "mov %%cr3, %0"
        : "=r"(cr3)
    );

    uint64_t root_physical = cr3 & PAGE_MASK;

    if (!root_physical)
        return false;

    pml4 = paging_phys_to_virt(root_physical);

    if (!pml4)
        return false;

    kernel_root_physical = root_physical;
    initialized = true;

    return true;
}

vmm_address_space_t vmm_create_address_space(void)
{
    if (!initialized)
        return 0;

    uint64_t lock_flags = spinlock_lock_irqsave(&vmm_lock);
    uint64_t *root = allocate_table();

    if (!root)
    {
        spinlock_unlock_irqrestore(&vmm_lock, lock_flags);
        return 0;
    }

    for (uint64_t i = 256; i < 512; i++)
        root[i] = pml4[i];

    uint64_t physical = table_physical(root);
    spinlock_unlock_irqrestore(&vmm_lock, lock_flags);
    return physical;
}

static bool clone_user_table(
    uint64_t *source,
    uint64_t *destination,
    uint8_t level
)
{
    uint64_t entries = level == 4 ? 256 : 512;

    for (uint64_t i = 0; i < entries; i++)
    {
        uint64_t entry = source[i];

        if (!(entry & VMM_PRESENT))
            continue;

        if (!(entry & VMM_USER))
            continue;

        if (level == 1)
        {
            uint64_t physical = pmm_alloc_page();
            if (!physical)
                return false;

            uint8_t *src = paging_phys_to_virt(entry & PAGE_MASK);
            uint8_t *dst = paging_phys_to_virt(physical);

            if (!src || !dst)
            {
                pmm_free_page(physical);
                return false;
            }

            for (uint64_t byte = 0; byte < VMM_PAGE_SIZE; byte++)
                dst[byte] = src[byte];

            destination[i] =
                physical | (entry & ~PAGE_MASK);
            continue;
        }

        if (entry & PAGE_HUGE)
            return false;

        uint64_t *source_child = table_pointer(entry);
        uint64_t *destination_child = allocate_table();

        if (!source_child || !destination_child)
        {
            if (destination_child)
                pmm_free_page(table_physical(destination_child));
            return false;
        }

        uint64_t child_physical = table_physical(destination_child);
        destination[i] =
            child_physical | (entry & ~PAGE_MASK);

        if (!clone_user_table(
                source_child,
                destination_child,
                level - 1))
        {
            return false;
        }
    }

    return true;
}

static void destroy_user_table(uint64_t *table, uint8_t level)
{
    for (uint64_t i = 0; i < 512; i++)
    {
        uint64_t entry = table[i];

        if (!(entry & VMM_PRESENT) ||
            !(entry & VMM_USER))
            continue;

        uint64_t physical = entry & PAGE_MASK;

        if (level == 1)
        {
            if (pmm_is_allocated_page(physical))
                pmm_free_page(physical);
            table[i] = 0;
            continue;
        }

        if (entry & PAGE_HUGE)
        {
            table[i] = 0;
            continue;
        }

        uint64_t *child = table_pointer(entry);
        if (child)
        {
            destroy_user_table(child, level - 1);
            table[i] = 0;
            if (pmm_is_allocated_page(physical))
                pmm_free_page(physical);
        }
    }
}

vmm_address_space_t vmm_clone_address_space(
    vmm_address_space_t source
)
{
    if (!initialized || !source ||
        (source & (VMM_PAGE_SIZE - 1)))
        return 0;

    uint64_t lock_flags = spinlock_lock_irqsave(&vmm_lock);
    uint64_t *source_root = paging_phys_to_virt(source);
    uint64_t *destination_root = allocate_table();

    if (!source_root || !destination_root)
    {
        if (destination_root)
            pmm_free_page(table_physical(destination_root));
        spinlock_unlock_irqrestore(&vmm_lock, lock_flags);
        return 0;
    }

    for (uint64_t i = 256; i < 512; i++)
        destination_root[i] = source_root[i];

    bool ok = clone_user_table(source_root, destination_root, 4);
    uint64_t destination_physical =
        table_physical(destination_root);

    spinlock_unlock_irqrestore(&vmm_lock, lock_flags);

    if (!ok)
    {
        vmm_destroy_address_space(destination_physical);
        return 0;
    }

    return destination_physical;
}

bool vmm_destroy_address_space(vmm_address_space_t address_space)
{
    if (!initialized || !address_space ||
        (address_space & (VMM_PAGE_SIZE - 1)) ||
        address_space == kernel_root_physical ||
        address_space == vmm_current_address_space())
        return false;

    uint64_t lock_flags = spinlock_lock_irqsave(&vmm_lock);
    uint64_t *root = paging_phys_to_virt(address_space);

    if (!root || !pmm_is_allocated_page(address_space))
    {
        spinlock_unlock_irqrestore(&vmm_lock, lock_flags);
        return false;
    }

    for (uint64_t i = 0; i < 256; i++)
    {
        uint64_t entry = root[i];

        if (!(entry & VMM_PRESENT) ||
            !(entry & VMM_USER) ||
            (entry & PAGE_HUGE))
            continue;

        uint64_t physical = entry & PAGE_MASK;
        uint64_t *child = table_pointer(entry);
        if (child)
        {
            destroy_user_table(child, 3);
            root[i] = 0;
            if (pmm_is_allocated_page(physical))
                pmm_free_page(physical);
        }
    }

    spinlock_unlock_irqrestore(&vmm_lock, lock_flags);
    return pmm_free_page(address_space);
}

bool vmm_switch_address_space(vmm_address_space_t address_space)
{
    if (!initialized || !address_space ||
        (address_space & (VMM_PAGE_SIZE - 1)))
        return false;

    uint64_t *root = paging_phys_to_virt(address_space);
    if (!root)
        return false;

    __asm__ volatile (
        "mov %0, %%cr3"
        :
        : "r"(address_space)
        : "memory"
    );

    pml4 = root;
    return true;
}

vmm_address_space_t vmm_current_address_space(void)
{
    return initialized ? table_physical(pml4) : 0;
}

/* ---------------------------------------------------------
   Map page
   --------------------------------------------------------- */

bool vmm_map_page_in(
    vmm_address_space_t address_space,
    uint64_t virtual_address,
    uint64_t physical_address,
    uint64_t flags
)
{
    if (!initialized || !address_space ||
        !canonical_address(virtual_address))
        return false;

    if ((virtual_address & (VMM_PAGE_SIZE - 1)) ||
        (physical_address & (VMM_PAGE_SIZE - 1)))
        return false;

    if (physical_address & ~PAGE_MASK)
        return false;

    const uint64_t allowed_flags =
        VMM_PRESENT |
        VMM_WRITABLE |
        VMM_USER |
        VMM_WRITE_THROUGH |
        VMM_CACHE_DISABLE |
        VMM_GLOBAL |
        VMM_NO_EXECUTE;

    if (flags & ~allowed_flags)
        return false;

    if ((flags & VMM_NO_EXECUTE) &&
        !paging_nx_supported())
        return false;

    uint64_t lock_flags =
        spinlock_lock_irqsave(&vmm_lock);

    uint64_t *root = paging_phys_to_virt(address_space);
    if (!root)
    {
        spinlock_unlock_irqrestore(&vmm_lock, lock_flags);
        return false;
    }

    bool user = (flags & VMM_USER) != 0;

    uint64_t pml4_index =
        (virtual_address >> 39) & 0x1FF;

    uint64_t pdpt_index =
        (virtual_address >> 30) & 0x1FF;

    uint64_t pd_index =
        (virtual_address >> 21) & 0x1FF;

    uint64_t pt_index =
        (virtual_address >> 12) & 0x1FF;

    uint64_t *pdpt = get_next_table(
        root, pml4_index, true, user
    );

    if (!pdpt)
    {
        prune_empty_path(
            root,
            pml4_index,
            pdpt_index,
            pd_index
        );

        spinlock_unlock_irqrestore(&vmm_lock, lock_flags);
        return false;
    }

    uint64_t *pd = get_next_table(
        pdpt, pdpt_index, true, user
    );

    if (!pd)
    {
        prune_empty_path(
            root,
            pml4_index,
            pdpt_index,
            pd_index
        );

        spinlock_unlock_irqrestore(&vmm_lock, lock_flags);
        return false;
    }

    uint64_t *pt = get_next_table(
        pd, pd_index, true, user
    );

    if (!pt)
    {
        prune_empty_path(
            root,
            pml4_index,
            pdpt_index,
            pd_index
        );

        spinlock_unlock_irqrestore(&vmm_lock, lock_flags);
        return false;
    }

    if (pt[pt_index] & VMM_PRESENT)
    {
        spinlock_unlock_irqrestore(&vmm_lock, lock_flags);
        return false;
    }

    pt[pt_index] = physical_address |
                   VMM_PRESENT |
                   (flags & ~VMM_PRESENT);

    paging_invlpg(virtual_address);

    spinlock_unlock_irqrestore(&vmm_lock, lock_flags);

    return true;
}

bool vmm_map_page(
    uint64_t virtual_address,
    uint64_t physical_address,
    uint64_t flags
)
{
    return vmm_map_page_in(
        vmm_current_address_space(),
        virtual_address,
        physical_address,
        flags
    );
}

/* ---------------------------------------------------------
   Unmap page and reclaim empty tables
   --------------------------------------------------------- */

bool vmm_unmap_page_in(
    vmm_address_space_t address_space,
    uint64_t virtual_address
)
{
    if (!initialized || !address_space ||
        !canonical_address(virtual_address) ||
        (virtual_address & (VMM_PAGE_SIZE - 1)))
    {
        return false;
    }

    uint64_t lock_flags =
        spinlock_lock_irqsave(&vmm_lock);

    uint64_t *root = paging_phys_to_virt(address_space);
    if (!root)
    {
        spinlock_unlock_irqrestore(&vmm_lock, lock_flags);
        return false;
    }

    uint64_t pml4_index =
        (virtual_address >> 39) & 0x1FF;

    uint64_t pdpt_index =
        (virtual_address >> 30) & 0x1FF;

    uint64_t pd_index =
        (virtual_address >> 21) & 0x1FF;

    uint64_t pt_index =
        (virtual_address >> 12) & 0x1FF;

    if (!(root[pml4_index] & VMM_PRESENT))
        goto fail;

    uint64_t *pdpt = table_pointer(root[pml4_index]);

    if (!pdpt || !(pdpt[pdpt_index] & VMM_PRESENT))
        goto fail;

    if (pdpt[pdpt_index] & PAGE_HUGE)
        goto fail;

    uint64_t *pd = table_pointer(pdpt[pdpt_index]);

    if (!pd || !(pd[pd_index] & VMM_PRESENT))
        goto fail;

    if (pd[pd_index] & PAGE_HUGE)
        goto fail;

    uint64_t *pt = table_pointer(pd[pd_index]);

    if (!pt || !(pt[pt_index] & VMM_PRESENT))
        goto fail;

    /* Remove the leaf mapping. */

    pt[pt_index] = 0;

    paging_invlpg(virtual_address);

    /* Reclaim empty child tables, from bottom to top. */

    reclaim_table(pd, pd_index);
    reclaim_table(pdpt, pdpt_index);
    reclaim_table(root, pml4_index);

    spinlock_unlock_irqrestore(&vmm_lock, lock_flags);

    return true;

fail:
    spinlock_unlock_irqrestore(&vmm_lock, lock_flags);
    return false;
}

bool vmm_unmap_page(uint64_t virtual_address)
{
    return vmm_unmap_page_in(
        vmm_current_address_space(),
        virtual_address
    );
}

bool vmm_protect_page_in(
    vmm_address_space_t address_space,
    uint64_t virtual_address,
    uint64_t flags
)
{
    const uint64_t allowed_flags =
        VMM_WRITABLE | VMM_USER | VMM_NO_EXECUTE;
    if (!initialized || !address_space ||
        !canonical_address(virtual_address) ||
        (virtual_address & (VMM_PAGE_SIZE - 1)) ||
        (flags & ~allowed_flags) ||
        ((flags & VMM_NO_EXECUTE) && !paging_nx_supported()))
        return false;

    uint64_t lock_flags = spinlock_lock_irqsave(&vmm_lock);
    uint64_t *root = paging_phys_to_virt(address_space);
    if (!root)
    {
        spinlock_unlock_irqrestore(&vmm_lock, lock_flags);
        return false;
    }

    uint64_t pml4_index = (virtual_address >> 39) & 0x1FF;
    uint64_t pdpt_index = (virtual_address >> 30) & 0x1FF;
    uint64_t pd_index = (virtual_address >> 21) & 0x1FF;
    uint64_t pt_index = (virtual_address >> 12) & 0x1FF;

    if (!(root[pml4_index] & VMM_PRESENT))
        goto fail;

    uint64_t *pdpt = table_pointer(root[pml4_index]);
    if (!pdpt || !(pdpt[pdpt_index] & VMM_PRESENT) ||
        (pdpt[pdpt_index] & PAGE_HUGE))
        goto fail;

    uint64_t *pd = table_pointer(pdpt[pdpt_index]);
    if (!pd || !(pd[pd_index] & VMM_PRESENT) ||
        (pd[pd_index] & PAGE_HUGE))
        goto fail;

    uint64_t *pt = table_pointer(pd[pd_index]);
    if (!pt || !(pt[pt_index] & VMM_PRESENT))
        goto fail;

    pt[pt_index] =
        (pt[pt_index] & ~(VMM_WRITABLE | VMM_USER | VMM_NO_EXECUTE)) |
        flags;
    paging_invlpg(virtual_address);
    spinlock_unlock_irqrestore(&vmm_lock, lock_flags);
    return true;

fail:
    spinlock_unlock_irqrestore(&vmm_lock, lock_flags);
    return false;
}

/* ---------------------------------------------------------
   Virtual-to-physical translation
   --------------------------------------------------------- */

uint64_t vmm_translate_in(
    vmm_address_space_t address_space,
    uint64_t virtual_address
)
{
    if (!initialized || !address_space ||
        !canonical_address(virtual_address))
        return 0;

    uint64_t lock_flags =
        spinlock_lock_irqsave(&vmm_lock);

    uint64_t *root = paging_phys_to_virt(address_space);
    if (!root)
    {
        spinlock_unlock_irqrestore(&vmm_lock, lock_flags);
        return 0;
    }

    uint64_t pml4_index =
        (virtual_address >> 39) & 0x1FF;

    uint64_t pdpt_index =
        (virtual_address >> 30) & 0x1FF;

    uint64_t pd_index =
        (virtual_address >> 21) & 0x1FF;

    uint64_t pt_index =
        (virtual_address >> 12) & 0x1FF;

    uint64_t result = 0;

    if (!(root[pml4_index] & VMM_PRESENT))
        goto done;

    uint64_t *pdpt = table_pointer(root[pml4_index]);

    if (!pdpt || !(pdpt[pdpt_index] & VMM_PRESENT))
        goto done;

    uint64_t entry = pdpt[pdpt_index];

    /* 1 GiB page. */

    if (entry & PAGE_HUGE)
    {
        result = (entry & 0x000FFFFFC0000000ULL) |
                 (virtual_address & 0x3FFFFFFFULL);

        goto done;
    }

    uint64_t *pd = table_pointer(entry);

    if (!pd || !(pd[pd_index] & VMM_PRESENT))
        goto done;

    entry = pd[pd_index];

    /* 2 MiB page. */

    if (entry & PAGE_HUGE)
    {
        result = (entry & 0x000FFFFFFFE00000ULL) |
                 (virtual_address & 0x1FFFFFULL);

        goto done;
    }

    uint64_t *pt = table_pointer(entry);

    if (!pt || !(pt[pt_index] & VMM_PRESENT))
        goto done;

    result = (pt[pt_index] & PAGE_MASK) |
             (virtual_address & (VMM_PAGE_SIZE - 1));

done:
    spinlock_unlock_irqrestore(&vmm_lock, lock_flags);

    return result;
}

uint64_t vmm_translate(uint64_t virtual_address)
{
    return vmm_translate_in(
        vmm_current_address_space(),
        virtual_address
    );
}

bool vmm_user_range_valid(
    vmm_address_space_t address_space,
    uint64_t address,
    uint64_t size,
    bool writable
)
{
    const uint64_t user_limit = 0x0000800000000000ULL;

    if (!size)
        return true;

    if (!address || address >= user_limit ||
        size > user_limit - address)
        return false;

    uint64_t last = address + size - 1;
    uint64_t page = address & ~(VMM_PAGE_SIZE - 1);
    uint64_t last_page = last & ~(VMM_PAGE_SIZE - 1);

    for (;; page += VMM_PAGE_SIZE)
    {
        uint64_t lock_flags =
            spinlock_lock_irqsave(&vmm_lock);
        uint64_t *root = paging_phys_to_virt(address_space);
        bool valid = root != NULL;
        bool user = true;
        bool page_writable = true;

        if (valid)
        {
            uint64_t indices[4] = {
                (page >> 39) & 0x1FF,
                (page >> 30) & 0x1FF,
                (page >> 21) & 0x1FF,
                (page >> 12) & 0x1FF
            };
            uint64_t *table = root;
            uint64_t entry = 0;

            for (uint32_t level = 0; level < 4; level++)
            {
                entry = table[indices[level]];
                if (!(entry & VMM_PRESENT))
                {
                    valid = false;
                    break;
                }

                user &= (entry & VMM_USER) != 0;
                page_writable &= (entry & VMM_WRITABLE) != 0;

                if (level < 3)
                {
                    if ((entry & PAGE_HUGE) && level > 0)
                    {
                        valid = false;
                        break;
                    }

                    table = table_pointer(entry);
                    if (!table)
                    {
                        valid = false;
                        break;
                    }
                }
            }
        }

        spinlock_unlock_irqrestore(&vmm_lock, lock_flags);

        if (!valid || !user || (writable && !page_writable))
            return false;

        if (page == last_page)
            return true;
    }
}

bool vmm_user_address_executable(
    vmm_address_space_t address_space,
    uint64_t address
)
{
    const uint64_t user_limit = 0x0000800000000000ULL;
    if (!address_space || !address || address >= user_limit)
        return false;

    uint64_t lock_flags =
        spinlock_lock_irqsave(&vmm_lock);
    uint64_t *table = paging_phys_to_virt(address_space);
    if (!table)
    {
        spinlock_unlock_irqrestore(&vmm_lock, lock_flags);
        return false;
    }

    uint64_t indices[4] = {
        (address >> 39) & 0x1FF,
        (address >> 30) & 0x1FF,
        (address >> 21) & 0x1FF,
        (address >> 12) & 0x1FF
    };
    bool valid = true;

    for (uint32_t level = 0; level < 4; level++)
    {
        uint64_t entry = table[indices[level]];
        if (!(entry & VMM_PRESENT) ||
            !(entry & VMM_USER) ||
            (entry & VMM_NO_EXECUTE))
        {
            valid = false;
            break;
        }

        if (level < 3)
        {
            if (entry & PAGE_HUGE)
            {
                valid = false;
                break;
            }

            table = table_pointer(entry);
            if (!table)
            {
                valid = false;
                break;
            }
        }
    }

    spinlock_unlock_irqrestore(&vmm_lock, lock_flags);
    return valid;
}