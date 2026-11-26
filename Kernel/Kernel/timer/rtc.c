#include <stdbool.h>
#include <stdint.h>

#include "rtc.h"

#define CMOS_INDEX_PORT 0x70
#define CMOS_DATA_PORT 0x71
#define CMOS_STATUS_A 0x0A
#define CMOS_STATUS_B 0x0B
#define CMOS_SECONDS 0x00
#define CMOS_MINUTES 0x02
#define CMOS_HOURS 0x04
#define CMOS_DAY 0x07
#define CMOS_MONTH 0x08
#define CMOS_YEAR 0x09
#define CMOS_CENTURY 0x32
#define CMOS_UPDATE_IN_PROGRESS 0x80
#define CMOS_24_HOUR 0x02
#define CMOS_BINARY_MODE 0x04
#define CMOS_PM 0x80

typedef struct
{
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint8_t year;
    uint8_t century;
    uint8_t status_b;
} rtc_snapshot_t;

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static uint8_t read_cmos(uint8_t index)
{
    uint8_t previous_index = inb(CMOS_INDEX_PORT);
    outb(CMOS_INDEX_PORT, index);
    uint8_t value = inb(CMOS_DATA_PORT);
    outb(CMOS_INDEX_PORT, previous_index);
    return value;
}

static bool wait_for_update(void)
{
    for (uint32_t attempt = 0; attempt < 100000; attempt++)
        if (!(read_cmos(CMOS_STATUS_A) & CMOS_UPDATE_IN_PROGRESS))
            return true;
    return false;
}

static bool read_snapshot(rtc_snapshot_t *snapshot)
{
    if (!wait_for_update())
        return false;
    snapshot->second = read_cmos(CMOS_SECONDS);
    snapshot->minute = read_cmos(CMOS_MINUTES);
    snapshot->hour = read_cmos(CMOS_HOURS);
    snapshot->day = read_cmos(CMOS_DAY);
    snapshot->month = read_cmos(CMOS_MONTH);
    snapshot->year = read_cmos(CMOS_YEAR);
    snapshot->century = read_cmos(CMOS_CENTURY);
    snapshot->status_b = read_cmos(CMOS_STATUS_B);
    return wait_for_update() &&
        !(read_cmos(CMOS_STATUS_A) & CMOS_UPDATE_IN_PROGRESS);
}

static bool snapshots_equal(
    const rtc_snapshot_t *left,
    const rtc_snapshot_t *right)
{
    return left->second == right->second &&
        left->minute == right->minute &&
        left->hour == right->hour &&
        left->day == right->day &&
        left->month == right->month &&
        left->year == right->year &&
        left->century == right->century &&
        left->status_b == right->status_b;
}

static uint8_t from_bcd(uint8_t value)
{
    return (uint8_t)((value & 0x0F) + ((value >> 4) * 10));
}

static bool leap_year(uint16_t year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

bool rtc_read_time(os_rtc_time_t *time)
{
    if (!time)
        return false;

    rtc_snapshot_t first;
    rtc_snapshot_t second;
    bool stable = false;
    for (uint32_t attempt = 0; attempt < 4; attempt++)
    {
        if (!read_snapshot(&first) || !read_snapshot(&second))
            return false;
        if (snapshots_equal(&first, &second))
        {
            stable = true;
            break;
        }
    }
    if (!stable)
        return false;

    uint8_t raw_hour = second.hour;
    bool is_24_hour = (second.status_b & CMOS_24_HOUR) != 0;
    bool is_binary = (second.status_b & CMOS_BINARY_MODE) != 0;
    bool is_pm = !is_24_hour && (raw_hour & CMOS_PM) != 0;
    if (!is_24_hour)
        raw_hour &= (uint8_t)~CMOS_PM;

    uint8_t values[] = {
        second.second, second.minute, raw_hour,
        second.day, second.month, second.year
    };
    if (!is_binary)
        for (uint32_t i = 0; i < sizeof(values); i++)
            values[i] = from_bcd(values[i]);

    uint16_t year;
    if (second.century != 0 && second.century != 0xFF)
    {
        uint8_t century = is_binary
            ? second.century : from_bcd(second.century);
        if (century < 19 || century > 99)
            return false;
        year = (uint16_t)century * 100 + values[5];
    }
    else
    {
        year = values[5] < 70
            ? (uint16_t)(2000 + values[5])
            : (uint16_t)(1900 + values[5]);
    }

    uint8_t hour = values[2];
    if (!is_24_hour)
    {
        if (hour < 1 || hour > 12)
            return false;
        hour = (uint8_t)(hour % 12 + (is_pm ? 12 : 0));
    }
    if (values[0] > 59 || values[1] > 59 || hour > 23 ||
        values[4] < 1 || values[4] > 12 ||
        values[3] < 1)
        return false;

    static const uint8_t days_per_month[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    uint8_t maximum_day = days_per_month[values[4] - 1];
    if (values[4] == 2 && leap_year(year))
        maximum_day++;
    if (values[3] > maximum_day)
        return false;

    *time = (os_rtc_time_t){
        .year = year,
        .month = values[4],
        .day = values[3],
        .hour = hour,
        .minute = values[1],
        .second = values[0],
        .reserved = 0
    };
    return true;
}
