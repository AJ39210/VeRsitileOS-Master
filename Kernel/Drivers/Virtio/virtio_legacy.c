#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "paging.h"
#include "pmm.h"
#include "virtio_legacy.h"

static inline void outw(uint16_t port, uint16_t value)
{
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline void outl(uint16_t port, uint32_t value)
{
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t value;
    __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

void virtio_legacy_memory_zero(void *destination, uint64_t size)
{
    volatile uint8_t *bytes = destination;
    for (uint64_t i = 0; i < size; i++)
        bytes[i] = 0;
}

void virtio_legacy_memory_copy(
    void *destination,
    const void *source,
    uint64_t size
)
{
    volatile uint8_t *output = destination;
    const volatile uint8_t *input = source;
    for (uint64_t i = 0; i < size; i++)
        output[i] = input[i];
}

bool virtio_legacy_alloc_page(uint64_t *physical, uint8_t **virtual_address)
{
    if (!physical || !virtual_address)
        return false;
    *physical = pmm_alloc_page();
    if (!*physical)
        return false;
    *virtual_address = paging_phys_to_virt(*physical);
    if (!*virtual_address)
    {
        pmm_free_page(*physical);
        *physical = 0;
        return false;
    }
    virtio_legacy_memory_zero(
        *virtual_address, VIRTIO_LEGACY_PAGE_SIZE);
    return true;
}

void virtio_legacy_free_pages(uint64_t physical, uint64_t page_count)
{
    for (uint64_t i = 0; i < page_count; i++)
        pmm_free_page(physical + i * VIRTIO_LEGACY_PAGE_SIZE);
}

bool virtio_split_queue_allocate(
    virtio_legacy_queue_t *queue,
    uint16_t size
)
{
    if (!queue || !size || size > VIRTIO_LEGACY_QUEUE_LIMIT)
        return false;
    *queue = (virtio_legacy_queue_t){0};
    uint64_t descriptor_bytes =
        (uint64_t)size * sizeof(virtio_descriptor_t);
    uint64_t available_bytes = 4 + (uint64_t)size * sizeof(uint16_t);
    uint64_t used_offset =
        (descriptor_bytes + available_bytes +
            VIRTIO_LEGACY_PAGE_SIZE - 1) &
        ~(uint64_t)(VIRTIO_LEGACY_PAGE_SIZE - 1);
    uint64_t used_bytes =
        4 + (uint64_t)size * sizeof(virtio_used_element_t);
    uint64_t total = used_offset + used_bytes;
    uint64_t page_count =
        (total + VIRTIO_LEGACY_PAGE_SIZE - 1) /
        VIRTIO_LEGACY_PAGE_SIZE;
    uint64_t physical = pmm_alloc_contiguous_pages(page_count);
    if (!physical)
        return false;
    uint8_t *memory = paging_phys_to_virt(physical);
    if (!memory)
    {
        virtio_legacy_free_pages(physical, page_count);
        return false;
    }
    virtio_legacy_memory_zero(
        memory, page_count * VIRTIO_LEGACY_PAGE_SIZE);

    queue->size = size;
    queue->physical = physical;
    queue->page_count = page_count;
    queue->memory = memory;
    queue->descriptors = (virtio_descriptor_t *)memory;
    queue->available_index =
        (volatile uint16_t *)(memory + descriptor_bytes + 2);
    queue->available_ring =
        (volatile uint16_t *)(memory + descriptor_bytes + 4);
    queue->used_index =
        (volatile uint16_t *)(memory + used_offset + 2);
    queue->used_ring =
        (volatile virtio_used_element_t *)(memory + used_offset + 4);
    queue->used_cursor = 0;
    return true;
}

bool virtio_legacy_queue_init(
    uint16_t io_base,
    uint16_t index,
    virtio_legacy_queue_t *queue
)
{
    if (!io_base || !queue)
        return false;
    *queue = (virtio_legacy_queue_t){0};
    outw(io_base + 0x0E, index);
    uint16_t size = inw(io_base + 0x0C);
    if (!virtio_split_queue_allocate(queue, size))
        return false;

    outl(io_base + 0x08,
        (uint32_t)(queue->physical / VIRTIO_LEGACY_PAGE_SIZE));
    if (inw(io_base + 0x0E) != index)
    {
        virtio_legacy_queue_release(io_base, index, queue);
        return false;
    }
    return true;
}

void virtio_legacy_queue_release(
    uint16_t io_base,
    uint16_t index,
    virtio_legacy_queue_t *queue
)
{
    if (!queue || !queue->physical || !queue->page_count)
        return;
    outw(io_base + 0x0E, index);
    outl(io_base + 0x08, 0);
    virtio_legacy_free_pages(queue->physical, queue->page_count);
    *queue = (virtio_legacy_queue_t){0};
}

void virtio_legacy_notify(uint16_t io_base, uint16_t queue_index)
{
    __asm__ volatile ("mfence" ::: "memory");
    outw(io_base + 0x10, queue_index);
}
