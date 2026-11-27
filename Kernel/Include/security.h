#ifndef SECURITY_H
#define SECURITY_H

#include <stdbool.h>
#include <stdint.h>

bool security_init(void);
bool security_is_write_protection_fault(uint64_t error_code);
bool security_verify_signed_module(
    const uint8_t *file_data,
    uint64_t file_size,
    uint64_t *payload_size
);

void security_panic_write_protection_fault(
    uint64_t fault_address,
    uint64_t error_code,
    uint64_t instruction_pointer
) __attribute__((noreturn));

#endif
