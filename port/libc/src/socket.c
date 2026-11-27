#include <stdint.h>

#include "errno.h"
#include "internal.h"
#include "poll.h"
#include "sys/socket.h"
#include "syscall.h"
#include "unistd.h"

int socketpair(int domain, int type, int protocol, int descriptors[2])
{
    if (!descriptors)
    {
        errno = EFAULT;
        return -1;
    }

    int64_t result = libc_syscall6(
        SYS_SOCKETPAIR,
        (uint64_t)domain,
        (uint64_t)type,
        (uint64_t)protocol,
        (uint64_t)(uintptr_t)descriptors,
        0,
        0);
    if (result < 0)
    {
        libc_set_errno_from_result(result);
        return -1;
    }
    return 0;
}

int poll(struct pollfd *fds, nfds_t count, int timeout)
{
    if ((count && !fds) || timeout < -1)
    {
        errno = EINVAL;
        return -1;
    }

    uint64_t start = (uint64_t)libc_syscall0(SYS_TIME_MS);
    for (;;)
    {
        int64_t result = libc_syscall3(
            SYS_POLL,
            (uint64_t)(uintptr_t)fds,
            count,
            0);
        if (result < 0)
        {
            libc_set_errno_from_result(result);
            return -1;
        }
        if (result > 0 || timeout == 0)
            return (int)result;

        if (timeout > 0)
        {
            uint64_t now = (uint64_t)libc_syscall0(SYS_TIME_MS);
            if (now - start >= (uint64_t)timeout)
                return 0;
        }
        yield();
    }
}
