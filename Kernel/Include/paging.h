
#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>
#include <stdbool.h>

bool paging_init(uint64_t hhdm_offset);

uint64_t paging_get_hhdm_offset(void);

void *paging_phys_to_virt(uint64_t physical_address);

void paging_invlpg(uint64_t virtual_address);

void *ioremap(
    uint64_t physical_address,
    uint64_t size
);

bool paging_nx_supported(void);

#endif
