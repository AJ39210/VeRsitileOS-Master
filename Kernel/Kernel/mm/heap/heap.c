#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "heap.h"
#include "region.h"
#include "pmm.h"
#include "vmm.h"
#include "paging.h"
#include "spinlock.h"

#define PAGE_SIZE 4096ULL

#define HEAP_MAX_SIZE (64ULL * 1024 * 1024)
#define HEAP_MAX_PAGES (HEAP_MAX_SIZE / PAGE_SIZE)

#define HEAP_ALIGNMENT 16ULL

#define BLOCK_MAGIC 0x48454150424C4F43ULL
#define TAG_MAGIC   0x48454150414C4C4FULL

struct heap_block
{
    uint64_t size;
    uint64_t magic;

    struct heap_block *prev;
    struct heap_block *next;

    uint64_t free;
} __attribute__((aligned(16)));

struct heap_tag
{
    uint64_t magic;
    struct heap_block *block;
    uint64_t requested;
    uint64_t alignment;
};

#define BLOCK_HEADER_SIZE sizeof(struct heap_block)
#define TAG_SIZE sizeof(struct heap_tag)

#define MIN_BLOCK_SIZE \
    ((BLOCK_HEADER_SIZE + TAG_SIZE + HEAP_ALIGNMENT + 15) & ~15ULL)

static uint64_t heap_base;
static uint64_t heap_pages;

static struct heap_block *first_block;
static struct heap_block *last_block;

static spinlock_t heap_lock;

static bool initialized;

/* ---------------------------------------------------------
   Utility functions
   --------------------------------------------------------- */

static bool power_of_two(uint64_t value)
{
    return value && !(value & (value - 1));
}

static uintptr_t align_up_pointer(
    uintptr_t value,
    uint64_t alignment
)
{
    if (value > UINTPTR_MAX - (alignment - 1))
        return 0;

    return (value + alignment - 1) & ~(uintptr_t)(alignment - 1);
}

static uint64_t align_up_size(uint64_t value)
{
    if (value > UINT64_MAX - 15)
        return 0;

    return (value + 15) & ~15ULL;
}

static void memory_zero(void *pointer, uint64_t size)
{
    uint8_t *bytes = pointer;

    for (uint64_t i = 0; i < size; i++)
        bytes[i] = 0;
}

static void memory_copy(
    void *destination,
    const void *source,
    uint64_t size
)
{
    uint8_t *dst = destination;
    const uint8_t *src = source;

    for (uint64_t i = 0; i < size; i++)
        dst[i] = src[i];
}

/* ---------------------------------------------------------
   Block management
   --------------------------------------------------------- */

static void split_block(
    struct heap_block *block,
    uint64_t required_size
)
{
    if (block->size < required_size)
        return;

    uint64_t remaining = block->size - required_size;

    if (remaining < MIN_BLOCK_SIZE)
        return;

    struct heap_block *new_block =
        (struct heap_block *)((uintptr_t)block + required_size);

    new_block->size = remaining;
    new_block->magic = BLOCK_MAGIC;
    new_block->prev = block;
    new_block->next = block->next;
    new_block->free = 1;

    if (new_block->next)
        new_block->next->prev = new_block;
    else
        last_block = new_block;

    block->next = new_block;
    block->size = required_size;
}

static struct heap_block *find_fit(
    uint64_t size,
    uint64_t alignment,
    uintptr_t *user_result,
    uint64_t *required_result
)
{
    struct heap_block *block = first_block;

    while (block)
    {
        if (block->free)
        {
            uintptr_t start = (uintptr_t)block;

            if (start <= UINTPTR_MAX -
                BLOCK_HEADER_SIZE - TAG_SIZE)
            {
                uintptr_t raw =
                    start + BLOCK_HEADER_SIZE + TAG_SIZE;

                uintptr_t user =
                    align_up_pointer(raw, alignment);

                if (user && user <= UINTPTR_MAX - size)
                {
                    uintptr_t end = user + size;

                    uint64_t required =
                        align_up_size(end - start);

                    if (required &&
                        required <= block->size)
                    {
                        *user_result = user;
                        *required_result = required;

                        return block;
                    }
                }
            }
        }

        block = block->next;
    }

    return NULL;
}

/* ---------------------------------------------------------
   Heap growth
   --------------------------------------------------------- */

