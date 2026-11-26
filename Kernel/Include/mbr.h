#ifndef MBR_H
#define MBR_H

#include <stdbool.h>
#include <stdint.h>

#define MBR_PARTITION_LIMIT 32
#define MBR_GPT_PARTITION_NAME_MAX 36

typedef struct
{
    uint32_t device_index;
    uint32_t number;
    uint8_t type;
    bool bootable;
    bool gpt;
    uint8_t type_guid[16];
    uint64_t start_lba;
    uint64_t sector_count;
} mbr_partition_t;

typedef struct
{
    uint8_t type_guid[16];
    uint64_t start_lba;
    uint64_t sector_count;
    uint64_t attributes;
    char name[MBR_GPT_PARTITION_NAME_MAX];
} mbr_gpt_partition_t;

uint32_t mbr_scan(void);
uint32_t mbr_partition_count(void);
const mbr_partition_t *mbr_partition_get(uint32_t index);
bool mbr_create_gpt(
    uint32_t device_index,
    const mbr_gpt_partition_t *requested,
    uint32_t partition_count
);
bool mbr_delete_partition(uint32_t device_index, uint32_t partition_number);

#endif
