#include <stdint.h>
#include <stdbool.h>

#include "timer.h"
#include "irq.h"
#include "panic.h"

#define PIT_CHANNEL_0  0x40
#define PIT_COMMAND    0x43

#define PIT_INPUT_FREQUENCY 1193182

static volatile uint64_t timer_ticks = 0;
static volatile uint64_t watchdog_timeout_ms = 0;
static volatile uint64_t watchdog_last_feed_ms = 0;

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile (
        "outb %0, %1"
        :
        : "a"(value), "Nd"(port)
    );
}

static void pit_irq_handler(void)
{
    timer_ticks++;
    uint64_t timeout = watchdog_timeout_ms;
    if (timeout &&
        timer_ticks * 10 - watchdog_last_feed_ms > timeout)
        kernel_panic("systemd software watchdog expired");
}

bool timer_init(void)
{
    /*
     * Calculate the PIT divisor.
     * 1193182 / 100 = approximately 100 interrupts per second.
     */
    uint16_t divisor = PIT_INPUT_FREQUENCY / PIT_FREQUENCY;

    /*
     * Configure PIT:
     * Channel 0
     * Low byte / high byte
     * Mode 3 (square wave generator)
     * Binary counting
     */
    outb(PIT_COMMAND, 0x36);

    /* Send divisor, low byte first. */
    outb(PIT_CHANNEL_0, (uint8_t)(divisor & 0xFF));

    /* Then high byte. */
    outb(PIT_CHANNEL_0, (uint8_t)(divisor >> 8));

    /*
     * Register IRQ0 only after programming the PIT.
     * This also unmasks IRQ0 through your APIC code.
     */
    if (!irq_register_handler(0, pit_irq_handler))
        return false;

    return true;
}

uint64_t timer_get_ticks(void)
{
    return timer_ticks;
}

uint64_t timer_get_milliseconds(void)
{
    return timer_ticks * 10;
}

void timer_systemd_watchdog_feed(uint32_t timeout_seconds)
{
    watchdog_last_feed_ms = timer_get_milliseconds();
    watchdog_timeout_ms = (uint64_t)timeout_seconds * 1000;
}