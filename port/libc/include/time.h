#ifndef VERSITILE_LIBC_TIME_H
#define VERSITILE_LIBC_TIME_H

#include <stdint.h>

typedef long time_t;
typedef long clock_t;

struct tm
{
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

#define CLOCKS_PER_SEC 1000
#define CLOCK_MONOTONIC 1

struct timespec
{
    time_t tv_sec;
    long tv_nsec;
};

int clock_gettime(int clock_id, struct timespec *time);
clock_t clock(void);

#endif
