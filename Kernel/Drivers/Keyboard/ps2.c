#include <stdint.h>
#include <stdbool.h>

#include "ps2.h"
#include "keyboard.h"
#include "irq.h"

#define PS2_DATA       0x60
#define PS2_STATUS     0x64
#define PS2_COMMAND    0x64

#define PS2_STATUS_OUTPUT_FULL  0x01
#define PS2_STATUS_INPUT_FULL   0x02

#define PS2_CMD_READ_CONFIG     0x20
#define PS2_CMD_WRITE_CONFIG    0x60
#define PS2_CMD_DISABLE_PORT1   0xAD
#define PS2_CMD_ENABLE_PORT1    0xAE

#define KEYBOARD_CMD_ENABLE     0xF4
#define KEYBOARD_ACK            0xFA

static bool controller_ready;

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile (
        "outb %0, %1"
        :
        : "a"(value), "Nd"(port)
    );
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;

    __asm__ volatile (
        "inb %1, %0"
        : "=a"(value)
        : "Nd"(port)
    );

    return value;
}

static bool wait_input_clear(void)
{
    for (uint32_t i = 0; i < 100000; i++)
    {
        if (!(inb(PS2_STATUS) & PS2_STATUS_INPUT_FULL))
            return true;
    }

    return false;
}

static bool wait_output_full(void)
{
    for (uint32_t i = 0; i < 100000; i++)
    {
        if (inb(PS2_STATUS) & PS2_STATUS_OUTPUT_FULL)
            return true;
    }

    return false;
}

static void flush_output(void)
{
    for (uint32_t i = 0; i < 1000; i++)
    {
        if (!(inb(PS2_STATUS) & PS2_STATUS_OUTPUT_FULL))
            break;

        (void)inb(PS2_DATA);
    }
}

static void keyboard_irq_handler(void)
{
    uint8_t status = inb(PS2_STATUS);

    if (!(status & PS2_STATUS_OUTPUT_FULL))
        return;

    /* Ignore auxiliary-device data. */
    if (status & 0x20)
        return;

    uint8_t scancode = inb(PS2_DATA);

    keyboard_handle_scancode(scancode);
}

bool ps2_init(void)
{
    controller_ready = false;
    keyboard_init();

    /* Disable the first PS/2 port during setup. */
    if (!wait_input_clear())
        return false;

    outb(PS2_COMMAND, PS2_CMD_DISABLE_PORT1);

    if (!wait_input_clear())
        return false;

    flush_output();

    /* Read controller configuration. */
    outb(PS2_COMMAND, PS2_CMD_READ_CONFIG);

    if (!wait_output_full())
        return false;

    uint8_t config = inb(PS2_DATA);

    /* Enable IRQ1 and enable the first port's clock. */
    config |=  (1 << 0);
    config &= ~(1 << 4);

    /* Keep IRQ12 disabled. */
    config &= ~(1 << 1);

    if (!wait_input_clear())
        return false;

    outb(PS2_COMMAND, PS2_CMD_WRITE_CONFIG);

    if (!wait_input_clear())
        return false;

    outb(PS2_DATA, config);

    if (!wait_input_clear())
        return false;

    /* Enable the first PS/2 port. */
    outb(PS2_COMMAND, PS2_CMD_ENABLE_PORT1);

    if (!wait_input_clear())
        return false;

    /* Tell the keyboard to start scanning. */
    outb(PS2_DATA, KEYBOARD_CMD_ENABLE);

    if (!wait_output_full())
        return false;

    uint8_t response = inb(PS2_DATA);

    if (response != KEYBOARD_ACK)
        return false;

    controller_ready = true;

    /* Register the handler and unmask IRQ1. */
    (void)irq_register_handler(1, keyboard_irq_handler);

    return true;
}

void ps2_poll(void)
{
    if (!controller_ready)
        return;

    for (uint32_t i = 0; i < 32; i++)
    {
        uint8_t status = inb(PS2_STATUS);
        if (!(status & PS2_STATUS_OUTPUT_FULL))
            break;

        uint8_t data = inb(PS2_DATA);
        if (!(status & 0x20))
            keyboard_handle_scancode(data);
    }
}