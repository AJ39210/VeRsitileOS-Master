#ifndef REGION_H
#define REGION_H

#include <stdint.h>
#include <stdbool.h>

bool region_init(void);

uint64_t region_alloc(
    uint64_t size,
    uint64_t alignment
);

bool region_free(
    uint64_t address,
    uint64_t size
);

bool region_is_reserved(uint64_t address);

#endif