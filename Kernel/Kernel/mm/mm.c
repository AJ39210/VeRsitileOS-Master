
#include <stdint.h>
#include <stdbool.h>

#include "limine.h"

#include "mm.h"
#include "paging.h"
#include "pmm.h"
#include "vmm.h"
#include "region.h"
#include "heap.h"

bool mm_init(
    struct limine_memmap_response *memmap,
    uint64_t hhdm_offset
)
{
    if (!memmap)
        return false;

    /* Initialize access to Limine's active page tables. */
    if (!paging_init(hhdm_offset))
        return false;

    /* Initialize physical memory allocation. */
    if (!pmm_init(memmap, hhdm_offset))
        return false;

    /* Initialize virtual memory management. */
    if (!vmm_init())
        return false;

    /* Initialize virtual memory regions. */
    if (!region_init())
        return false;

    /* Initialize the kernel heap. */
    if (!kheap_init())
        return false;

    return true;
}
