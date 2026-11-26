#ifndef VIRTIO_LEGACY_H
#define VIRTIO_LEGACY_H

#include <stdbool.h>
#include <stdint.h>

#define VIRTIO_LEGACY_QUEUE_LIMIT 256
#define VIRTIO_LEGACY_PAGE_SIZE 4096
#define VIRTIO_DESC_F_NEXT 1
#define VIRTIO_DESC_F_WRITE 2

typedef struct __attribute__((packed))
{
    uint64_t address;
    uint32_t length;
    uint16_t flags;
    uint16_t next;
} virtio_descriptor_t;

typedef struct __attribute__((packed))
{
    uint32_t id;
    uint32_t length;
} virtio_used_element_t;

typedef struct virtio_legacy_queue
{
    uint16_t size;
    uint64_t physical;
    uint64_t page_count;
    uint8_t *memory;
    virtio_descriptor_t *descriptors;
    volatile uint16_t *available_index;
    volatile uint16_t *available_ring;
    volatile uint16_t *used_index;
    volatile virtio_used_element_t *used_ring;
    uint16_t used_cursor;
} virtio_legacy_queue_t;

struct virtio_legacy_queue;

bool virtio_split_queue_allocate(
    virtio_legacy_queue_t *queue,
    uint16_t size
);
bool virtio_legacy_queue_init(
    uint16_t io_base,
    uint16_t index,
    virtio_legacy_queue_t *queue
);
void virtio_legacy_queue_release(
    uint16_t io_base,
    uint16_t index,
    virtio_legacy_queue_t *queue
);
bool virtio_legacy_alloc_page(uint64_t *physical, uint8_t **virtual_address);
void virtio_legacy_free_pages(uint64_t physical, uint64_t page_count);
void virtio_legacy_notify(uint16_t io_base, uint16_t queue_index);
void virtio_legacy_memory_copy(
    void *destination,
    const void *source,
    uint64_t size
);
void virtio_legacy_memory_zero(void *destination, uint64_t size);

#endif
