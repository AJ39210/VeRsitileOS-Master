#include <stdint.h>
#include <stdbool.h>

#include "region.h"
#include "spinlock.h"

#define PAGE_SIZE 4096ULL

#define REGION_BASE  0xFFFFC10000000000ULL
#define REGION_LIMIT 0xFFFFC20000000000ULL

#define REGION_SIZE  (REGION_LIMIT - REGION_BASE)
#define REGION_PAGES (REGION_SIZE / PAGE_SIZE)

#define BITMAP_WORDS (REGION_PAGES / 64)

static uint64_t region_bitmap[BITMAP_WORDS];

static spinlock_t region_lock;

static bool initialized;

static inline bool bit_test(uint64_t page)
{
    return (region_bitmap[page / 64] &
            (1ULL << (page % 64))) != 0;
}

static inline void bit_set(uint64_t page)
{
    region_bitmap[page / 64] |= 1ULL << (page % 64);
}

static inline void bit_clear(uint64_t page)
{
    region_bitmap[page / 64] &= ~(1ULL << (page % 64));
}

static bool power_of_two(uint64_t value)
{
    return value && !(value & (value - 1));
}

static uint64_t align_up(uint64_t value, uint64_t alignment)
{
    if (value > UINT64_MAX - (alignment - 1))
        return 0;

    return (value + alignment - 1) & ~(alignment - 1);
}

bool region_init(void)
{
    spinlock_init(&region_lock);

    for (uint64_t i = 0; i < BITMAP_WORDS; i++)
        region_bitmap[i] = 0;

    initialized = true;

    return true;
}

uint64_t region_alloc(uint64_t size, uint64_t alignment)
{
    if (!initialized || size == 0)
        return 0;

    if (alignment < PAGE_SIZE)
        alignment = PAGE_SIZE;

    if (!power_of_two(alignment))
        return 0;

    if (size > UINT64_MAX - (PAGE_SIZE - 1))
        return 0;

    uint64_t rounded_size =
        (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    uint64_t pages = rounded_size / PAGE_SIZE;

    if (pages > REGION_PAGES)
        return 0;

    uint64_t flags = spinlock_lock_irqsave(&region_lock);

    for (uint64_t i = 0; i + pages <= REGION_PAGES;)
    {
        uint64_t address = REGION_BASE + i * PAGE_SIZE;

        if (address & (alignment - 1))
        {
            i++;
            continue;
        }

        uint64_t j;

        for (j = 0; j < pages; j++)
        {
            if (bit_test(i + j))
                break;
        }

        if (j == pages)
        {
            for (uint64_t k = 0; k < pages; k++)
                bit_set(i + k);

            spinlock_unlock_irqrestore(&region_lock, flags);

            return address;
        }

        i += j + 1;
    }

    spinlock_unlock_irqrestore(&region_lock, flags);

    return 0;
}

bool region_free(uint64_t address, uint64_t size)
{
    if (!initialized || size == 0)
        return false;

    if ((address & (PAGE_SIZE - 1)) ||
        address < REGION_BASE ||
        address >= REGION_LIMIT)
    {
        return false;
    }

    if (size > UINT64_MAX - (PAGE_SIZE - 1))
        return false;

    uint64_t rounded_size =
        (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    uint64_t pages = rounded_size / PAGE_SIZE;

    uint64_t first_page =
        (address - REGION_BASE) / PAGE_SIZE;

    if (pages > REGION_PAGES ||
        first_page > REGION_PAGES - pages)
    {
        return false;
    }

    uint64_t flags = spinlock_lock_irqsave(&region_lock);

    /* Verify the entire allocation before freeing it. */

    for (uint64_t i = 0; i < pages; i++)
    {
        if (!bit_test(first_page + i))
        {
            spinlock_unlock_irqrestore(&region_lock, flags);
            return false;
        }
    }

    for (uint64_t i = 0; i < pages; i++)
        bit_clear(first_page + i);

    spinlock_unlock_irqrestore(&region_lock, flags);

    return true;
}

bool region_is_reserved(uint64_t address)
{
    if (!initialized ||
        address < REGION_BASE ||
        address >= REGION_LIMIT)
    {
        return false;
    }

    uint64_t page =
        (address - REGION_BASE) / PAGE_SIZE;

    uint64_t flags = spinlock_lock_irqsave(&region_lock);

    bool result = bit_test(page);

    spinlock_unlock_irqrestore(&region_lock, flags);

    return result;
}