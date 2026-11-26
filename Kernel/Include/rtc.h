#ifndef RTC_H
#define RTC_H

#include <stdbool.h>

#include <os/gui.h>

bool rtc_read_time(os_rtc_time_t *time);

#endif
