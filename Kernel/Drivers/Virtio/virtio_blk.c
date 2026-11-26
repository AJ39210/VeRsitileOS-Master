#include <stdbool.h>
#include <stdint.h>

#include "block.h"
#include "pci.h"
#include "serial.h"
#include "virtio_blk.h"
#include "virtio_legacy.h"
#include "virtio_pci.h"

#define VIRTIO_BLK_DEVICE_LIMIT 4
#define VIRTIO_STATUS_ACKNOWLEDGE 0x01
#define VIRTIO_STATUS_DRIVER 0x02
#define VIRTIO_STATUS_DRIVER_OK 0x04
#define VIRTIO_STATUS_FAILED 0x80
#define VIRTIO_BLK_F_RO (1U << 5)
#define VIRTIO_BLK_T_IN 0
#define VIRTIO_BLK_T_OUT 1
#define VIRTIO_BLK_S_OK 0
#define VIRTIO_BLK_S_IOERR 1
#define VIRTIO_BLK_MAX_SECTORS 8
#define VIRTIO_BLK_TIMEOUT 10000000U

typedef struct __attribute__((packed))
{
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} virtio_blk_request_t;

typedef struct
{
    uint16_t io_base;
    uint16_t queue_index;
    uint64_t sector_count;
    bool writable;
    bool modern;
    bool used;
    const virtio_pci_device_t *transport;
    virtio_legacy_queue_t queue;
    uint64_t request_physical;
    uint8_t *request_memory;
    uint64_t data_physical;
    uint8_t *data_memory;
    uint64_t status_physical;
    uint8_t *status_memory;
    uint16_t available_cursor;
} virtio_blk_device_t;

static virtio_blk_device_t devices[VIRTIO_BLK_DEVICE_LIMIT];
static uint32_t device_count;

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
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

