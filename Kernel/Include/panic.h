#ifndef PANIC_H
#define PANIC_H

#include <stdint.h>

void kernel_panic(const char *message) __attribute__((noreturn));
void kernel_panic_exception(
    uint64_t vector,
    uint64_t error_code,
    uint64_t instruction_pointer
) __attribute__((noreturn));

#endif
