
#include <stdint.h>
#include <stdbool.h>

#include "atapi.h"


#define ATA_REG_DATA       0
#define ATA_REG_ERROR      1
#define ATA_REG_FEATURES   1
#define ATA_REG_SECCOUNT   2
#define ATA_REG_LBA_LOW    3
#define ATA_REG_LBA_MID    4
#define ATA_REG_LBA_HIGH   5
#define ATA_REG_DRIVE      6
#define ATA_REG_STATUS     7
#define ATA_REG_COMMAND    7


#define ATA_CMD_IDENTIFY_PACKET 0xA1
#define ATA_CMD_PACKET          0xA0

/* ATA status flags */

#define ATA_STATUS_ERR   0x01
#define ATA_STATUS_DRQ   0x08
#define ATA_STATUS_DF    0x20
#define ATA_STATUS_DRDY  0x40
#define ATA_STATUS_BSY   0x80

/* ATAPI commands */

#define ATAPI_CMD_READ10 0x28

#define ATAPI_TIMEOUT 1000000

/* CPU port I/O */

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;

    __asm__ volatile (
        "inb %1, %0"
        : "=a"(value)
        : "Nd"(port)
    );

    return value;
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t value;

    __asm__ volatile (
        "inw %1, %0"
        : "=a"(value)
        : "Nd"(port)
    );

    return value;
}

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile (
        "outb %0, %1"
        :
        : "a"(value), "Nd"(port)
    );
}

static inline void outw(uint16_t port, uint16_t value)
{
    __asm__ volatile (
        "outw %0, %1"
        :
        : "a"(value), "Nd"(port)
    );
}



static void ata_delay(atapi_device_t *device)
{
    inb(device->control_base);
    inb(device->control_base);
    inb(device->control_base);
    inb(device->control_base);
}



static bool ata_wait_not_busy(atapi_device_t *device)
{
    for (uint32_t i = 0; i < ATAPI_TIMEOUT; i++)
    {
        uint8_t status = inb(
            device->io_base + ATA_REG_STATUS
        );

        if (status == 0xFF || status == 0x00)
            return false;

        if (!(status & ATA_STATUS_BSY))
            return true;
    }

    return false;
}



static bool ata_wait_drq(atapi_device_t *device)
{
    for (uint32_t i = 0; i < ATAPI_TIMEOUT; i++)
    {
        uint8_t status = inb(
            device->io_base + ATA_REG_STATUS
        );

        if (status == 0xFF || status == 0x00)
            return false;

        if (status & (ATA_STATUS_ERR | ATA_STATUS_DF))
            return false;

        if (!(status & ATA_STATUS_BSY) &&
            (status & ATA_STATUS_DRQ))
            return true;
    }

    return false;
}

/* Check the final command status. */

static bool ata_command_finished(atapi_device_t *device)
{
    if (!ata_wait_not_busy(device))
        return false;

    uint8_t status = inb(
        device->io_base + ATA_REG_STATUS
    );

    return !(status & (ATA_STATUS_ERR | ATA_STATUS_DF));
}



static void ata_select_device(atapi_device_t *device)
{
    uint8_t drive = device->slave ? 0xB0 : 0xA0;

    outb(
        device->io_base + ATA_REG_DRIVE,
        drive
    );

    ata_delay(device);
}

/* Initialize the ATAPI device. */

bool atapi_init(
    atapi_device_t *device,
    uint16_t io_base,
    uint16_t control_base,
    bool slave
)
{
    if (!device)
        return false;

    device->io_base = io_base;
    device->control_base = control_base;
    device->slave = slave;
    device->initialized = false;

    ata_select_device(device);

    if (!ata_wait_not_busy(device))
        return false;

  

    outb(io_base + ATA_REG_FEATURES, 0);
    outb(io_base + ATA_REG_SECCOUNT, 0);
    outb(io_base + ATA_REG_LBA_LOW, 0);
    outb(io_base + ATA_REG_LBA_MID, 0);
    outb(io_base + ATA_REG_LBA_HIGH, 0);

    /* Send IDENTIFY PACKET DEVICE. */

    outb(io_base + ATA_REG_COMMAND,
         ATA_CMD_IDENTIFY_PACKET);

    uint8_t status = inb(io_base + ATA_REG_STATUS);

    if (status == 0x00 || status == 0xFF)
        return false;

    if (!ata_wait_not_busy(device))
        return false;

    if (!ata_wait_drq(device))
        return false;

    

    for (uint32_t i = 0; i < 256; i++)
        (void)inw(io_base + ATA_REG_DATA);

    if (!ata_command_finished(device))
        return false;

    device->initialized = true;

    return true;
}

/* Send an ATAPI packet command. */

static bool atapi_send_packet(
    atapi_device_t *device,
    const uint8_t *packet
)
{
    if (!ata_wait_not_busy(device))
        return false;

    /*
     * Request a maximum transfer phase of 2048 bytes.
     * Cylinder Low and High specify the byte count.
     */

    outb(device->io_base + ATA_REG_FEATURES, 0);

    outb(device->io_base + ATA_REG_LBA_MID, 0x00);
    outb(device->io_base + ATA_REG_LBA_HIGH, 0x08);

    outb(device->io_base + ATA_REG_COMMAND,
         ATA_CMD_PACKET);

    if (!ata_wait_drq(device))
        return false;



    for (uint32_t i = 0; i < 12; i += 2)
    {
        uint16_t word =
            packet[i] |
            ((uint16_t)packet[i + 1] << 8);

        outw(device->io_base + ATA_REG_DATA, word);
    }

    return true;
}

/* Read one 2048-byte sector. */

bool atapi_read_sector(
    void *context,
    uint32_t lba,
    uint8_t *buffer
)
{
    atapi_device_t *device = context;

    if (!device || !device->initialized || !buffer)
        return false;



    uint8_t packet[12] = {0};

    packet[0] = ATAPI_CMD_READ10;

    packet[2] = (lba >> 24) & 0xFF;
    packet[3] = (lba >> 16) & 0xFF;
    packet[4] = (lba >> 8) & 0xFF;
    packet[5] = lba & 0xFF;

    /* Read one sector. */

    packet[7] = 0;
    packet[8] = 1;

    ata_select_device(device);

    if (!atapi_send_packet(device, packet))
        return false;

    uint32_t total = 0;

    while (total < ATAPI_SECTOR_SIZE)
    {
        if (!ata_wait_not_busy(device))
            return false;

        uint8_t status = inb(
            device->io_base + ATA_REG_STATUS
        );

        if (status & (ATA_STATUS_ERR | ATA_STATUS_DF))
            return false;

        if (!(status & ATA_STATUS_DRQ))
            return false;

        uint16_t byte_count =
            inb(device->io_base + ATA_REG_LBA_MID) |
            ((uint16_t)inb(
                device->io_base + ATA_REG_LBA_HIGH
            ) << 8);

        if (byte_count == 0)
            return false;

        for (uint32_t i = 0; i < byte_count; i += 2)
        {
            uint16_t word =
                inw(device->io_base + ATA_REG_DATA);

            if (total < ATAPI_SECTOR_SIZE)
                buffer[total++] = word & 0xFF;
            else
                return false;

            if (i + 1 < byte_count)
            {
                if (total < ATAPI_SECTOR_SIZE)
                    buffer[total++] = word >> 8;
                else
                    return false;
            }
        }
    }

    return ata_command_finished(device);
}