static inline uint32_t inl(uint16_t port)
{
    uint32_t value;
    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline uint64_t inq(uint16_t port)
{
    uint64_t low = inl(port);
    return low | ((uint64_t)inl(port + 4) << 32);
}

static bool request_wait(virtio_blk_device_t *device, uint16_t old_used)
{
    for (uint32_t iteration = 0; iteration < VIRTIO_BLK_TIMEOUT; iteration++)
    {
        __asm__ volatile ("mfence" ::: "memory");
        if (*device->queue.used_index != old_used)
            return device->status_memory[0] == VIRTIO_BLK_S_OK;
        __asm__ volatile ("pause");
    }
    return false;
}

static bool transfer_one(
    virtio_blk_device_t *device,
    uint64_t sector,
    uint16_t sectors,
    uint8_t *buffer,
    bool write
)
{
    if (!device || !device->used || !sectors ||
        sectors > VIRTIO_BLK_MAX_SECTORS ||
        (write && !device->writable))
        return false;

    uint32_t bytes = (uint32_t)sectors * BLOCK_SECTOR_SIZE;
    virtio_blk_request_t *request =
        (virtio_blk_request_t *)device->request_memory;
    *request = (virtio_blk_request_t){
        .type = write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN,
        .sector = sector
    };
    if (write)
        virtio_legacy_memory_copy(device->data_memory, buffer, bytes);
    device->status_memory[0] = VIRTIO_BLK_S_IOERR;

    virtio_descriptor_t *descriptors = device->queue.descriptors;
    descriptors[0] = (virtio_descriptor_t){
        .address = device->request_physical,
        .length = sizeof(*request),
        .flags = VIRTIO_DESC_F_NEXT,
        .next = 1
    };
    descriptors[1] = (virtio_descriptor_t){
        .address = device->data_physical,
        .length = bytes,
        .flags = VIRTIO_DESC_F_NEXT |
            (write ? 0 : VIRTIO_DESC_F_WRITE),
        .next = 2
    };
    descriptors[2] = (virtio_descriptor_t){
        .address = device->status_physical,
        .length = 1,
        .flags = VIRTIO_DESC_F_WRITE
    };

    uint16_t old_used = *device->queue.used_index;
    uint16_t available = device->available_cursor++;
    device->queue.available_ring[
        available % device->queue.size] = 0;
    __asm__ volatile ("mfence" ::: "memory");
    *device->queue.available_index = available + 1;
    if (device->modern)
        virtio_pci_modern_notify(
            device->transport, device->queue_index);
    else
        virtio_legacy_notify(device->io_base, device->queue_index);
    if (!request_wait(device, old_used))
        return false;
    if (!write)
        virtio_legacy_memory_copy(buffer, device->data_memory, bytes);
    return true;
}

static bool device_read(
    void *context,
    uint64_t lba,
    uint32_t count,
    void *buffer
)
{
    virtio_blk_device_t *device = context;
    uint8_t *output = buffer;
    while (count)
    {
        uint16_t batch = count > VIRTIO_BLK_MAX_SECTORS
            ? VIRTIO_BLK_MAX_SECTORS
            : (uint16_t)count;
        if (!transfer_one(device, lba, batch, output, false))
            return false;
        lba += batch;
        count -= batch;
        output += (uint32_t)batch * BLOCK_SECTOR_SIZE;
    }
    return true;
}

static bool device_write(
    void *context,
    uint64_t lba,
    uint32_t count,
    const void *buffer
)
{
    virtio_blk_device_t *device = context;
    const uint8_t *input = buffer;
    while (count)
    {
        uint16_t batch = count > VIRTIO_BLK_MAX_SECTORS
            ? VIRTIO_BLK_MAX_SECTORS
            : (uint16_t)count;
        if (!transfer_one(device, lba, batch, (uint8_t *)input, true))
            return false;
        lba += batch;
        count -= batch;
        input += (uint32_t)batch * BLOCK_SECTOR_SIZE;
    }
    return true;
}

static bool initialize_device(
    const virtio_pci_device_t *transport,
    virtio_blk_device_t *device
)
{
    const pci_device_t *pci = transport->pci_device;
    uint16_t io_base = transport->io_base;
    if (!virtio_pci_enable_device(pci))
        return false;
    device->transport = transport;
    device->modern = transport->modern;
    device->io_base = io_base;
    uint32_t features;
    if (device->modern)
    {
        if (!virtio_pci_modern_init(
                transport,
                0,
                1,
                &features,
                NULL))
            return false;
        if (transport->device_config == NULL)
            return false;
    }
    else
    {
        outb(io_base + 0x12, 0);
        outb(io_base + 0x12, VIRTIO_STATUS_ACKNOWLEDGE);
        outb(io_base + 0x12,
            VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
        features = inl(io_base);
    }
    device->writable = (features & VIRTIO_BLK_F_RO) == 0;
    if (!device->modern)
        outl(io_base + 0x04, features & VIRTIO_BLK_F_RO);

    if (!(device->modern
            ? virtio_pci_modern_queue_init(
                transport, 0, &device->queue)
            : virtio_legacy_queue_init(
                io_base, 0, &device->queue)) ||
        device->queue.size < 3)
        return false;
    if (!virtio_legacy_alloc_page(
            &device->request_physical, &device->request_memory) ||
        !virtio_legacy_alloc_page(
            &device->data_physical, &device->data_memory) ||
        !virtio_legacy_alloc_page(
            &device->status_physical, &device->status_memory))
        return false;

    device->sector_count = device->modern
        ? *(volatile uint64_t *)transport->device_config
        : inq(io_base + 0x14);
    if (!device->sector_count)
        return false;
    if (device->modern)
        return virtio_pci_modern_driver_ok(transport);
    outb(io_base + 0x12,
        VIRTIO_STATUS_ACKNOWLEDGE |
        VIRTIO_STATUS_DRIVER |
        VIRTIO_STATUS_DRIVER_OK);
    return (inb(io_base + 0x12) &
        VIRTIO_STATUS_DRIVER_OK) != 0;
}

bool virtio_blk_init(void)
{
    device_count = 0;
    for (uint32_t ordinal = 0;
         ordinal < VIRTIO_BLK_DEVICE_LIMIT;
         ordinal++)
    {
        const virtio_pci_device_t *transport =
            virtio_pci_find_device(2, ordinal);
        if (!transport)
            break;
        virtio_blk_device_t *device = &devices[device_count];
        *device = (virtio_blk_device_t){0};
        if (!initialize_device(transport, device))
        {
            if (!transport->modern && transport->io_base)
                outb(transport->io_base + 0x12, VIRTIO_STATUS_FAILED);
            serial_log(
                SERIAL_LOG_WARN,
                "VirtIO block device initialization failed");
            continue;
        }

        block_device_t block = {0};
        block.name[0] = 'v';
        block.name[1] = 'd';
        block.name[2] = (char)('0' + device_count);
        block.sector_count = device->sector_count;
        block.sector_size = BLOCK_SECTOR_SIZE;
        block.writable = device->writable;
        block.context = device;
        block.read = device_read;
        block.write = device->writable ? device_write : NULL;
        if (!block_register(&block))
        {
            serial_log(SERIAL_LOG_WARN,
                "Could not register VirtIO block device");
            continue;
        }
        device->used = true;
        serial_log_begin(SERIAL_LOG_INFO);
        serial_write("VirtIO block device ");
        serial_write(block.name);
        serial_write(" ready (");
        serial_write_dec(device->sector_count);
        serial_write(" sectors)");
        serial_log_end();
        device_count++;
    }
    return true;
}

uint32_t virtio_blk_device_count(void)
{
    return device_count;
}
