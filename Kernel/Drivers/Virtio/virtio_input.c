#include <stdbool.h>
#include <stdint.h>

#include "keyboard.h"
#include "mouse.h"
#include "pci.h"
#include "serial.h"
#include "virtio_input.h"
#include "virtio_legacy.h"
#include "virtio_pci.h"

#define VIRTIO_INPUT_DEVICE_LIMIT 4
#define VIRTIO_INPUT_EVENT_BUFFER_COUNT 16
#define VIRTIO_INPUT_STATUS_ACKNOWLEDGE 0x01
#define VIRTIO_INPUT_STATUS_DRIVER 0x02
#define VIRTIO_INPUT_STATUS_DRIVER_OK 0x04
#define VIRTIO_EV_SYN 0
#define VIRTIO_EV_KEY 1
#define VIRTIO_EV_REL 2
#define VIRTIO_SYN_REPORT 0
#define VIRTIO_REL_X 0
#define VIRTIO_REL_Y 1
#define VIRTIO_KEY_LEFT 0x110
#define VIRTIO_KEY_RIGHT 0x111
#define VIRTIO_KEY_MIDDLE 0x112

typedef struct __attribute__((packed))
{
    uint16_t type;
    uint16_t code;
    int32_t value;
} virtio_input_event_t;

typedef struct
{
    uint16_t io_base;
    uint16_t queue_index;
    bool modern;
    const virtio_pci_device_t *transport;
    bool used;
    bool keyboard;
    bool mouse;
    virtio_legacy_queue_t event_queue;
    uint64_t event_physical[VIRTIO_INPUT_EVENT_BUFFER_COUNT];
    virtio_input_event_t *events[VIRTIO_INPUT_EVENT_BUFFER_COUNT];
    uint16_t event_buffer_count;
    uint16_t available_cursor;
    int16_t mouse_dx;
    int16_t mouse_dy;
    uint8_t mouse_buttons;
} virtio_input_device_t;

