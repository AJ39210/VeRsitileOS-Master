#ifndef USER_SYSCALL_H
#define USER_SYSCALL_H

#include <stdint.h>

#include "syscall.h"

static inline int64_t user_syscall0(uint64_t number)
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

static inline int64_t user_syscall1(
    uint64_t number,
    uint64_t argument1
)
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

static inline int64_t user_syscall3(
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

static inline int64_t user_write(
    int64_t descriptor,
    const void *buffer,
    uint64_t size
)
{
    return user_syscall3(
        SYS_WRITE,
        (uint64_t)descriptor,
        (uint64_t)(uintptr_t)buffer,
        size
    );
}

static inline int64_t user_read(
    int64_t descriptor,
    void *buffer,
    uint64_t size
)
{
    return user_syscall3(
        SYS_READ,
        (uint64_t)descriptor,
        (uint64_t)(uintptr_t)buffer,
        size
    );
}

static inline int64_t user_time_ms(void)
{
    return user_syscall0(SYS_TIME_MS);
}

static inline int64_t user_alloc(uint64_t size)
{
    return user_syscall1(SYS_ALLOC, size);
}

static inline int64_t user_free(void *address)
{
    return user_syscall1(
        SYS_FREE,
        (uint64_t)(uintptr_t)address
    );
}

static inline int64_t user_open(const char *path)
{
    return user_syscall1(
        SYS_OPEN,
        (uint64_t)(uintptr_t)path
    );
}

static inline int64_t user_close(int64_t descriptor)
{
    return user_syscall1(SYS_CLOSE, (uint64_t)descriptor);
}

static inline int64_t user_spawn(void (*entry)(void))
{
    return user_syscall1(
        SYS_SPAWN,
        (uint64_t)(uintptr_t)entry
    );
}

static inline int64_t user_spawn_elf(const char *path)
{
    return user_syscall1(
        SYS_SPAWN_ELF,
        (uint64_t)(uintptr_t)path
    );
}

static inline int64_t user_exit(int64_t status)
{
    return user_syscall1(SYS_EXIT, (uint64_t)status);
}

static inline int64_t user_getpid(void)
{
    return user_syscall0(SYS_GETPID);
}

static inline int64_t user_yield(void)
{
    return user_syscall0(SYS_YIELD);
}

static inline int64_t user_console_clear(void)
{
    return user_syscall0(SYS_CONSOLE_CLEAR);
}

#endif