static bool heap_grow(void)
{
    if (heap_pages >= HEAP_MAX_PAGES)
        return false;

    uint64_t physical = pmm_alloc_page();

    if (!physical)
        return false;

    uint64_t virtual_address =
        heap_base + heap_pages * PAGE_SIZE;

    uint64_t flags = VMM_PRESENT | VMM_WRITABLE;

    if (paging_nx_supported())
        flags |= VMM_NO_EXECUTE;

    if (!vmm_map_page(
            virtual_address,
            physical,
            flags
        ))
    {
        pmm_free_page(physical);
        return false;
    }

    memory_zero((void *)virtual_address, PAGE_SIZE);

    heap_pages++;

    if (last_block &&
        last_block->free &&
        (uintptr_t)last_block + last_block->size == virtual_address)
    {
        last_block->size += PAGE_SIZE;
    }
    else
    {
        struct heap_block *block =
            (struct heap_block *)virtual_address;

        block->size = PAGE_SIZE;
        block->magic = BLOCK_MAGIC;
        block->prev = last_block;
        block->next = NULL;
        block->free = 1;

        if (last_block)
            last_block->next = block;
        else
            first_block = block;

        last_block = block;
    }

    return true;
}

/* ---------------------------------------------------------
   Allocation
   --------------------------------------------------------- */

static void *heap_alloc_locked(
    uint64_t size,
    uint64_t alignment
)
{
    if (!initialized || size == 0)
        return NULL;

    if (!power_of_two(alignment) ||
        alignment < HEAP_ALIGNMENT)
    {
        return NULL;
    }

    for (;;)
    {
        uintptr_t user = 0;
        uint64_t required = 0;

        struct heap_block *block =
            find_fit(size, alignment, &user, &required);

        if (block)
        {
            split_block(block, required);

            block->free = 0;

            struct heap_tag *tag =
                (struct heap_tag *)(user - TAG_SIZE);

            tag->magic = TAG_MAGIC;
            tag->block = block;
            tag->requested = size;
            tag->alignment = alignment;

            return (void *)user;
        }

        if (!heap_grow())
            return NULL;
    }
}

/* ---------------------------------------------------------
   Free and merge
   --------------------------------------------------------- */

static bool heap_free_locked(void *pointer)
{
    if (!pointer)
        return true;

    uintptr_t address = (uintptr_t)pointer;

    uintptr_t heap_end =
        heap_base + heap_pages * PAGE_SIZE;

    if (address < heap_base + BLOCK_HEADER_SIZE + TAG_SIZE ||
        address >= heap_end)
    {
        return false;
    }

    struct heap_tag *tag =
        (struct heap_tag *)(address - TAG_SIZE);

    if (tag->magic != TAG_MAGIC)
        return false;

    struct heap_block *block = tag->block;

    if (!block ||
        (uintptr_t)block < heap_base ||
        (uintptr_t)block >= heap_end ||
        block->magic != BLOCK_MAGIC ||
        block->free)
    {
        return false;
    }

    if (address < (uintptr_t)block + BLOCK_HEADER_SIZE + TAG_SIZE ||
        address >= (uintptr_t)block + block->size)
    {
        return false;
    }

    tag->magic = 0;

    block->free = 1;

    /* Merge with previous block. */

    if (block->prev &&
        block->prev->free &&
        (uintptr_t)block->prev + block->prev->size ==
        (uintptr_t)block)
    {
        struct heap_block *previous = block->prev;

        previous->size += block->size;
        previous->next = block->next;

        if (block->next)
            block->next->prev = previous;
        else
            last_block = previous;

        block = previous;
    }

    /* Merge with next block. */

    if (block->next &&
        block->next->free &&
        (uintptr_t)block + block->size ==
        (uintptr_t)block->next)
    {
        struct heap_block *next = block->next;

        block->size += next->size;
        block->next = next->next;

        if (next->next)
            next->next->prev = block;
        else
            last_block = block;
    }

    return true;
}

/* ---------------------------------------------------------
   Release unused trailing pages
   --------------------------------------------------------- */

static void heap_trim(void)
{
    struct heap_block *block = last_block;

    if (!block || !block->free)
        return;

    while (heap_pages > 1 &&
           block->size >= PAGE_SIZE + MIN_BLOCK_SIZE)
    {
        uint64_t virtual_address =
            heap_base + (heap_pages - 1) * PAGE_SIZE;

        uint64_t physical =
            vmm_translate(virtual_address);

        if (!physical ||
            !pmm_is_allocated_page(physical))
        {
            break;
        }

        if (!vmm_unmap_page(virtual_address))
            break;

        if (!pmm_free_page(physical))
            break;

        block->size -= PAGE_SIZE;
        heap_pages--;
    }
}

/* ---------------------------------------------------------
   Initialize heap
   --------------------------------------------------------- */