static virtio_input_device_t devices[VIRTIO_INPUT_DEVICE_LIMIT];
static uint32_t device_count;

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline void outl(uint16_t port, uint32_t value)
{
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t value;
    __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static uint8_t key_scancode(uint16_t key)
{
    static const uint8_t key_map[128] = {
        [1] = 0x01,
        [2] = 0x02, [3] = 0x03, [4] = 0x04, [5] = 0x05,
        [6] = 0x06, [7] = 0x07, [8] = 0x08, [9] = 0x09,
        [10] = 0x0A, [11] = 0x0B, [12] = 0x0C, [13] = 0x0D,
        [14] = 0x0E, [15] = 0x0F,
        [16] = 0x10, [17] = 0x11, [18] = 0x12, [19] = 0x13,
        [20] = 0x14, [21] = 0x15, [22] = 0x16, [23] = 0x17,
        [24] = 0x18, [25] = 0x19, [26] = 0x1A, [27] = 0x1B,
        [28] = 0x1C, [29] = 0x1D, [30] = 0x1E, [31] = 0x1F,
        [32] = 0x20, [33] = 0x21, [34] = 0x22, [35] = 0x23,
        [36] = 0x24, [37] = 0x25, [38] = 0x26, [39] = 0x27,
        [40] = 0x28, [41] = 0x29, [42] = 0x2A, [43] = 0x2B,
        [44] = 0x2C, [45] = 0x2D, [46] = 0x2E, [47] = 0x2F,
        [48] = 0x30, [49] = 0x31, [50] = 0x32, [51] = 0x33,
        [52] = 0x34, [53] = 0x35, [54] = 0x36, [55] = 0x37,
        [56] = 0x38, [57] = 0x39,
        [59] = 0x3B, [60] = 0x3C, [61] = 0x3D, [62] = 0x3E,
        [63] = 0x3F, [64] = 0x40, [65] = 0x41, [66] = 0x42,
        [67] = 0x43, [68] = 0x44,
        [87] = 0x57, [88] = 0x58
    };
    if (key < sizeof(key_map))
        return key_map[key];
    switch (key)
    {
        case 97: return 0x1C;
        case 100: return 0x35;
        case 103: return 0x48;
        case 105: return 0x4B;
        case 106: return 0x4D;
        case 108: return 0x50;
        case 111: return 0x47;
        case 113: return 0x4F;
        case 114: return 0x49;
        case 115: return 0x51;
        case 119: return 0x52;
        case 125: return 0x53;
        default: return 0;
    }
}

static uint8_t mouse_button_bit(uint16_t code)
{
    switch (code)
    {
        case VIRTIO_KEY_LEFT: return 1U;
        case VIRTIO_KEY_RIGHT: return 2U;
        case VIRTIO_KEY_MIDDLE: return 4U;
        default: return 0;
    }
}

static void handle_event(
    virtio_input_device_t *device,
    const virtio_input_event_t *event
)
{
    if (event->type == VIRTIO_EV_KEY)
    {
        if (device->keyboard)
        {
            uint8_t scancode = key_scancode(event->code);
            if (scancode)
            {
                if (event->value == 0)
                    scancode |= 0x80;
                keyboard_handle_scancode(scancode);
            }
        }
        if (device->mouse)
        {
            uint8_t bit = mouse_button_bit(event->code);
            if (bit)
            {
                if (event->value)
                    device->mouse_buttons |= bit;
                else
                    device->mouse_buttons &= (uint8_t)~bit;
            }
        }
    }
    else if (event->type == VIRTIO_EV_REL && device->mouse)
    {
        if (event->code == VIRTIO_REL_X)
            device->mouse_dx = (int16_t)(
                device->mouse_dx + event->value);
        else if (event->code == VIRTIO_REL_Y)
            device->mouse_dy = (int16_t)(
                device->mouse_dy + event->value);
    }
    else if (event->type == VIRTIO_EV_SYN &&
        event->code == VIRTIO_SYN_REPORT && device->mouse)
    {
        mouse_handle_usb_report(
            device->mouse_dx,
            device->mouse_dy,
            device->mouse_buttons);
        device->mouse_dx = 0;
        device->mouse_dy = 0;
    }
}

static bool initialize_device(
    const virtio_pci_device_t *transport,
    virtio_input_device_t *device
)
{
    const pci_device_t *pci = transport->pci_device;
    uint16_t io_base = transport->io_base;
    if (!virtio_pci_enable_device(pci))
        return false;
    device->transport = transport;
    device->modern = transport->modern;
    device->io_base = io_base;
    if (device->modern)
    {
        if (!virtio_pci_modern_init(
                transport, 0, 1, NULL, NULL))
            return false;
    }
    else
    {
        outb(io_base + 0x12, 0);
        outb(io_base + 0x12, VIRTIO_INPUT_STATUS_ACKNOWLEDGE);
        outb(io_base + 0x12,
            VIRTIO_INPUT_STATUS_ACKNOWLEDGE |
            VIRTIO_INPUT_STATUS_DRIVER);
        outl((uint16_t)(io_base + 0x04), 0U);
    }
    if (!(device->modern
            ? virtio_pci_modern_queue_init(
                transport, 0, &device->event_queue)
            : virtio_legacy_queue_init(
                io_base, 0, &device->event_queue)))
        return false;
    if (device->event_queue.size < 2)
        return false;
    device->event_buffer_count = device->event_queue.size;
    if (device->event_buffer_count > VIRTIO_INPUT_EVENT_BUFFER_COUNT)
        device->event_buffer_count = VIRTIO_INPUT_EVENT_BUFFER_COUNT;

    for (uint16_t i = 0; i < device->event_buffer_count; i++)
    {
        uint8_t *memory;
        if (!virtio_legacy_alloc_page(&device->event_physical[i], &memory))
            return false;
        device->events[i] = (virtio_input_event_t *)memory;
        device->event_queue.descriptors[i] = (virtio_descriptor_t){
            .address = device->event_physical[i],
            .length = sizeof(virtio_input_event_t),
            .flags = VIRTIO_DESC_F_WRITE
        };
        device->event_queue.available_ring[i] = i;
    }
    device->available_cursor = device->event_buffer_count;
    __asm__ volatile ("mfence" ::: "memory");
    *device->event_queue.available_index = device->event_buffer_count;
    if (device->modern)
        virtio_pci_modern_notify(transport, 0);
    else
    {
        virtio_legacy_notify(io_base, 0);
        outb(io_base + 0x12,
            VIRTIO_INPUT_STATUS_ACKNOWLEDGE |
            VIRTIO_INPUT_STATUS_DRIVER |
            VIRTIO_INPUT_STATUS_DRIVER_OK);
        return (inb(io_base + 0x12) &
            VIRTIO_INPUT_STATUS_DRIVER_OK) != 0;
    }
    return virtio_pci_modern_driver_ok(transport);
}

bool virtio_input_init(void)
{
    device_count = 0;
    for (uint32_t ordinal = 0;
         ordinal < VIRTIO_INPUT_DEVICE_LIMIT;
         ordinal++)
    {
        const virtio_pci_device_t *transport =
            virtio_pci_find_device(18, ordinal);
        if (!transport)
            break;
        virtio_input_device_t *device = &devices[device_count];
        *device = (virtio_input_device_t){0};
        if (!initialize_device(transport, device))
        {
            if (!transport->modern && transport->io_base)
                outb(transport->io_base + 0x12, 0x80);
            serial_log(
                SERIAL_LOG_WARN,
                "VirtIO input device initialization failed");
            continue;
        }
        device->keyboard = true;
        device->mouse = true;
        device->used = true;
        device_count++;
        serial_log(SERIAL_LOG_INFO,
            "Initialized VirtIO input device");
    }
    return true;
}

void virtio_input_poll(void)
{
    for (uint32_t i = 0; i < device_count; i++)
    {
        virtio_input_device_t *device = &devices[i];
        while (device->event_queue.used_cursor !=
            *device->event_queue.used_index)
        {
            __asm__ volatile ("mfence" ::: "memory");
            virtio_used_element_t element =
                device->event_queue.used_ring[
                    device->event_queue.used_cursor %
                    device->event_queue.size];
            device->event_queue.used_cursor++;
            if (element.id >= device->event_buffer_count ||
                element.length < sizeof(virtio_input_event_t))
                continue;
            handle_event(device, device->events[element.id]);
            uint16_t available = device->available_cursor++;
            device->event_queue.available_ring[
                available % device->event_queue.size] =
                    (uint16_t)element.id;
            __asm__ volatile ("mfence" ::: "memory");
            *device->event_queue.available_index = available + 1;
            if (device->modern)
                virtio_pci_modern_notify(device->transport, 0);
            else
                virtio_legacy_notify(device->io_base, 0);
        }
    }
}

uint32_t virtio_input_device_count(void)
{
    return device_count;
}