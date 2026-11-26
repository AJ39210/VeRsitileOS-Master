#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "paging.h"
#include "pci.h"
#include "pmm.h"
#include "serial.h"
#include "virtio_net.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA 0xCFC
#define VIRTIO_VENDOR_ID 0x1AF4
#define VIRTIO_NET_LEGACY_DEVICE_ID 0x1000
#define VIRTIO_NET_QUEUE_LIMIT 256
#define VIRTIO_NET_RX_BUFFER_COUNT 16
#define VIRTIO_NET_TX_BUFFER_COUNT 8
#define VIRTIO_NET_BUFFER_SIZE 2048
#define VIRTIO_NET_HEADER_SIZE 10
#define VIRTIO_PAGE_SIZE 4096

#define VIRTIO_STATUS_ACKNOWLEDGE 0x01
#define VIRTIO_STATUS_DRIVER 0x02
#define VIRTIO_STATUS_DRIVER_OK 0x04
#define VIRTIO_STATUS_FEATURES_OK 0x08
#define VIRTIO_STATUS_FAILED 0x80

#define VIRTIO_NET_F_MAC (1U << 5)
#define VIRTIO_NET_F_STATUS (1U << 16)
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

typedef struct
{
    uint16_t size;
    uint64_t physical;
    uint8_t *memory;
    virtio_descriptor_t *descriptors;
    volatile uint16_t *available_index;
    volatile uint16_t *available_ring;
    volatile uint16_t *used_index;
    volatile virtio_used_element_t *used_ring;
    uint16_t used_cursor;
} virtio_queue_t;

