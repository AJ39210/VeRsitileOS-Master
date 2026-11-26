
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "paging.h"
#include "vmm.h"

#define PAGE_SIZE       4096ULL
#define PAGE_MASK       0x000FFFFFFFFFF000ULL

#define IOREMAP_BASE    0xFFFFC00000000000ULL
#define IOREMAP_LIMIT   0xFFFFC10000000000ULL

#define EFER_MSR        0xC0000080
#define EFER_NXE        (1ULL << 11)

#define IA32_PAT_DEFAULT 0x0007040600070406ULL

static uint64_t hhdm_offset;
static uint64_t *active_pml4;
static uint64_t next_ioremap = IOREMAP_BASE;

static bool nx_supported;

static inline uint64_t read_msr(uint32_t msr)
{
    uint32_t low;
    uint32_t high;

    __asm__ volatile (
        "rdmsr"
        : "=a"(low), "=d"(high)
        : "c"(msr)
    );

    return ((uint64_t)high << 32) | low;
}

static inline void write_msr(uint32_t msr, uint64_t value)
{
    uint32_t low = (uint32_t)value;
    uint32_t high = (uint32_t)(value >> 32);

    __asm__ volatile (
        "wrmsr"
        :
        : "c"(msr), "a"(low), "d"(high)
    );
}

static void initialize_nx(void)
{
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;

    __asm__ volatile (
        "cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(0x80000000), "c"(0)
    );

    if (eax < 0x80000001)
        return;

    __asm__ volatile (
        "cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(0x80000001), "c"(0)
    );

    if (!(edx & (1U << 20)))
        return;

    uint64_t efer = read_msr(EFER_MSR);

    write_msr(EFER_MSR, efer | EFER_NXE);

    nx_supported = true;
}

bool paging_init(uint64_t offset)
{
    hhdm_offset = offset;

    uint64_t cr3;

    __asm__ volatile (
        "mov %%cr3, %0"
        : "=r"(cr3)
    );

    uint64_t root_physical = cr3 & PAGE_MASK;

    if (!root_physical)
        return false;

    active_pml4 = paging_phys_to_virt(root_physical);

    if (!active_pml4)
        return false;

    next_ioremap = IOREMAP_BASE;

    initialize_nx();

    return true;
}

uint64_t paging_get_hhdm_offset(void)
{
    return hhdm_offset;
}

void *paging_phys_to_virt(uint64_t physical_address)
{
    if (physical_address > UINT64_MAX - hhdm_offset)
        return NULL;

    return (void *)(physical_address + hhdm_offset);
}

void paging_invlpg(uint64_t virtual_address)
{
    __asm__ volatile (
        "invlpg (%0)"
        :
        : "r"(virtual_address)
        : "memory"
    );
}

bool paging_nx_supported(void)
{
    return nx_supported;
}

void *ioremap(uint64_t physical_address, uint64_t size)
{
    if (!active_pml4 || size == 0)
        return NULL;

    if (physical_address > UINT64_MAX - size)
        return NULL;

    uint64_t offset = physical_address & (PAGE_SIZE - 1);

    uint64_t physical_start =
        physical_address & ~(PAGE_SIZE - 1);

    if (size > UINT64_MAX - offset)
        return NULL;

    uint64_t total_size = size + offset;

    if (total_size > UINT64_MAX - (PAGE_SIZE - 1))
        return NULL;

    uint64_t mapped_size =
        (total_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    if (next_ioremap > IOREMAP_LIMIT ||
        mapped_size > IOREMAP_LIMIT - next_ioremap)
        return NULL;

    uint64_t virtual_start = next_ioremap;

    /* Reserve the range, even if a later mapping fails. */
    next_ioremap += mapped_size;

    for (uint64_t page_offset = 0;
         page_offset < mapped_size;
         page_offset += PAGE_SIZE)
    {
        if (physical_start > UINT64_MAX - page_offset)
            return NULL;

        if (!vmm_map_page(
                virtual_start + page_offset,
                physical_start + page_offset,
                VMM_PRESENT |
                VMM_WRITABLE |
                VMM_WRITE_THROUGH |
                VMM_CACHE_DISABLE
            ))
        {
            return NULL;
        }
    }

    return (void *)(virtual_start + offset);
}
