#include <stdbool.h>
#include <stdint.h>

#include "security.h"

#define CR0_WRITE_PROTECT (1ULL << 16)

bool security_init(void)
{
    uint64_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= CR0_WRITE_PROTECT;
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0) : "memory");
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    return (cr0 & CR0_WRITE_PROTECT) != 0;
}

bool security_is_write_protection_fault(uint64_t error_code)
{
    return (error_code & 0x7) == 0x3;
}