static uint16_t io_base;
static uint32_t device_features;
static uint8_t mac_address[6];
static virtio_queue_t receive_queue;
static virtio_queue_t transmit_queue;
static uint64_t receive_buffer_physical[VIRTIO_NET_RX_BUFFER_COUNT];
static uint8_t *receive_buffers[VIRTIO_NET_RX_BUFFER_COUNT];
static uint64_t transmit_buffer_physical[VIRTIO_NET_TX_BUFFER_COUNT];
static uint8_t *transmit_buffers[VIRTIO_NET_TX_BUFFER_COUNT];
static uint16_t receive_buffer_count;
static uint16_t transmit_buffer_count;
static uint16_t transmit_cursor;
static bool initialized;

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline void outw(uint16_t port, uint16_t value)
{
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline void outl(uint16_t port, uint32_t value)
{
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t value;
    __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t value;
    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void memory_zero(void *destination, size_t length)
{
    volatile uint8_t *bytes = destination;
    for (size_t i = 0; i < length; i++)
        bytes[i] = 0;
}

static void memory_copy(void *destination, const void *source, size_t length)
{
    volatile uint8_t *output = destination;
    const volatile uint8_t *input = source;
    for (size_t i = 0; i < length; i++)
        output[i] = input[i];
}

static void pci_write16(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint8_t offset,
    uint16_t value
)
{
    uint32_t address = 0x80000000U |
        ((uint32_t)bus << 16) |
        ((uint32_t)device << 11) |
        ((uint32_t)function << 8) |
        (offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS, address);
    outw(PCI_CONFIG_DATA + (offset & 2), value);
}

static const pci_device_t *find_legacy_device(void)
{
    for (uint32_t i = 0; i < pci_device_count(); i++)
    {
        const pci_device_t *device = pci_device_get(i);
        if (device &&
            device->vendor_id == VIRTIO_VENDOR_ID &&
            device->device_id == VIRTIO_NET_LEGACY_DEVICE_ID &&
            device->class_code == 0x02 &&
            (device->bars[0] & 1))
            return device;
    }
    return NULL;
}

static bool allocate_dma_page(uint64_t *physical, uint8_t **virtual_address)
{
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
    memory_zero(*virtual_address, VIRTIO_PAGE_SIZE);
    return true;
}

static bool queue_initialize(virtio_queue_t *queue, uint16_t index)
{
    outw(io_base + 0x0E, index);
    uint16_t queue_size = inw(io_base + 0x0C);
    if (!queue_size || queue_size > VIRTIO_NET_QUEUE_LIMIT)
        return false;

    uint64_t descriptor_bytes =
        (uint64_t)queue_size * sizeof(virtio_descriptor_t);
    uint64_t available_bytes = 4 + (uint64_t)queue_size * sizeof(uint16_t);
    uint64_t used_offset =
        (descriptor_bytes + available_bytes + VIRTIO_PAGE_SIZE - 1) &
        ~(uint64_t)(VIRTIO_PAGE_SIZE - 1);
    uint64_t used_bytes =
        4 + (uint64_t)queue_size * sizeof(virtio_used_element_t);
    uint64_t allocation_bytes = used_offset + used_bytes;
    uint64_t page_count =
        (allocation_bytes + VIRTIO_PAGE_SIZE - 1) / VIRTIO_PAGE_SIZE;
    uint64_t physical =
        pmm_alloc_contiguous_pages(page_count);
    if (!physical)
        return false;
    uint8_t *memory = paging_phys_to_virt(physical);
    if (!memory)
    {
        for (uint64_t i = 0; i < page_count; i++)
            pmm_free_page(physical + i * VIRTIO_PAGE_SIZE);
        return false;
    }

    memory_zero(memory, (size_t)(page_count * VIRTIO_PAGE_SIZE));
    queue->size = queue_size;
    queue->physical = physical;
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
    outl(io_base + 0x08, (uint32_t)(physical / VIRTIO_PAGE_SIZE));
    return true;
}

static void queue_release(virtio_queue_t *queue)
{
    if (!queue->physical || !queue->size)
        return;
    uint64_t descriptor_bytes =
        (uint64_t)queue->size * sizeof(virtio_descriptor_t);
    uint64_t available_bytes = 4 + (uint64_t)queue->size * sizeof(uint16_t);
    uint64_t used_offset =
        (descriptor_bytes + available_bytes + VIRTIO_PAGE_SIZE - 1) &
        ~(uint64_t)(VIRTIO_PAGE_SIZE - 1);
    uint64_t used_bytes =
        4 + (uint64_t)queue->size * sizeof(virtio_used_element_t);
    uint64_t page_count =
        (used_offset + used_bytes + VIRTIO_PAGE_SIZE - 1) /
        VIRTIO_PAGE_SIZE;
    outw(io_base + 0x0E, (uint16_t)(
        queue == &receive_queue ? 0 : 1));
    outl(io_base + 0x08, 0);
    for (uint64_t i = 0; i < page_count; i++)
        pmm_free_page(queue->physical + i * VIRTIO_PAGE_SIZE);
    *queue = (virtio_queue_t){0};
}

static void release_buffers(void)
{
    for (uint16_t i = 0; i < receive_buffer_count; i++)
    {
        if (receive_buffer_physical[i])
            pmm_free_page(receive_buffer_physical[i]);
        receive_buffer_physical[i] = 0;
        receive_buffers[i] = NULL;
    }
    for (uint16_t i = 0; i < transmit_buffer_count; i++)
    {
        if (transmit_buffer_physical[i])
            pmm_free_page(transmit_buffer_physical[i]);
        transmit_buffer_physical[i] = 0;
        transmit_buffers[i] = NULL;
    }
    receive_buffer_count = 0;
    transmit_buffer_count = 0;
}

static bool initialize_receive_buffers(void)
{
    receive_buffer_count = receive_queue.size;
    if (receive_buffer_count > VIRTIO_NET_RX_BUFFER_COUNT)
        receive_buffer_count = VIRTIO_NET_RX_BUFFER_COUNT;

    for (uint16_t i = 0; i < receive_buffer_count; i++)
    {
        if (!allocate_dma_page(
                &receive_buffer_physical[i],
                &receive_buffers[i]))
            return false;
        receive_queue.descriptors[i] = (virtio_descriptor_t){
            .address = receive_buffer_physical[i],
            .length = VIRTIO_NET_BUFFER_SIZE,
            .flags = VIRTIO_DESC_F_WRITE,
            .next = 0
        };
        receive_queue.available_ring[i] = i;
    }

    __asm__ volatile ("mfence" ::: "memory");
    *receive_queue.available_index = receive_buffer_count;
    return true;
}

static bool initialize_transmit_buffers(void)
{
    transmit_buffer_count = transmit_queue.size;
    if (transmit_buffer_count > VIRTIO_NET_TX_BUFFER_COUNT)
        transmit_buffer_count = VIRTIO_NET_TX_BUFFER_COUNT;

    for (uint16_t i = 0; i < transmit_buffer_count; i++)
    {
        if (!allocate_dma_page(
                &transmit_buffer_physical[i],
                &transmit_buffers[i]))
            return false;
        transmit_queue.descriptors[i] = (virtio_descriptor_t){
            .address = transmit_buffer_physical[i],
            .length = 0,
            .flags = 0,
            .next = 0
        };
    }
    return true;
}

static bool initialization_failed(void)
{
    if (io_base)
    {
        outb(io_base + 0x12, VIRTIO_STATUS_FAILED);
        serial_log(
            SERIAL_LOG_WARN,
            "VirtIO legacy PCI network initialization failed");
    }
    release_buffers();
    queue_release(&transmit_queue);
    queue_release(&receive_queue);
    initialized = false;
    return false;
}

bool virtio_net_init(void)
{
    initialized = false;
    io_base = 0;
    memory_zero(&receive_queue, sizeof(receive_queue));
    memory_zero(&transmit_queue, sizeof(transmit_queue));
    memory_zero(receive_buffer_physical, sizeof(receive_buffer_physical));
    memory_zero(receive_buffers, sizeof(receive_buffers));
    memory_zero(transmit_buffer_physical, sizeof(transmit_buffer_physical));
    memory_zero(transmit_buffers, sizeof(transmit_buffers));
    receive_buffer_count = 0;
    transmit_buffer_count = 0;

    const pci_device_t *device = find_legacy_device();
    if (!device)
        return false;

    uint32_t io_bar = device->bars[0];
    uint16_t base = (uint16_t)(io_bar & ~3U);
    if (base < 0x100 || base > 0xFFE0)
    {
        serial_log(
            SERIAL_LOG_WARN,
            "VirtIO network device has an invalid I/O BAR");
        return false;
    }
    io_base = base;
    transmit_cursor = 0;

    uint16_t command = device->command | (1U << 0) | (1U << 2);
    pci_write16(device->bus, device->device, device->function, 0x04, command);

    outb(io_base + 0x12, 0);
    outb(io_base + 0x12, VIRTIO_STATUS_ACKNOWLEDGE);
    outb(io_base + 0x12,
        VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    device_features = inl(io_base);
    if (!(device_features & VIRTIO_NET_F_MAC))
        return initialization_failed();
    uint32_t accepted_features = VIRTIO_NET_F_MAC;
    if (device_features & VIRTIO_NET_F_STATUS)
        accepted_features |= VIRTIO_NET_F_STATUS;
    outl(io_base + 0x04, accepted_features);
    outb(io_base + 0x12,
        VIRTIO_STATUS_ACKNOWLEDGE |
        VIRTIO_STATUS_DRIVER |
        VIRTIO_STATUS_FEATURES_OK);
    if (!(inb(io_base + 0x12) & VIRTIO_STATUS_FEATURES_OK))
        return initialization_failed();

    for (uint32_t i = 0; i < sizeof(mac_address); i++)
        mac_address[i] = inb(io_base + 0x14 + i);

    if (!queue_initialize(&receive_queue, 0) ||
        !queue_initialize(&transmit_queue, 1) ||
        !receive_queue.size ||
        !transmit_queue.size ||
        !initialize_receive_buffers() ||
        !initialize_transmit_buffers())
        return initialization_failed();

    __asm__ volatile ("mfence" ::: "memory");
    outw(io_base + 0x10, 0);
    outb(io_base + 0x12,
        VIRTIO_STATUS_ACKNOWLEDGE |
        VIRTIO_STATUS_DRIVER |
        VIRTIO_STATUS_FEATURES_OK |
        VIRTIO_STATUS_DRIVER_OK);
    if (!(inb(io_base + 0x12) & VIRTIO_STATUS_DRIVER_OK))
        return initialization_failed();

    initialized = true;
    serial_log(SERIAL_LOG_INFO, "Initialized VirtIO legacy PCI network device");
    return true;
}

bool virtio_net_is_initialized(void)
{
    return initialized;
}

bool virtio_net_link_is_up(void)
{
    if (!initialized)
        return false;
    if (device_features & VIRTIO_NET_F_STATUS)
        return (inw(io_base + 0x1A) & 1) != 0;
    return true;
}

const uint8_t *virtio_net_mac_address(void)
{
    return initialized ? mac_address : NULL;
}

bool virtio_net_send(const void *packet, size_t length)
{
    if (!initialized || !packet || length < 14 ||
        length > VIRTIO_NET_BUFFER_SIZE - VIRTIO_NET_HEADER_SIZE)
        return false;

    uint16_t descriptor_index =
        transmit_cursor++ % transmit_buffer_count;
    uint8_t *buffer = transmit_buffers[descriptor_index];
    memory_zero(buffer, VIRTIO_NET_HEADER_SIZE);
    memory_copy(buffer + VIRTIO_NET_HEADER_SIZE, packet, length);
    transmit_queue.descriptors[descriptor_index].length =
        VIRTIO_NET_HEADER_SIZE + (uint32_t)length;

    uint16_t old_used_index = *transmit_queue.used_index;
    uint16_t available_index = *transmit_queue.available_index;
    transmit_queue.available_ring[
        available_index % transmit_queue.size] = descriptor_index;
    __asm__ volatile ("mfence" ::: "memory");
    *transmit_queue.available_index = available_index + 1;
    __asm__ volatile ("mfence" ::: "memory");
    outw(io_base + 0x10, 1);

    for (uint32_t timeout = 0;
         timeout < 5000000 &&
         *transmit_queue.used_index == old_used_index;
         timeout++)
        __asm__ volatile ("pause");
    return *transmit_queue.used_index != old_used_index;
}

size_t virtio_net_receive(void *packet, size_t capacity)
{
    if (!initialized || !packet || !capacity)
        return 0;
    if (receive_queue.used_cursor == *receive_queue.used_index)
        return 0;

    __asm__ volatile ("mfence" ::: "memory");
    virtio_used_element_t element =
        receive_queue.used_ring[
            receive_queue.used_cursor % receive_queue.size];
    receive_queue.used_cursor++;
    if (element.id >= receive_buffer_count)
        return 0;

    size_t packet_length = 0;
    if (element.length >= VIRTIO_NET_HEADER_SIZE &&
        element.length <= VIRTIO_NET_BUFFER_SIZE)
    {
        packet_length = element.length - VIRTIO_NET_HEADER_SIZE;
        if (packet_length > capacity)
            packet_length = capacity;
        memory_copy(
            packet,
            receive_buffers[element.id] + VIRTIO_NET_HEADER_SIZE,
            packet_length);
    }

    receive_queue.descriptors[element.id].length =
        VIRTIO_NET_BUFFER_SIZE;
    receive_queue.descriptors[element.id].flags = VIRTIO_DESC_F_WRITE;
    uint16_t available_index = *receive_queue.available_index;
    receive_queue.available_ring[
        available_index % receive_queue.size] = (uint16_t)element.id;
    __asm__ volatile ("mfence" ::: "memory");
    *receive_queue.available_index = available_index + 1;
    __asm__ volatile ("mfence" ::: "memory");
    outw(io_base + 0x10, 0);
    return packet_length;
}
