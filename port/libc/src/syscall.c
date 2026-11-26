#include <stdint.h>

#include "internal.h"

int errno;

int64_t libc_syscall0(uint64_t number)
{
    register uint64_t result __asm__("rax") = number;
    __asm__ volatile (
        "int $0x80"
        : "+a"(result)
        :
        : "memory", "cc"
    );
    return (int64_t)result;
}

int64_t libc_syscall1(uint64_t number, uint64_t argument1)
{
    register uint64_t result __asm__("rax") = number;
    register uint64_t first __asm__("rdi") = argument1;
    __asm__ volatile (
        "int $0x80"
        : "+a"(result), "+D"(first)
        :
        : "memory", "cc"
    );
    return (int64_t)result;
}

int64_t libc_syscall3(
    uint64_t number,
    uint64_t argument1,
    uint64_t argument2,
    uint64_t argument3
)
{
    register uint64_t result __asm__("rax") = number;
    register uint64_t first __asm__("rdi") = argument1;
    register uint64_t second __asm__("rsi") = argument2;
    register uint64_t third __asm__("rdx") = argument3;
    __asm__ volatile (
        "int $0x80"
        : "+a"(result),
          "+D"(first),
          "+S"(second),
          "+d"(third)
        :
        : "memory", "cc"
    );
    return (int64_t)result;
}

int64_t libc_syscall6(
    uint64_t number,
    uint64_t argument1,
    uint64_t argument2,
    uint64_t argument3,
    uint64_t argument4,
    uint64_t argument5,
    uint64_t argument6
)
{
    register uint64_t result __asm__("rax") = number;
    register uint64_t first __asm__("rdi") = argument1;
    register uint64_t second __asm__("rsi") = argument2;
    register uint64_t third __asm__("rdx") = argument3;
    register uint64_t fourth __asm__("r10") = argument4;
    register uint64_t fifth __asm__("r8") = argument5;
    register uint64_t sixth __asm__("r9") = argument6;
    __asm__ volatile (
        "int $0x80"
        : "+a"(result)
        : "D"(first),
          "S"(second),
          "d"(third),
          "r"(fourth),
          "r"(fifth),
          "r"(sixth)
        : "memory", "cc"
    );
    return (int64_t)result;
}

void libc_set_errno_from_result(int64_t result)
{
    errno = (int)-result;
}
