#ifndef ATAPI_H
#define ATAPI_H

#include <stdint.h>
#include <stdbool.h>

#define ATAPI_SECTOR_SIZE 2048

typedef struct
{
    uint16_t io_base;
    uint16_t control_base;

    bool slave;
    bool initialized;

} atapi_device_t;

/* Initialize an ATAPI CD-ROM device. */
bool atapi_init(
    atapi_device_t *device,
    uint16_t io_base,
    uint16_t control_base,
    bool slave
);

/* Read one 2048-byte CD-ROM sector. */
bool atapi_read_sector(
    void *context,
    uint32_t lba,
    uint8_t *buffer
);

#endif