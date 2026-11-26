#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ata_pio.h"
#include "block.h"
#include "serial.h"

#define ATA_DEVICE_LIMIT 4
#define ATA_PIO_TIMEOUT 1000000
#define ATA_LBA28_LIMIT 0x10000000ULL

#define ATA_REG_DATA 0
#define ATA_REG_ERROR 1
#define ATA_REG_SECTOR_COUNT 2
#define ATA_REG_LBA_LOW 3
#define ATA_REG_LBA_MID 4
#define ATA_REG_LBA_HIGH 5
#define ATA_REG_DRIVE 6
#define ATA_REG_STATUS 7
#define ATA_REG_COMMAND 7

#define ATA_CMD_IDENTIFY 0xEC
#define ATA_CMD_READ_SECTORS 0x20
#define ATA_CMD_WRITE_SECTORS 0x30
#define ATA_CMD_CACHE_FLUSH 0xE7

#define ATA_STATUS_ERR 0x01
#define ATA_STATUS_DRQ 0x08
#define ATA_STATUS_DF 0x20
#define ATA_STATUS_BSY 0x80

typedef struct
{
    uint16_t io_base;
    uint16_t control_base;
    uint64_t sector_count;
    bool slave;
    bool online;
    bool writable;
} ata_device_t;

static ata_device_t devices[ATA_DEVICE_LIMIT];
static bool channel_busy[2];

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

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline void outw(uint16_t port, uint16_t value)
{
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static void ata_delay(ata_device_t *device)
{
    (void)inb(device->control_base);
    (void)inb(device->control_base);
    (void)inb(device->control_base);
    (void)inb(device->control_base);
}

static bool ata_wait_not_busy(ata_device_t *device)
{
    for (uint32_t i = 0; i < ATA_PIO_TIMEOUT; i++)
    {
        uint8_t status = inb(device->io_base + ATA_REG_STATUS);
        if (status == 0 || status == 0xFF)
            return false;
        if (!(status & ATA_STATUS_BSY))
            return !(status & (ATA_STATUS_ERR | ATA_STATUS_DF));
        __asm__ volatile ("pause");
    }
    return false;
}

static bool ata_wait_data(ata_device_t *device)
{
    for (uint32_t i = 0; i < ATA_PIO_TIMEOUT; i++)
    {
        uint8_t status = inb(device->io_base + ATA_REG_STATUS);
        if (status == 0 || status == 0xFF ||
            (status & (ATA_STATUS_ERR | ATA_STATUS_DF)))
            return false;
        if (!(status & ATA_STATUS_BSY) && (status & ATA_STATUS_DRQ))
            return true;
        __asm__ volatile ("pause");
    }
    return false;
}

static bool ata_probe(ata_device_t *device)
{
    outb(device->io_base + ATA_REG_DRIVE,
        (uint8_t)(0xA0 | (device->slave ? 0x10 : 0)));
    ata_delay(device);
    outb(device->io_base + ATA_REG_SECTOR_COUNT, 0);
    outb(device->io_base + ATA_REG_LBA_LOW, 0);
    outb(device->io_base + ATA_REG_LBA_MID, 0);
    outb(device->io_base + ATA_REG_LBA_HIGH, 0);
    outb(device->io_base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

    uint8_t status = inb(device->io_base + ATA_REG_STATUS);
    if (status == 0 || status == 0xFF || !ata_wait_not_busy(device))
        return false;
    if (inb(device->io_base + ATA_REG_LBA_MID) ||
        inb(device->io_base + ATA_REG_LBA_HIGH) ||
        !ata_wait_data(device))
        return false;

    uint16_t identify[256];
    for (uint32_t i = 0; i < 256; i++)
        identify[i] = inw(device->io_base + ATA_REG_DATA);
    if (!(identify[49] & (1U << 9)))
        return false;

    uint64_t sectors = (uint32_t)identify[60] |
        ((uint32_t)identify[61] << 16);
    if (!sectors)
        return false;
    if (sectors > ATA_LBA28_LIMIT)
        sectors = ATA_LBA28_LIMIT;
    device->sector_count = sectors;
    device->online = true;
    device->writable = true;
    return true;
}

static bool ata_transfer(
    ata_device_t *device,
    uint64_t lba,
    uint32_t count,
    void *buffer,
    bool write
)
{
    if (!device || !device->online || !buffer || !count ||
        lba >= device->sector_count ||
        count > device->sector_count - lba ||
        lba >= ATA_LBA28_LIMIT ||
        count > ATA_LBA28_LIMIT - lba ||
        (write && !device->writable))
        return false;

    uint32_t channel = device->io_base == 0x1F0 ? 0 : 1;
    uint64_t interrupt_flags;
    __asm__ volatile (
        "pushfq\n\t"
        "popq %0\n\t"
        "cli"
        : "=r"(interrupt_flags)
        :
        : "memory"
    );
    if (channel_busy[channel])
    {
        if (interrupt_flags & (1ULL << 9))
            __asm__ volatile ("sti" ::: "memory");
        return false;
    }
    channel_busy[channel] = true;

    bool success = true;
    uint8_t *bytes = buffer;
    for (uint32_t sector = 0; sector < count && success; sector++)
    {
        uint32_t current_lba = (uint32_t)(lba + sector);
        outb(device->io_base + ATA_REG_DRIVE,
            (uint8_t)(0xE0 |
                (device->slave ? 0x10 : 0) |
                ((current_lba >> 24) & 0x0F)));
        ata_delay(device);
        if (!ata_wait_not_busy(device))
        {
            serial_log_begin(SERIAL_LOG_WARN);
            serial_write("ATA disk remained busy, status ");
            serial_write_hex(inb(device->io_base + ATA_REG_STATUS));
            serial_log_end();
            success = false;
            break;
        }

        outb(device->io_base + ATA_REG_ERROR, 0);
        outb(device->io_base + ATA_REG_SECTOR_COUNT, 1);
        outb(device->io_base + ATA_REG_LBA_LOW, (uint8_t)current_lba);
        outb(
            device->io_base + ATA_REG_LBA_MID,
            (uint8_t)(current_lba >> 8));
        outb(
            device->io_base + ATA_REG_LBA_HIGH,
            (uint8_t)(current_lba >> 16));
        outb(
            device->io_base + ATA_REG_COMMAND,
            write ? ATA_CMD_WRITE_SECTORS : ATA_CMD_READ_SECTORS);

        if (!ata_wait_data(device))
        {
            serial_log_begin(SERIAL_LOG_WARN);
            serial_write("ATA disk did not provide sector data, status ");
            serial_write_hex(inb(device->io_base + ATA_REG_STATUS));
            serial_write(" error ");
            serial_write_hex(inb(device->io_base + ATA_REG_ERROR));
            serial_log_end();
            success = false;
            break;
        }

        uint8_t *sector_buffer = bytes + (size_t)sector * BLOCK_SECTOR_SIZE;
        if (write)
        {
            for (uint32_t word = 0; word < BLOCK_SECTOR_SIZE / 2; word++)
                outw(
                    device->io_base + ATA_REG_DATA,
                    (uint16_t)sector_buffer[word * 2] |
                        ((uint16_t)sector_buffer[word * 2 + 1] << 8));
            if (!ata_wait_not_busy(device))
            {
                serial_log(SERIAL_LOG_WARN, "ATA write did not finish");
                success = false;
            }
        }
        else
        {
            for (uint32_t word = 0; word < BLOCK_SECTOR_SIZE / 2; word++)
            {
                uint16_t value = inw(device->io_base + ATA_REG_DATA);
                sector_buffer[word * 2] = (uint8_t)value;
                sector_buffer[word * 2 + 1] = (uint8_t)(value >> 8);
            }
        }
    }

    if (success && write)
    {
        outb(device->io_base + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
        success = ata_wait_not_busy(device);
    }

    channel_busy[channel] = false;
    if (interrupt_flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
    return success;
}

static bool ata_read(void *context, uint64_t lba, uint32_t count, void *buffer)
{
    return ata_transfer(context, lba, count, buffer, false);
}

static bool ata_write(
    void *context,
    uint64_t lba,
    uint32_t count,
    const void *buffer
)
{
    return ata_transfer(context, lba, count, (void *)buffer, true);
}

uint32_t ata_pio_init(void)
{
    static const uint16_t io_bases[2] = { 0x1F0, 0x170 };
    static const uint16_t control_bases[2] = { 0x3F6, 0x376 };
    uint32_t registered = 0;

    for (uint32_t channel = 0; channel < 2; channel++)
    {
        for (uint32_t unit = 0; unit < 2; unit++)
        {
            uint32_t index = channel * 2 + unit;
            ata_device_t *device = &devices[index];
            device->io_base = io_bases[channel];
            device->control_base = control_bases[channel];
            device->slave = unit != 0;
            if (!ata_probe(device))
                continue;

            block_device_t block = {
                .name = { 'h', 'd', (char)('0' + index), '\0' },
                .sector_count = device->sector_count,
                .sector_size = BLOCK_SECTOR_SIZE,
                .writable = true,
                .context = device,
                .read = ata_read,
                .write = ata_write
            };
            if (block_register(&block))
                registered++;
        }
    }

    if (registered)
        serial_log(SERIAL_LOG_INFO, "Initialized IDE ATA PIO block devices");
    else
        serial_log(SERIAL_LOG_INFO, "No IDE ATA PIO disks detected");
    return registered;
}
