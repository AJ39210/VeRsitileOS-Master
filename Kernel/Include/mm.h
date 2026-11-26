
#ifndef MM_H
#define MM_H

#include <stdint.h>
#include <stdbool.h>

struct limine_memmap_response;

bool mm_init(
    struct limine_memmap_response *memmap,
    uint64_t hhdm_offset
);

#endif
