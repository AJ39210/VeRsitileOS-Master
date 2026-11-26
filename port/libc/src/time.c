#include "internal.h"
#include "time.h"
#include "syscall.h"

int clock_gettime(int clock_id, struct timespec *time)
{
    if (!time || (clock_id != CLOCK_MONOTONIC && clock_id != 0))
        return -1;

    int64_t milliseconds = libc_syscall0(SYS_TIME_MS);
    if (milliseconds < 0)
    {
        libc_set_errno_from_result(milliseconds);
        return -1;
    }

    time->tv_sec = milliseconds / 1000;
    time->tv_nsec = (milliseconds % 1000) * 1000000;
    return 0;
}

clock_t clock(void)
{
    int64_t milliseconds = libc_syscall0(SYS_TIME_MS);
    if (milliseconds < 0)
    {
        libc_set_errno_from_result(milliseconds);
        return (clock_t)-1;
    }
    return (clock_t)milliseconds;
}
