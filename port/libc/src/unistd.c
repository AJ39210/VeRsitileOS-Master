#include <stdint.h>

#include "errno.h"
#include "fcntl.h"
#include "syscall.h"
#include "sys/types.h"
#include "unistd.h"
#include "internal.h"

ssize_t read(int descriptor, void *buffer, size_t size)
{
    int64_t result = libc_syscall3(
        SYS_READ,
        (uint64_t)descriptor,
        (uint64_t)(uintptr_t)buffer,
        size
    );
    if (result < 0)
    {
        libc_set_errno_from_result(result);
        return -1;
    }
    return (ssize_t)result;
}

ssize_t write(int descriptor, const void *buffer, size_t size)
{
    int64_t result = libc_syscall3(
        SYS_WRITE,
        (uint64_t)descriptor,
        (uint64_t)(uintptr_t)buffer,
        size
    );
    if (result < 0)
    {
        libc_set_errno_from_result(result);
        return -1;
    }
    return (ssize_t)result;
}

int open(const char *path, int flags, ...)
{
    const int allowed_flags =
        O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND;
    if (!path || (flags & ~(allowed_flags)) ||
        ((flags & O_TRUNC) && (flags & 3) == O_RDONLY))
    {
        errno = EINVAL;
        return -1;
    }

    int64_t result = libc_syscall3(
        SYS_OPEN,
        (uint64_t)(uintptr_t)path,
        (uint64_t)flags,
        0
    );
    if (result < 0)
    {
        libc_set_errno_from_result(result);
        return -1;
    }
    return (int)result;
}

int close(int descriptor)
{
    int64_t result = libc_syscall1(SYS_CLOSE, (uint64_t)descriptor);
    if (result < 0)
    {
        libc_set_errno_from_result(result);
        return -1;
    }
    return (int)result;
}

int unlink(const char *path)
{
    if (!path)
    {
        errno = EINVAL;
        return -1;
    }
    int64_t result = libc_syscall1(
        SYS_UNLINK,
        (uint64_t)(uintptr_t)path
    );
    if (result < 0)
    {
        libc_set_errno_from_result(result);
        return -1;
    }
    return (int)result;
}

off_t lseek(int descriptor, off_t offset, int whence)
{
    int64_t result = libc_syscall3(
        SYS_SEEK,
        (uint64_t)descriptor,
        (uint64_t)offset,
        (uint64_t)whence
    );
    if (result < 0)
    {
        libc_set_errno_from_result(result);
        return (off_t)-1;
    }
    return (off_t)result;
}

int getpid(void)
{
    int64_t result = libc_syscall0(SYS_GETPID);
    if (result < 0)
    {
        libc_set_errno_from_result(result);
        return -1;
    }
    return (int)result;
}

int isatty(int descriptor)
{
    if (descriptor >= STDIN_FILENO && descriptor <= STDERR_FILENO)
        return 1;
    errno = EBADF;
    return 0;
}

void yield(void)
{
    (void)libc_syscall0(SYS_YIELD);
}

_Noreturn void _exit(int status)
{
    (void)libc_syscall1(SYS_EXIT, (uint64_t)status);
    for (;;)
        __asm__ volatile ("pause");
}
