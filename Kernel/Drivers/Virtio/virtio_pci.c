#include <stdbool.h>
#include <stdint.h>

#include "paging.h"
#include "pci.h"
#include "serial.h"
#include "virtio_legacy.h"
#include "virtio_pci.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA 0xCFC
#define VIRTIO_VENDOR_ID 0x1AF4

static virtio_pci_device_t devices[VIRTIO_PCI_DEVICE_LIMIT];
static uint32_t device_count;
static bool initialized;

static inline void outl(uint16_t port, uint32_t value)
{
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t value;
    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static uint32_t config_read32(const pci_device_t *device, uint8_t offset)
{
    uint32_t address = 0x80000000U |
        ((uint32_t)device->bus << 16) |
        ((uint32_t)device->device << 11) |
        ((uint32_t)device->function << 8) |
        (offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

static uint8_t config_read8(const pci_device_t *device, uint8_t offset)
{
    uint32_t value = config_read32(device, offset);
    return (uint8_t)(value >> ((offset & 3) * 8));
}

static uint64_t pci_bar_address(
    const pci_device_t *device,
    uint8_t index
)
{
    if (!device || index >= device->bar_count)
        return 0;
    uint32_t bar = device->bars[index];
    if (!bar || (bar & 1))
        return 0;
    uint64_t address = bar & ~0xFULL;
    if (((bar >> 1) & 3) == 2)
    {
        if (index + 1 >= device->bar_count)
            return 0;
        address |= (uint64_t)device->bars[index + 1] << 32;
    }
    return address;
}

static uint16_t config_read16(const pci_device_t *device, uint8_t offset)
{
    uint32_t value = config_read32(device, offset);
    return (uint16_t)(value >> ((offset & 2) * 8));
}

static void config_write16(
    const pci_device_t *device,
    uint8_t offset,
    uint16_t value
)
{
    uint32_t address = 0x80000000U |
        ((uint32_t)device->bus << 16) |
        ((uint32_t)device->device << 11) |
        ((uint32_t)device->function << 8) |
        (offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS, address);
    __asm__ volatile (
        "outw %0, %1"
        :
        : "a"(value), "Nd"((uint16_t)(PCI_CONFIG_DATA + (offset & 2)))
    );
}

static bool known_device_type(uint16_t type)
{
    return type >= 1 && type <= 0x3F;
}

static uint16_t resolve_device_type(const pci_device_t *device)
{
    if (device->device_id == 0x1000)
        return 1;
    if (device->device_id == 0x1001)
        return 2;
    if (device->device_id >= 0x1010 && device->device_id <= 0x103F)
        return (uint16_t)(device->device_id - 0x1000);
    if (device->device_id >= 0x1040 && device->device_id <= 0x107F)
        return (uint16_t)(device->device_id - 0x1040);
    if (device->device_id == 0x1010 &&
        known_device_type(device->subsystem_id))
        return device->subsystem_id;
    return 0;
}

static void read_capabilities(
    const pci_device_t *device,
    virtio_pci_device_t *entry
)
{
    if (!(device->status & (1U << 4)))
        return;
    uint8_t offset = config_read8(device, 0x34) & ~3U;
    uint8_t visited = 0;
    while (offset >= 0x40 && offset <= 0xFC &&
           visited++ < 48 &&
           entry->capability_count < VIRTIO_PCI_CAPABILITY_LIMIT)
    {
        uint8_t id = config_read8(device, offset);
        uint8_t next = config_read8(device, offset + 1) & ~3U;
        if (id == 0x09 && config_read8(device, offset + 2) >= 16)
        {
            uint8_t type = config_read8(device, offset + 3);
            if (type >= 1 && type <= 4)
            {
                uint32_t bar_offset = config_read32(
                    device, (uint8_t)(offset + 4));
                uint32_t region_offset = config_read32(
                    device, (uint8_t)(offset + 8));
                uint32_t region_length = config_read32(
                    device, (uint8_t)(offset + 12));
                if ((bar_offset & 0xFF) < PCI_BAR_LIMIT &&
                    region_length)
                {
                    uint8_t bar = (uint8_t)(bar_offset & 0xFF);
                    uint64_t base = pci_bar_address(device, bar);
                    volatile uint8_t *mapped = base
                        ? ioremap(
                            base + region_offset,
                            region_length)
                        : NULL;
                    if (!mapped)
                    {
                        offset = next;
                        if (!offset)
                            break;
                        continue;
                    }
                    uint8_t index = entry->capability_count++;
                    entry->capabilities[index].type = type;
                    entry->capabilities[index].bar = bar;
                    entry->capabilities[index].offset = region_offset;
                    entry->capabilities[index].length = region_length;
                    if (type == 2 &&
                        config_read8(device, (uint8_t)(offset + 2)) >= 20)
                        entry->capabilities[index].notify_multiplier =
                            config_read32(device, (uint8_t)(offset + 16));
                    if (type == 1)
                        entry->common_config = mapped;
                    else if (type == 2)
                    {
                        entry->notify_config = mapped;
                        entry->notify_length = region_length;
                        entry->notify_multiplier =
                            entry->capabilities[index].notify_multiplier;
                    }
                    else if (type == 4)
                        entry->device_config = mapped;
                }
            }
        }
        if (!next || next == offset)
            break;
        offset = next;
    }
}

uint32_t virtio_pci_device_count(void)
{
    if (!initialized)
    {
        device_count = 0;
        for (uint32_t i = 0; i < pci_device_count() &&
             device_count < VIRTIO_PCI_DEVICE_LIMIT; i++)
        {
            const pci_device_t *pci = pci_device_get(i);
            if (!pci || pci->vendor_id != VIRTIO_VENDOR_ID)
                continue;
            uint16_t type = resolve_device_type(pci);
            if (!type)
                continue;
            virtio_pci_device_t *entry = &devices[device_count];
            *entry = (virtio_pci_device_t){
                .pci_device = pci,
                .device_type = type
            };
            if (pci->bar_count && (pci->bars[0] & 1))
            {
                uint32_t base = pci->bars[0] & ~3U;
                if (base >= 0x100 && base <= 0xFFE0)
                {
                    entry->legacy_io = true;
                    entry->io_base = (uint16_t)base;
                }
            }
            (void)virtio_pci_enable_device(pci);
            read_capabilities(pci, entry);
            entry->modern = entry->common_config && entry->notify_config;
            serial_log_begin(SERIAL_LOG_INFO);
            serial_write("VirtIO PCI device id ");
            serial_write_hex(pci->device_id);
            serial_write(" type ");
            serial_write_dec(type);
            serial_write(entry->legacy_io ? " legacy I/O " : " modern ");
            serial_write_hex(entry->io_base);
            serial_log_end();
            device_count++;
        }
        initialized = true;
        if (device_count)
        {
            serial_log_begin(SERIAL_LOG_INFO);
            serial_write("VirtIO PCI transport discovered ");
            serial_write_dec(device_count);
            serial_write(" device(s)");
            serial_log_end();
        }
    }
    return device_count;
}

const virtio_pci_device_t *virtio_pci_device_get(uint32_t index)
{
    (void)virtio_pci_device_count();
    return index < device_count ? &devices[index] : NULL;
}

const virtio_pci_device_t *virtio_pci_find_device(
    uint16_t device_type,
    uint32_t index
)
{
    uint32_t found = 0;
    uint32_t count = virtio_pci_device_count();
    for (uint32_t i = 0; i < count; i++)
    {
        if (devices[i].device_type != device_type)
            continue;
        if (found++ == index)
            return &devices[i];
    }
    return NULL;
}

const pci_device_t *virtio_pci_find_legacy(
    uint16_t device_type,
    uint32_t index,
    uint16_t *io_base
)
{
    if (!io_base)
        return NULL;
    uint32_t found = 0;
    uint32_t count = virtio_pci_device_count();
    for (uint32_t i = 0; i < count; i++)
    {
        const virtio_pci_device_t *entry = &devices[i];
        if (entry->device_type != device_type || !entry->legacy_io)
            continue;
        if (found++ != index)
            continue;
        *io_base = entry->io_base;
        return entry->pci_device;
    }
    return NULL;
}

bool virtio_pci_enable_device(const pci_device_t *device)
{
    if (!device || device->vendor_id != VIRTIO_VENDOR_ID)
        return false;
    uint16_t command = config_read16(device, 0x04);
    command |= (1U << 0) | (1U << 1) | (1U << 2);
    config_write16(device, 0x04, command);
    return (config_read16(device, 0x04) &
        ((1U << 0) | (1U << 1) | (1U << 2))) ==
        ((1U << 0) | (1U << 1) | (1U << 2));
}

bool virtio_pci_modern_init(
    const virtio_pci_device_t *device,
    uint32_t accepted_features_low,
    uint32_t accepted_features_high,
    uint32_t *device_features_low,
    uint32_t *device_features_high
)
{
    if (!device || !device->modern ||
        !device->common_config)
        return false;
    volatile uint8_t *common = device->common_config;
    volatile uint8_t *status = common + 20;
    *status = 0;
    __asm__ volatile ("mfence" ::: "memory");
    *status = 1;
    *status = 3;

    *(volatile uint32_t *)(common + 0) = 0;
    uint32_t feature_low = *(volatile uint32_t *)(common + 4);
    *(volatile uint32_t *)(common + 0) = 1;
    uint32_t feature_high = *(volatile uint32_t *)(common + 4);

    *(volatile uint32_t *)(common + 8) = 0;
    *(volatile uint32_t *)(common + 12) = accepted_features_low;
    *(volatile uint32_t *)(common + 8) = 1;
    *(volatile uint32_t *)(common + 12) = accepted_features_high;
    *status = 11;
    __asm__ volatile ("mfence" ::: "memory");
    if (!(*status & 8))
    {
        *status |= 0x80;
        return false;
    }

    if (device_features_low)
        *device_features_low = feature_low;
    if (device_features_high)
        *device_features_high = feature_high;
    return true;
}

bool virtio_pci_modern_queue_init(
    const virtio_pci_device_t *device,
    uint16_t index,
    virtio_legacy_queue_t *queue
)
{
    if (!device || !device->modern || !queue)
        return false;
    volatile uint8_t *common = device->common_config;
    *(volatile uint16_t *)(common + 22) = index;
    uint16_t size = *(volatile uint16_t *)(common + 24);
    if (!virtio_split_queue_allocate(queue, size))
        return false;

    uint64_t descriptor_bytes =
        (uint64_t)queue->size * sizeof(virtio_descriptor_t);
    uint64_t available_bytes =
        4 + (uint64_t)queue->size * sizeof(uint16_t);
    uint64_t used_offset =
        (descriptor_bytes + available_bytes +
            VIRTIO_LEGACY_PAGE_SIZE - 1) &
        ~(uint64_t)(VIRTIO_LEGACY_PAGE_SIZE - 1);
    uint64_t available_address = queue->physical +
        descriptor_bytes;
    uint64_t used_address = queue->physical + used_offset;

    *(volatile uint16_t *)(common + 24) = queue->size;
    *(volatile uint64_t *)(common + 32) = queue->physical;
    *(volatile uint64_t *)(common + 40) = available_address;
    *(volatile uint64_t *)(common + 48) = used_address;
    __asm__ volatile ("mfence" ::: "memory");
    *(volatile uint16_t *)(common + 28) = 1;
    return *(volatile uint16_t *)(common + 28) == 1;
}

bool virtio_pci_modern_driver_ok(const virtio_pci_device_t *device)
{
    if (!device || !device->modern || !device->common_config)
        return false;
    volatile uint8_t *status = device->common_config + 20;
    *status |= 4;
    __asm__ volatile ("mfence" ::: "memory");
    return (*status & 4) != 0;
}

void virtio_pci_modern_notify(
    const virtio_pci_device_t *device,
    uint16_t queue_index
)
{
    if (!device || !device->modern || !device->notify_config)
        return;
    volatile uint8_t *common = device->common_config;
    *(volatile uint16_t *)(common + 22) = queue_index;
    uint16_t offset = *(volatile uint16_t *)(common + 30);
    uint64_t notify_offset =
        (uint64_t)offset * device->notify_multiplier;
    if (notify_offset + sizeof(uint16_t) > device->notify_length)
        return;
    __asm__ volatile ("mfence" ::: "memory");
    *(volatile uint16_t *)(device->notify_config + notify_offset) =
        queue_index;
}
