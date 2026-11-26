#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stdbool.h>

struct limine_memmap_response;

bool pmm_init(
    struct limine_memmap_response *memmap,
    uint64_t hhdm_offset
);

uint64_t pmm_alloc_page(void);
uint64_t pmm_alloc_contiguous_pages(uint64_t page_count);
bool pmm_free_page(uint64_t physical_address);

bool pmm_is_allocated_page(uint64_t physical_address);

uint64_t pmm_get_total_memory(void);
uint64_t pmm_get_free_memory(void);
uint64_t pmm_get_used_memory(void);

uint64_t pmm_get_total_pages(void);
uint64_t pmm_get_free_pages(void);

#endif