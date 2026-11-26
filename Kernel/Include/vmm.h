
#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stdbool.h>

#define VMM_PAGE_SIZE       4096ULL

#define VMM_PRESENT         (1ULL << 0)
#define VMM_WRITABLE        (1ULL << 1)
#define VMM_USER            (1ULL << 2)
#define VMM_WRITE_THROUGH   (1ULL << 3)
#define VMM_CACHE_DISABLE   (1ULL << 4)
#define VMM_GLOBAL          (1ULL << 8)
#define VMM_NO_EXECUTE      (1ULL << 63)

typedef uint64_t vmm_address_space_t;

bool vmm_init(void);

vmm_address_space_t vmm_create_address_space(void);
vmm_address_space_t vmm_clone_address_space(vmm_address_space_t source);
bool vmm_destroy_address_space(vmm_address_space_t address_space);
bool vmm_switch_address_space(vmm_address_space_t address_space);
vmm_address_space_t vmm_current_address_space(void);

bool vmm_map_page(
    uint64_t virtual_address,
    uint64_t physical_address,
    uint64_t flags
);

bool vmm_map_page_in(
    vmm_address_space_t address_space,
    uint64_t virtual_address,
    uint64_t physical_address,
    uint64_t flags
);

bool vmm_unmap_page(uint64_t virtual_address);
bool vmm_unmap_page_in(
    vmm_address_space_t address_space,
    uint64_t virtual_address
);
bool vmm_protect_page_in(
    vmm_address_space_t address_space,
    uint64_t virtual_address,
    uint64_t flags
);

uint64_t vmm_translate(uint64_t virtual_address);
uint64_t vmm_translate_in(
    vmm_address_space_t address_space,
    uint64_t virtual_address
);
bool vmm_user_range_valid(
    vmm_address_space_t address_space,
    uint64_t address,
    uint64_t size,
    bool writable
);
bool vmm_user_address_executable(
    vmm_address_space_t address_space,
    uint64_t address
);

#endif
