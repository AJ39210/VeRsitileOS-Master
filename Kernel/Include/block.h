#ifndef BLOCK_H
#define BLOCK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BLOCK_SECTOR_SIZE 512
#define BLOCK_DEVICE_NAME_MAX 16
#define BLOCK_DEVICE_LIMIT 8

typedef bool (*block_read_fn)(
    void *context,
    uint64_t lba,
    uint32_t count,
    void *buffer
);
typedef bool (*block_write_fn)(
    void *context,
    uint64_t lba,
    uint32_t count,
    const void *buffer
);

typedef struct
{
    char name[BLOCK_DEVICE_NAME_MAX];
    uint64_t sector_count;
    uint32_t sector_size;
    bool writable;
    void *context;
    block_read_fn read;
    block_write_fn write;
} block_device_t;

bool block_register(const block_device_t *device);
uint32_t block_device_count(void);
const block_device_t *block_device_get(uint32_t index);
bool block_read(
    uint32_t device_index,
    uint64_t lba,
    uint32_t count,
    void *buffer
);
bool block_write(
    uint32_t device_index,
    uint64_t lba,
    uint32_t count,
    const void *buffer
);

#endif
