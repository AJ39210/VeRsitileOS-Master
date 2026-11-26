#include <stdbool.h>
#include <stdint.h>

#include "irq.h"
#include "keyboard.h"
#include "mouse.h"
#include "syscall.h"
#include "xhci.h"
#include "virtio_input.h"

#define PS2_DATA       0x60
#define PS2_STATUS     0x64
#define PS2_COMMAND    0x64

#define PS2_STATUS_OUTPUT_FULL  0x01
#define PS2_STATUS_INPUT_FULL   0x02
#define PS2_STATUS_AUXILIARY    0x20

#define PS2_CMD_READ_CONFIG     0x20
#define PS2_CMD_WRITE_CONFIG    0x60
#define PS2_CMD_DISABLE_AUX     0xA7
#define PS2_CMD_ENABLE_AUX      0xA8
#define PS2_CMD_WRITE_AUX       0xD4

#define MOUSE_CMD_SET_DEFAULTS  0xF6
#define MOUSE_CMD_ENABLE        0xF4
#define MOUSE_ACK               0xFA

#define MOUSE_EVENT_BUFFER_SIZE 64

static mouse_event_t event_buffer[MOUSE_EVENT_BUFFER_SIZE];
static volatile uint32_t event_head;
static volatile uint32_t event_tail;
static uint8_t packet[3];
static uint8_t packet_index;
static bool initialized;

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

static bool controller_command(uint8_t command)
{
    if (!wait_input_clear())
        return false;
    outb(PS2_COMMAND, command);
    return true;
}

static bool read_controller_config(uint8_t *config)
{
    if (!controller_command(PS2_CMD_READ_CONFIG) ||
        !wait_output_full())
        return false;
    *config = inb(PS2_DATA);
    return true;
}

static bool write_controller_config(uint8_t config)
{
    if (!controller_command(PS2_CMD_WRITE_CONFIG) ||
        !wait_input_clear())
        return false;
    outb(PS2_DATA, config);
    return wait_input_clear();
}

static bool send_mouse_command(uint8_t command)
{
    if (!controller_command(PS2_CMD_WRITE_AUX) ||
        !wait_input_clear())
        return false;
    outb(PS2_DATA, command);

    for (uint32_t i = 0; i < 100000; i++)
    {
        if (!(inb(PS2_STATUS) & PS2_STATUS_OUTPUT_FULL))
            continue;

        uint8_t status = inb(PS2_STATUS);
        uint8_t response = inb(PS2_DATA);
        if (!(status & PS2_STATUS_AUXILIARY))
        {
            keyboard_handle_scancode(response);
            continue;
        }
        return response == MOUSE_ACK;
    }
    return false;
}

static void push_event(mouse_event_t event)
{
    uint32_t head = event_head;
    uint32_t next = (head + 1) % MOUSE_EVENT_BUFFER_SIZE;
    if (next == event_tail)
        return;

    event_buffer[head] = event;
    __atomic_store_n(&event_head, next, __ATOMIC_RELEASE);
}

void mouse_handle_usb_report(int16_t dx, int16_t dy, uint8_t buttons)
{
    mouse_event_t event = {
        .dx = dx,
        .dy = dy,
        .buttons = buttons & 0x07,
        .reserved = {0, 0, 0}
    };
    push_event(event);
}

static void mouse_irq_handler(void)
{
    uint8_t status = inb(PS2_STATUS);
    if (!(status & PS2_STATUS_OUTPUT_FULL))
        return;

    uint8_t value = inb(PS2_DATA);
    if (!(status & PS2_STATUS_AUXILIARY))
    {
        keyboard_handle_scancode(value);
        return;
    }

    if (packet_index == 0 && !(value & 0x08))
        return;

    packet[packet_index++] = value;
    if (packet_index != sizeof(packet))
        return;
    packet_index = 0;

    int16_t dx = (int8_t)packet[1];
    int16_t dy = -(int16_t)(int8_t)packet[2];
    if (packet[0] & 0xC0)
    {
        dx = 0;
        dy = 0;
    }

    mouse_event_t event = {
        .dx = dx,
        .dy = dy,
        .buttons = packet[0] & 0x07,
        .reserved = {0, 0, 0}
    };
    push_event(event);
}

bool mouse_init(void)
{
    initialized = false;
    event_head = 0;
    event_tail = 0;
    packet_index = 0;

    if (!controller_command(PS2_CMD_DISABLE_AUX))
        return false;

    uint8_t config;
    if (!read_controller_config(&config))
        return false;

    config |= 1U << 1;
    config &= ~(1U << 5);
    if (!write_controller_config(config) ||
        !controller_command(PS2_CMD_ENABLE_AUX))
        return false;

    if (!send_mouse_command(MOUSE_CMD_SET_DEFAULTS) ||
        !send_mouse_command(MOUSE_CMD_ENABLE) ||
        !irq_register_handler(12, mouse_irq_handler))
        return false;

    initialized = true;
    return true;
}

bool mouse_is_initialized(void)
{
    return initialized;
}

int64_t mouse_read_events(void *buffer, uint64_t size)
{
    xhci_poll();
    virtio_input_poll();
    if (!buffer && size)
        return -SYSCALL_EINVAL;
    if (size < sizeof(mouse_event_t))
        return -SYSCALL_EINVAL;

    uint64_t capacity = size / sizeof(mouse_event_t);
    uint64_t count = 0;
    mouse_event_t *events = buffer;

    while (count < capacity)
    {
        uint64_t flags;
        __asm__ volatile (
            "pushfq\n"
            "popq %0\n"
            "cli"
            : "=r"(flags)
            :
            : "memory"
        );

        uint32_t tail = event_tail;
        if (tail == event_head)
        {
            if (flags & (1ULL << 9))
                __asm__ volatile ("sti" ::: "memory");
            break;
        }

        events[count++] = event_buffer[tail];
        __atomic_store_n(
            &event_tail,
            (tail + 1) % MOUSE_EVENT_BUFFER_SIZE,
            __ATOMIC_RELEASE
        );

        if (flags & (1ULL << 9))
            __asm__ volatile ("sti" ::: "memory");
    }

    return count ? (int64_t)(count * sizeof(mouse_event_t)) :
        -SYSCALL_EAGAIN;
}
