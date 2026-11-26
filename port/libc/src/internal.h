#ifndef VERSITILE_LIBC_INTERNAL_H
#define VERSITILE_LIBC_INTERNAL_H

#include <stdint.h>

int64_t libc_syscall0(uint64_t number);
int64_t libc_syscall1(uint64_t number, uint64_t argument1);
int64_t libc_syscall3(
    uint64_t number,
    uint64_t argument1,
    uint64_t argument2,
    uint64_t argument3
);
int64_t libc_syscall6(
    uint64_t number,
    uint64_t argument1,
    uint64_t argument2,
    uint64_t argument3,
    uint64_t argument4,
    uint64_t argument5,
    uint64_t argument6
);
void libc_set_errno_from_result(int64_t result);

#endif