bool kheap_init(void)
{
    if (initialized)
        return true;

    spinlock_init(&heap_lock);

    heap_base = region_alloc(
        HEAP_MAX_SIZE,
        PAGE_SIZE
    );

    if (!heap_base)
        return false;

    uint64_t physical = pmm_alloc_page();

    if (!physical)
    {
        region_free(heap_base, HEAP_MAX_SIZE);
        heap_base = 0;
        return false;
    }

    uint64_t flags = VMM_PRESENT | VMM_WRITABLE;

    if (paging_nx_supported())
        flags |= VMM_NO_EXECUTE;

    if (!vmm_map_page(heap_base, physical, flags))
    {
        pmm_free_page(physical);
        region_free(heap_base, HEAP_MAX_SIZE);
        heap_base = 0;
        return false;
    }

    memory_zero((void *)heap_base, PAGE_SIZE);

    first_block = (struct heap_block *)heap_base;

    first_block->size = PAGE_SIZE;
    first_block->magic = BLOCK_MAGIC;
    first_block->prev = NULL;
    first_block->next = NULL;
    first_block->free = 1;

    last_block = first_block;

    heap_pages = 1;
    initialized = true;

    return true;
}

/* ---------------------------------------------------------
   Public allocation API
   --------------------------------------------------------- */

void *kmalloc(uint64_t size)
{
    uint64_t flags = spinlock_lock_irqsave(&heap_lock);

    void *result = heap_alloc_locked(
        size,
        HEAP_ALIGNMENT
    );

    spinlock_unlock_irqrestore(&heap_lock, flags);

    return result;
}

void *kmemalign(uint64_t alignment, uint64_t size)
{
    uint64_t flags = spinlock_lock_irqsave(&heap_lock);

    void *result = heap_alloc_locked(size, alignment);

    spinlock_unlock_irqrestore(&heap_lock, flags);

    return result;
}

void *kcalloc(uint64_t count, uint64_t size)
{
    if (count && size > UINT64_MAX / count)
        return NULL;

    uint64_t total = count * size;

    void *pointer = kmalloc(total);

    if (pointer)
        memory_zero(pointer, total);

    return pointer;
}

void kfree(void *pointer)
{
    if (!pointer || !initialized)
        return;

    uint64_t flags = spinlock_lock_irqsave(&heap_lock);

    if (heap_free_locked(pointer))
        heap_trim();

    spinlock_unlock_irqrestore(&heap_lock, flags);
}

void *krealloc(void *pointer, uint64_t new_size)
{
    if (!pointer)
        return kmalloc(new_size);

    if (new_size == 0)
    {
        kfree(pointer);
        return NULL;
    }

    uint64_t flags = spinlock_lock_irqsave(&heap_lock);

    uintptr_t address = (uintptr_t)pointer;

    if (address < heap_base + BLOCK_HEADER_SIZE + TAG_SIZE ||
        address >= heap_base + heap_pages * PAGE_SIZE)
    {
        spinlock_unlock_irqrestore(&heap_lock, flags);
        return NULL;
    }

    struct heap_tag *tag =
        (struct heap_tag *)(address - TAG_SIZE);

    if (tag->magic != TAG_MAGIC)
    {
        spinlock_unlock_irqrestore(&heap_lock, flags);
        return NULL;
    }

    uint64_t old_size = tag->requested;
    uint64_t alignment = tag->alignment;

    void *new_pointer =
        heap_alloc_locked(new_size, alignment);

    if (!new_pointer)
    {
        spinlock_unlock_irqrestore(&heap_lock, flags);
        return NULL;
    }

    uint64_t copy_size =
        old_size < new_size ? old_size : new_size;

    memory_copy(new_pointer, pointer, copy_size);

    heap_free_locked(pointer);
    heap_trim();

    spinlock_unlock_irqrestore(&heap_lock, flags);

    return new_pointer;
}

/* ---------------------------------------------------------
   Heap statistics
   --------------------------------------------------------- */

uint64_t kheap_get_used(void)
{
    if (!initialized)
        return 0;

    uint64_t flags = spinlock_lock_irqsave(&heap_lock);

    uint64_t used = 0;

    for (struct heap_block *block = first_block;
         block;
         block = block->next)
    {
        if (!block->free)
            used += block->size;
    }

    spinlock_unlock_irqrestore(&heap_lock, flags);

    return used;
}

uint64_t kheap_get_free(void)
{
    if (!initialized)
        return 0;

    uint64_t flags = spinlock_lock_irqsave(&heap_lock);

    uint64_t free = 0;

    for (struct heap_block *block = first_block;
         block;
         block = block->next)
    {
        if (block->free)
            free += block->size;
    }

    spinlock_unlock_irqrestore(&heap_lock, flags);

    return free;
}

uint64_t kheap_get_committed(void)
{
    if (!initialized)
        return 0;

    uint64_t flags = spinlock_lock_irqsave(&heap_lock);

    uint64_t result = heap_pages * PAGE_SIZE;

    spinlock_unlock_irqrestore(&heap_lock, flags);

    return result;
}