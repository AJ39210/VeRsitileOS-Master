#include <stdint.h>
#include <stdbool.h>

#include "limine.h"
#include "pmm.h"
#include "paging.h"
#include "spinlock.h"

#define PAGE_SIZE 4096ULL

static uint64_t *used_bitmap;
static uint64_t *allocated_bitmap;

static uint64_t max_pages;
static uint64_t total_pages;
static uint64_t free_pages;

static uint64_t bitmap_words;
static uint64_t bitmap_physical;
static uint64_t bitmap_storage_pages;

static uint64_t search_hint;

static bool initialized;

static spinlock_t pmm_lock;

/* Bitmap helpers */

static inline void bit_set(uint64_t *bitmap, uint64_t index)
{
    bitmap[index / 64] |= 1ULL << (index % 64);
}

static inline void bit_clear(uint64_t *bitmap, uint64_t index)
{
    bitmap[index / 64] &= ~(1ULL << (index % 64));
}

static inline bool bit_test(const uint64_t *bitmap, uint64_t index)
{
    return (bitmap[index / 64] & (1ULL << (index % 64))) != 0;
}

/* Alignment helpers */

static uint64_t align_up(uint64_t value)
{
    if (value > UINT64_MAX - (PAGE_SIZE - 1))
        return 0;

    return (value + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

static uint64_t align_down(uint64_t value)
{
    return value & ~(PAGE_SIZE - 1);
}

/* Find bitmap storage */

static bool find_bitmap_storage(
    struct limine_memmap_response *memmap,
    uint64_t required_size,
    uint64_t *result
)
{
    for (uint64_t i = 0; i < memmap->entry_count; i++)
    {
        struct limine_memmap_entry *entry = memmap->entries[i];

        if (!entry || entry->type != LIMINE_MEMMAP_USABLE)
            continue;

        if (entry->base > UINT64_MAX - entry->length)
            continue;

        uint64_t start = align_up(entry->base);
        uint64_t end = align_down(entry->base + entry->length);

        if (start < PAGE_SIZE)
            start = PAGE_SIZE;

        if (end < start)
            continue;

        if (end - start >= required_size)
        {
            *result = start;
            return true;
        }
    }

    return false;
}

/* Initialize PMM */

bool pmm_init(
    struct limine_memmap_response *memmap,
    uint64_t hhdm_offset
)
{
    (void)hhdm_offset;

    initialized = false;

    spinlock_init(&pmm_lock);

    if (!memmap || !memmap->entries || !memmap->entry_count)
        return false;

    uint64_t highest_address = 0;

    for (uint64_t i = 0; i < memmap->entry_count; i++)
    {
        struct limine_memmap_entry *entry = memmap->entries[i];

        if (!entry || entry->base > UINT64_MAX - entry->length)
            return false;

        uint64_t end = entry->base + entry->length;

        if (end > highest_address)
            highest_address = end;
    }

    if (!highest_address)
        return false;

    max_pages = highest_address / PAGE_SIZE;

    if (highest_address % PAGE_SIZE)
        max_pages++;

    if (!max_pages)
        return false;

    bitmap_words = max_pages / 64;

    if (max_pages % 64)
        bitmap_words++;

    if (bitmap_words > UINT64_MAX / sizeof(uint64_t))
        return false;

    uint64_t one_bitmap_size =
        align_up(bitmap_words * sizeof(uint64_t));

    if (!one_bitmap_size || one_bitmap_size > UINT64_MAX / 2)
        return false;

    uint64_t storage_size = one_bitmap_size * 2;

    if (!find_bitmap_storage(
            memmap,
            storage_size,
            &bitmap_physical
        ))
    {
        return false;
    }

    bitmap_storage_pages = storage_size / PAGE_SIZE;

    used_bitmap = paging_phys_to_virt(bitmap_physical);

    allocated_bitmap = paging_phys_to_virt(
        bitmap_physical + one_bitmap_size
    );

    if (!used_bitmap || !allocated_bitmap)
        return false;

    for (uint64_t i = 0; i < bitmap_words; i++)
    {
        used_bitmap[i] = UINT64_MAX;
        allocated_bitmap[i] = 0;
    }

    total_pages = 0;
    free_pages = 0;

    /* Release usable pages. */

    for (uint64_t i = 0; i < memmap->entry_count; i++)
    {
        struct limine_memmap_entry *entry = memmap->entries[i];

        if (entry->type != LIMINE_MEMMAP_USABLE)
            continue;

        uint64_t start = align_up(entry->base);
        uint64_t end = align_down(entry->base + entry->length);

        if (start < PAGE_SIZE)
            start = PAGE_SIZE;

        if (end <= start)
            continue;

        uint64_t first_page = start / PAGE_SIZE;
        uint64_t end_page = end / PAGE_SIZE;

        for (uint64_t page = first_page;
             page < end_page && page < max_pages;
             page++)
        {
            if (bit_test(used_bitmap, page))
            {
                bit_clear(used_bitmap, page);

                total_pages++;
                free_pages++;
            }
        }
    }

    /* Reserve bitmap storage. */

    uint64_t first_bitmap_page =
        bitmap_physical / PAGE_SIZE;

    for (uint64_t i = 0; i < bitmap_storage_pages; i++)
    {
        uint64_t page = first_bitmap_page + i;

        if (page < max_pages && !bit_test(used_bitmap, page))
        {
            bit_set(used_bitmap, page);
            free_pages--;
        }
    }

    search_hint = 1;
    initialized = true;

    return true;
}

/* Allocate physical page */

uint64_t pmm_alloc_page(void)
{
    uint64_t flags = spinlock_lock_irqsave(&pmm_lock);

    if (!initialized || free_pages == 0)
    {
        spinlock_unlock_irqrestore(&pmm_lock, flags);
        return 0;
    }

    for (uint64_t pass = 0; pass < 2; pass++)
    {
        uint64_t start = pass == 0 ? search_hint : 1;
        uint64_t end = pass == 0 ? max_pages : search_hint;

        for (uint64_t page = start; page < end; page++)
        {
            if (!bit_test(used_bitmap, page))
            {
                bit_set(used_bitmap, page);
                bit_set(allocated_bitmap, page);

                free_pages--;

                search_hint = page + 1;

                if (search_hint >= max_pages)
                    search_hint = 1;

                uint64_t physical = page * PAGE_SIZE;

                spinlock_unlock_irqrestore(&pmm_lock, flags);

                return physical;
            }
        }
    }

    spinlock_unlock_irqrestore(&pmm_lock, flags);

    return 0;
}

uint64_t pmm_alloc_contiguous_pages(uint64_t page_count)
{
    if (!page_count)
        return 0;

    uint64_t flags = spinlock_lock_irqsave(&pmm_lock);
    if (!initialized || page_count > free_pages)
    {
        spinlock_unlock_irqrestore(&pmm_lock, flags);
        return 0;
    }

    uint64_t allocation_start = 0;
    uint64_t candidate = 1;
    while (candidate < max_pages &&
           page_count <= max_pages - candidate)
    {
        uint64_t run = 0;
        while (run < page_count &&
               !bit_test(used_bitmap, candidate + run))
            run++;
        if (run == page_count)
        {
            allocation_start = candidate;
            break;
        }
        candidate += run + 1;
    }

    if (allocation_start)
    {
        for (uint64_t i = 0; i < page_count; i++)
        {
            bit_set(used_bitmap, allocation_start + i);
            bit_set(allocated_bitmap, allocation_start + i);
        }
        free_pages -= page_count;
        search_hint = allocation_start + page_count;
        if (search_hint >= max_pages)
            search_hint = 1;
    }

    spinlock_unlock_irqrestore(&pmm_lock, flags);
    return allocation_start * PAGE_SIZE;
}

/* Free physical page */

bool pmm_free_page(uint64_t physical_address)
{
    if (physical_address == 0 ||
        (physical_address & (PAGE_SIZE - 1)))
    {
        return false;
    }

    uint64_t flags = spinlock_lock_irqsave(&pmm_lock);

    if (!initialized)
    {
        spinlock_unlock_irqrestore(&pmm_lock, flags);
        return false;
    }

    uint64_t page = physical_address / PAGE_SIZE;

    if (page >= max_pages ||
        !bit_test(allocated_bitmap, page))
    {
        spinlock_unlock_irqrestore(&pmm_lock, flags);
        return false;
    }

    bit_clear(allocated_bitmap, page);
    bit_clear(used_bitmap, page);

    free_pages++;

    if (page < search_hint)
        search_hint = page;

    spinlock_unlock_irqrestore(&pmm_lock, flags);

    return true;
}

/* Check ownership */

bool pmm_is_allocated_page(uint64_t physical_address)
{
    if (physical_address == 0 ||
        (physical_address & (PAGE_SIZE - 1)))
        return false;

    uint64_t flags = spinlock_lock_irqsave(&pmm_lock);

    bool result = false;

    if (initialized)
    {
        uint64_t page = physical_address / PAGE_SIZE;

        if (page < max_pages)
            result = bit_test(allocated_bitmap, page);
    }

    spinlock_unlock_irqrestore(&pmm_lock, flags);

    return result;
}

/* Statistics */

uint64_t pmm_get_total_memory(void)
{
    uint64_t flags = spinlock_lock_irqsave(&pmm_lock);
    uint64_t result = total_pages * PAGE_SIZE;
    spinlock_unlock_irqrestore(&pmm_lock, flags);
    return result;
}

uint64_t pmm_get_free_memory(void)
{
    uint64_t flags = spinlock_lock_irqsave(&pmm_lock);
    uint64_t result = free_pages * PAGE_SIZE;
    spinlock_unlock_irqrestore(&pmm_lock, flags);
    return result;
}

uint64_t pmm_get_used_memory(void)
{
    uint64_t flags = spinlock_lock_irqsave(&pmm_lock);
    uint64_t result = (total_pages - free_pages) * PAGE_SIZE;
    spinlock_unlock_irqrestore(&pmm_lock, flags);
    return result;
}

uint64_t pmm_get_total_pages(void)
{
    uint64_t flags = spinlock_lock_irqsave(&pmm_lock);
    uint64_t result = total_pages;
    spinlock_unlock_irqrestore(&pmm_lock, flags);
    return result;
}

uint64_t pmm_get_free_pages(void)
{
    uint64_t flags = spinlock_lock_irqsave(&pmm_lock);
    uint64_t result = free_pages;
    spinlock_unlock_irqrestore(&pmm_lock, flags);
    return result;
}