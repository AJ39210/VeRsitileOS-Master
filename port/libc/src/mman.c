#include <stdint.h>

#include "errno.h"
#include "internal.h"
#include "sys/mman.h"
#include "syscall.h"

void *mmap(
    void *address,
    size_t length,
    int protection,
    int flags,
    int descriptor,
    off_t offset
)
{
    int64_t result = libc_syscall6(
        SYS_MMAP,
        (uint64_t)(uintptr_t)address,
        length,
        (uint64_t)protection,
        (uint64_t)flags,
        (uint64_t)(int64_t)descriptor,
        (uint64_t)offset);
    if (result < 0)
    {
        libc_set_errno_from_result(result);
        return MAP_FAILED;
    }
    return (void *)(uintptr_t)result;
}

int munmap(void *address, size_t length)
{
    int64_t result = libc_syscall3(
        SYS_MUNMAP,
        (uint64_t)(uintptr_t)address,
        length,
        0);
    if (result < 0)
    {
        libc_set_errno_from_result(result);
        return -1;
    }
    return 0;
}

int mprotect(void *address, size_t length, int protection)
{
    int64_t result = libc_syscall3(
        SYS_MPROTECT,
        (uint64_t)(uintptr_t)address,
        length,
        (uint64_t)protection);
    if (result < 0)
    {
        libc_set_errno_from_result(result);
        return -1;
    }
    return 0;
}
