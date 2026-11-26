#ifndef ELF_LOADER_H
#define ELF_LOADER_H

#include <stdint.h>

#include "vmm.h"

int64_t elf_load_executable(
    vmm_address_space_t address_space,
    const char *path,
    uint64_t *entry_point
);

#endif
