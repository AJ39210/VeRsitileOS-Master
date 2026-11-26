#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hid.h"
#include "keyboard.h"
#include "mouse.h"
#include "block.h"
#include "paging.h"
#include "pci.h"
#include "pmm.h"
#include "serial.h"
#include "timer.h"
#include "xhci.h"

#define XHCI_CONTROLLER_LIMIT 4
#define XHCI_DEVICE_LIMIT 16
#define XHCI_PAGE_SIZE 4096
#define XHCI_RING_TRBS 256
#define XHCI_MAX_CONFIG_SIZE 512
#define XHCI_MMIO_SIZE 0x10000
#define XHCI_TIMEOUT_MS 100

#define XHCI_USBCMD_RUN (1U << 0)
#define XHCI_USBCMD_RESET (1U << 1)
#define XHCI_USBSTS_HALTED (1U << 0)
#define XHCI_USBSTS_NOT_READY (1U << 11)
#define XHCI_PORT_CONNECTED (1U << 0)
#define XHCI_PORT_ENABLED (1U << 1)
#define XHCI_PORT_RESET (1U << 4)
#define XHCI_PORT_WARM_RESET (1U << 31)
enum
{
    TRB_TYPE_NORMAL = 1,
    TRB_TYPE_SETUP_STAGE = 2,
    TRB_TYPE_DATA_STAGE = 3,
    TRB_TYPE_STATUS_STAGE = 4,
    TRB_TYPE_LINK = 6,
    TRB_TYPE_ENABLE_SLOT = 9,
    TRB_TYPE_ADDRESS_DEVICE = 11,
    TRB_TYPE_CONFIGURE_ENDPOINT = 12,
    TRB_TYPE_EVALUATE_CONTEXT = 13,
    TRB_TYPE_TRANSFER_EVENT = 32,
    TRB_TYPE_COMMAND_EVENT = 33,
    TRB_TYPE_PORT_EVENT = 34
};

typedef struct __attribute__((packed, aligned(16)))
{
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
} xhci_trb_t;

typedef struct __attribute__((packed))
{
    uint64_t address;
    uint32_t size;
    uint32_t reserved;
} xhci_erst_entry_t;

typedef struct
{
    uint64_t physical;
    xhci_trb_t *trbs;
    uint16_t enqueue;
    uint8_t cycle;
} xhci_ring_t;

typedef struct
{
    bool used;
    bool keyboard;
    bool mouse;
    bool mass_storage;
    bool transfer_pending;
    uint8_t slot_id;
    uint8_t port;
    uint8_t speed;
    uint8_t endpoint_id;
    uint8_t storage_in_endpoint_id;
    uint8_t storage_out_endpoint_id;
    uint8_t interface_number;
    uint8_t interval;
    uint16_t max_packet;
    uint16_t storage_in_max_packet;
    uint16_t storage_out_max_packet;
    uint32_t storage_tag;
    uint64_t storage_sector_count;
    struct xhci_controller *controller;
    uint64_t *device_context;
    uint64_t *input_context;
    uint8_t *report_buffer;
    uint8_t *descriptor_buffer;
    uint8_t *configuration_buffer;
    uint8_t *storage_command_buffer;
    uint8_t *storage_data_buffer;
    xhci_ring_t control_ring;
    xhci_ring_t interrupt_ring;
    xhci_ring_t storage_in_ring;
    xhci_ring_t storage_out_ring;
    uint64_t pending_trb_physical;
} xhci_device_t;

typedef struct xhci_controller
{
    xhci_controller_info_t info;
    volatile uint8_t *registers;
    volatile uint8_t *operational;
    volatile uint8_t *runtime;
    volatile uint32_t *doorbells;
    uint8_t context_size;
    uint8_t max_slots;
    uint64_t *device_context_array;
    uint64_t device_context_array_physical;
    uint64_t *scratchpad_array;
    uint32_t scratchpad_count;
    uint64_t scratchpad_pages[256];
    xhci_ring_t command_ring;
    uint64_t event_ring_physical;
    xhci_trb_t *event_ring;
    uint64_t erst_physical;
    xhci_erst_entry_t *erst;
    uint16_t event_dequeue;
    uint8_t event_cycle;
    xhci_device_t devices[XHCI_DEVICE_LIMIT];
    bool running;
} xhci_controller_t;

static xhci_controller_t controllers[XHCI_CONTROLLER_LIMIT];
static uint32_t controller_count;
static uint32_t usb_disk_count;

static void start_interrupt_transfer(
    xhci_controller_t *controller,
    xhci_device_t *device
);

static uint32_t register_read32(
    const volatile uint8_t *base,
    uint32_t offset
)
{
    return *(const volatile uint32_t *)(base + offset);
}

static void register_write32(
    volatile uint8_t *base,
    uint32_t offset,
    uint32_t value
)
{
    *(volatile uint32_t *)(base + offset) = value;
}

static void register_write64(
    volatile uint8_t *base,
    uint32_t offset,
    uint64_t value
)
{
    *(volatile uint64_t *)(base + offset) = value;
}

static bool allocate_dma_page(uint64_t *physical, void **virtual_address)
{
    *physical = pmm_alloc_page();
    if (!*physical)
        return false;
    *virtual_address = paging_phys_to_virt(*physical);
    if (!*virtual_address)
    {
        pmm_free_page(*physical);
        *physical = 0;
        return false;
    }
    for (uint32_t i = 0; i < XHCI_PAGE_SIZE; i++)
        ((volatile uint8_t *)*virtual_address)[i] = 0;
    return true;
}

static bool allocate_ring(xhci_ring_t *ring)
{
    void *memory;
    if (!allocate_dma_page(&ring->physical, &memory))
        return false;
    ring->trbs = memory;
    ring->enqueue = 0;
    ring->cycle = 1;
    ring->trbs[XHCI_RING_TRBS - 1] = (xhci_trb_t){
        .parameter = ring->physical,
        .control = ((uint32_t)TRB_TYPE_LINK << 10) |
            (1U << 1) | 1U
    };
    return true;
}

static uint64_t trb_physical(const xhci_ring_t *ring, uint16_t index)
{
    return ring->physical + (uint64_t)index * sizeof(xhci_trb_t);
}

static uint16_t ring_reserve(xhci_ring_t *ring)
{
    uint16_t index = ring->enqueue;
    if (index == XHCI_RING_TRBS - 1)
    {
        ring->trbs[index] = (xhci_trb_t){
            .parameter = ring->physical,
            .control = ((uint32_t)TRB_TYPE_LINK << 10) |
                (1U << 1) | ring->cycle
        };
        index = 0;
        ring->cycle ^= 1;
    }
    ring->enqueue = index + 1;
    return index;
}

static bool wait_register(
    xhci_controller_t *controller,
    uint32_t offset,
    uint32_t mask,
    bool set
)
{
    uint64_t started = timer_get_milliseconds();
    for (uint32_t iteration = 0;
         iteration < 100000 &&
             timer_get_milliseconds() - started < XHCI_TIMEOUT_MS;
         iteration++)
    {
        bool value = (register_read32(
            controller->operational, offset) & mask) != 0;
        if (value == set)
            return true;
        __asm__ volatile ("pause");
    }
    serial_log(SERIAL_LOG_WARN, "xHCI controller register wait timed out");
    return false;
}

static bool acquire_firmware_ownership(xhci_controller_t *controller)
{
    uint32_t hcc = register_read32(controller->registers, 0x10);
    uint32_t offset = ((hcc >> 16) & 0xFFFFU) * 4U;
    if (!offset)
        return true;

    for (uint32_t visited = 0; offset && visited < 64; visited++)
    {
        uint32_t capability = register_read32(
            controller->registers, offset);
        uint8_t id = (uint8_t)capability;
        uint8_t next = (uint8_t)(capability >> 8);
        if (id == 1)
        {
            register_write32(
                controller->registers,
                offset,
                capability | (1U << 24));
            for (uint32_t wait = 0; wait < 10000000; wait++)
            {
                if (!(register_read32(
                        controller->registers, offset) & (1U << 16)))
                    break;
                __asm__ volatile ("pause");
            }
            if (register_read32(
                    controller->registers, offset) & (1U << 16))
            {
                serial_log(
                    SERIAL_LOG_WARN,
                    "xHCI firmware ownership handoff timed out");
                return false;
            }
            return true;
        }
        if (!next)
            break;
        offset += (uint32_t)next * 4U;
        if (offset >= XHCI_MMIO_SIZE)
            break;
    }
    return true;
}

static bool initialize_dma_structures(xhci_controller_t *controller)
{
    void *memory;
    if (!allocate_dma_page(
            &controller->command_ring.physical, &memory))
        return false;
    controller->command_ring.trbs = memory;
    controller->command_ring.enqueue = 0;
    controller->command_ring.cycle = 1;
    controller->command_ring.trbs[XHCI_RING_TRBS - 1] =
        (xhci_trb_t){
            .parameter = controller->command_ring.physical,
            .control = ((uint32_t)TRB_TYPE_LINK << 10) |
                (1U << 1) | 1U
        };

    if (!allocate_dma_page(
            &controller->event_ring_physical, &memory))
        return false;
    controller->event_ring = memory;

    if (!allocate_dma_page(&controller->erst_physical, &memory))
        return false;
    controller->erst = memory;
    controller->erst[0] = (xhci_erst_entry_t){
        .address = controller->event_ring_physical,
        .size = XHCI_RING_TRBS
    };

    if (!allocate_dma_page(
            &controller->device_context_array_physical, &memory))
        return false;
    controller->device_context_array = memory;

    uint32_t hcs2 = register_read32(controller->registers, 0x08);
    uint32_t scratchpad_high = (hcs2 >> 27) & 0x1F;
    uint32_t scratchpad_low = (hcs2 >> 21) & 0x1F;
    controller->scratchpad_count =
        (scratchpad_high << 5) | scratchpad_low;
    if (controller->scratchpad_count > 256)
        return false;

    if (controller->scratchpad_count)
    {
        uint64_t scratch_array_physical;
        if (!allocate_dma_page(
                &scratch_array_physical, &memory))
            return false;
        controller->scratchpad_array = memory;
        controller->device_context_array[0] = scratch_array_physical;
        for (uint32_t i = 0; i < controller->scratchpad_count; i++)
        {
            void *scratch_page;
            if (!allocate_dma_page(
                    &controller->scratchpad_pages[i],
                    &scratch_page))
                return false;
            controller->scratchpad_array[i] =
                controller->scratchpad_pages[i];
        }
    }

    return true;
}

static bool controller_start(xhci_controller_t *controller)
{
    uint32_t command = register_read32(controller->operational, 0x00);
    register_write32(
        controller->operational, 0x00, command & ~XHCI_USBCMD_RUN);
    if (!wait_register(
            controller, 0x04, XHCI_USBSTS_HALTED, true))
        return false;

    register_write32(
        controller->operational,
        0x00,
        register_read32(controller->operational, 0x00) |
            XHCI_USBCMD_RESET);
    if (!wait_register(
            controller, 0x00, XHCI_USBCMD_RESET, false))
        return false;
    if (!wait_register(
            controller, 0x04, XHCI_USBSTS_NOT_READY, false))
        return false;

    uint32_t page_size = register_read32(
        controller->operational, 0x08);
    if (!(page_size & 1))
        return false;
    if (!initialize_dma_structures(controller))
        return false;

    uint32_t runtime_offset =
        register_read32(controller->registers, 0x18) & ~0x1FU;
    uint32_t doorbell_offset =
        register_read32(controller->registers, 0x14) & ~3U;
    if (!runtime_offset || !doorbell_offset ||
        runtime_offset >= XHCI_MMIO_SIZE ||
        doorbell_offset >= XHCI_MMIO_SIZE)
        return false;

    controller->runtime = controller->registers + runtime_offset;
    controller->doorbells = (volatile uint32_t *)(
        controller->registers + doorbell_offset);

    register_write64(
        controller->operational,
        0x30,
        controller->device_context_array_physical);
    register_write64(
        controller->operational,
        0x18,
        controller->command_ring.physical | 1U);
    register_write32(
        controller->operational,
        0x38,
        controller->max_slots);

    volatile uint8_t *interrupter = controller->runtime + 0x20;
    register_write32(interrupter, 0x08, 1);
    register_write64(interrupter, 0x10, controller->erst_physical);
    register_write64(interrupter, 0x18, controller->event_ring_physical);
    register_write32(interrupter, 0x00, 0);

    command = register_read32(controller->operational, 0x00);
    command &= ~(1U << 2);
    register_write32(
        controller->operational, 0x00, command | XHCI_USBCMD_RUN);
    if (!wait_register(
            controller, 0x04, XHCI_USBSTS_HALTED, false))
        return false;
    controller->running = true;
    return true;
}

static bool event_next(xhci_controller_t *controller, xhci_trb_t *event)
{
    xhci_trb_t *current =
        &controller->event_ring[controller->event_dequeue];
    __asm__ volatile ("mfence" ::: "memory");
    if ((current->control & 1U) != controller->event_cycle)
        return false;
    *event = *current;

    controller->event_dequeue++;
    if (controller->event_dequeue == XHCI_RING_TRBS)
    {
        controller->event_dequeue = 0;
        controller->event_cycle ^= 1;
    }
    volatile uint8_t *interrupter = controller->runtime + 0x20;
    uint64_t dequeue = controller->event_ring_physical +
        (uint64_t)controller->event_dequeue * sizeof(xhci_trb_t);
    register_write64(interrupter, 0x18, dequeue | (1U << 3));
    return true;
}

static bool command_wait(
    xhci_controller_t *controller,
    uint64_t command_physical,
    uint8_t *slot_id
)
{
    uint64_t started = timer_get_milliseconds();
    for (uint32_t iteration = 0;
         iteration < 1000000 &&
             timer_get_milliseconds() - started < XHCI_TIMEOUT_MS;
         iteration++)
    {
        xhci_trb_t event;
        while (event_next(controller, &event))
        {
            uint8_t type = (uint8_t)((event.control >> 10) & 0x3F);
            if (type != TRB_TYPE_COMMAND_EVENT ||
                event.parameter != command_physical)
                continue;
            uint8_t completion = (uint8_t)(event.status >> 24);
            if (slot_id)
                *slot_id = (uint8_t)(event.control >> 24);
            return completion == 1;
        }
        __asm__ volatile ("pause");
    }
    return false;
}

static bool command_submit(
    xhci_controller_t *controller,
    uint32_t type,
    uint64_t parameter,
    uint32_t status,
    uint32_t extra_control,
    uint8_t *slot_id
)
{
    uint16_t index = ring_reserve(&controller->command_ring);
    xhci_trb_t *trb = &controller->command_ring.trbs[index];
    uint64_t physical = trb_physical(
        &controller->command_ring, index);
    trb->parameter = parameter;
    trb->status = status;
    trb->control = (type << 10) | extra_control |
        controller->command_ring.cycle;
    __asm__ volatile ("mfence" ::: "memory");
    controller->doorbells[0] = 0;
    return command_wait(controller, physical, slot_id);
}

static void dispatch_hid_transfer_event(
    xhci_controller_t *controller,
    const xhci_trb_t *event
)
{
    uint8_t slot_id = (uint8_t)(event->control >> 24);
    if (!slot_id || slot_id >= XHCI_DEVICE_LIMIT)
        return;

    xhci_device_t *device = &controller->devices[slot_id];
    if (!device->used || !device->transfer_pending ||
        (!device->keyboard && !device->mouse) ||
        (event->parameter & ~0xFULL) !=
            (device->pending_trb_physical & ~0xFULL))
        return;

    device->transfer_pending = false;
    if ((uint8_t)(event->status >> 24) == 1)
    {
        if (device->keyboard)
            (void)hid_handle_boot_keyboard_report(device->report_buffer);
        else
            (void)hid_handle_boot_mouse_report(device->report_buffer);
    }
    start_interrupt_transfer(controller, device);
}

static bool transfer_wait(
    xhci_controller_t *controller,
    uint8_t slot_id,
    uint64_t expected_trb,
    uint8_t *completion_code
)
{
    uint64_t started = timer_get_milliseconds();
    for (uint32_t iteration = 0;
         iteration < 1000000 &&
             timer_get_milliseconds() - started < XHCI_TIMEOUT_MS;
         iteration++)
    {
        xhci_trb_t event;
        while (event_next(controller, &event))
        {
            uint8_t type = (uint8_t)((event.control >> 10) & 0x3F);
            if (type != TRB_TYPE_TRANSFER_EVENT)
                continue;
            if ((uint8_t)(event.control >> 24) == slot_id &&
                (event.parameter & ~0xFULL) ==
                    (expected_trb & ~0xFULL))
            {
                *completion_code = (uint8_t)(event.status >> 24);
                if (*completion_code != 1)
                {
                    serial_log_begin(SERIAL_LOG_WARN);
                    serial_write("xHCI transfer completion code ");
                    serial_write_dec(*completion_code);
                    serial_log_end();
                }
                return *completion_code == 1;
            }
            if ((uint8_t)(event.control >> 24) == slot_id)
            {
                serial_log_begin(SERIAL_LOG_WARN);
                serial_write("xHCI transfer event TRB mismatch: got ");
                serial_write_hex(event.parameter);
                serial_write(" expected ");
                serial_write_hex(expected_trb);
                serial_log_end();
            }
            dispatch_hid_transfer_event(controller, &event);
        }
        __asm__ volatile ("pause");
    }
    serial_log(SERIAL_LOG_WARN, "xHCI transfer timed out");
    return false;
}

static bool bulk_transfer(
    xhci_controller_t *controller,
    xhci_device_t *device,
    bool direction_in,
    void *buffer,
    uint32_t length
)
{
    if (!buffer || !length || length > XHCI_PAGE_SIZE ||
        !device->mass_storage)
        return false;

    xhci_ring_t *ring = direction_in
        ? &device->storage_in_ring
        : &device->storage_out_ring;
    uint8_t endpoint_id = direction_in
        ? device->storage_in_endpoint_id
        : device->storage_out_endpoint_id;
    uint64_t address = (uint64_t)(uintptr_t)buffer;
    uint64_t hhdm = paging_get_hhdm_offset();
    if (address < hhdm)
        return false;

    uint16_t index = ring_reserve(ring);
    uint64_t trb_physical_address = trb_physical(ring, index);
    ring->trbs[index] = (xhci_trb_t){
        .parameter = address - hhdm,
        .status = length,
        .control = (TRB_TYPE_NORMAL << 10) | (1U << 5) |
            ring->cycle
    };
    __asm__ volatile ("mfence" ::: "memory");
    controller->doorbells[device->slot_id] = endpoint_id;
    uint8_t completion = 0;
    return transfer_wait(
        controller,
        device->slot_id,
        trb_physical_address,
        &completion);
}

static bool control_transfer(
    xhci_controller_t *controller,
    xhci_device_t *device,
    const uint8_t setup[8],
    void *data,
    uint16_t length,
    bool data_in
)
{
    xhci_ring_t *ring = &device->control_ring;
    uint64_t data_physical = 0;
    if (length)
    {
        uint64_t virtual_address =
            (uint64_t)(uintptr_t)data;
        uint64_t hhdm = paging_get_hhdm_offset();
        if (virtual_address < hhdm)
            return false;
        data_physical = virtual_address - hhdm;
    }

    uint16_t setup_index = ring_reserve(ring);
    uint64_t setup_physical = trb_physical(ring, setup_index);
    uint64_t setup_parameter = 0;
    for (uint32_t i = 0; i < 8; i++)
        setup_parameter |= (uint64_t)setup[i] << (8 * i);
    ring->trbs[setup_index] = (xhci_trb_t){
        .parameter = setup_parameter,
        .status = 8U,
        .control = (TRB_TYPE_SETUP_STAGE << 10) |
            (1U << 6) | (1U << 4) |
            (length ? ((data_in ? 3U : 2U) << 16) : 0) |
            ring->cycle
    };

    if (length)
    {
        uint16_t data_index = ring_reserve(ring);
        ring->trbs[data_index] = (xhci_trb_t){
            .parameter = data_physical,
            .status = length,
            .control = (TRB_TYPE_DATA_STAGE << 10) |
                (1U << 4) | (data_in ? (1U << 16) : 0) |
                ring->cycle
        };
    }

    uint16_t status_index = ring_reserve(ring);
    uint64_t status_physical = trb_physical(ring, status_index);
    ring->trbs[status_index] = (xhci_trb_t){
        .control = (TRB_TYPE_STATUS_STAGE << 10) |
            (1U << 5) |
            ((length ? !data_in : true) ? (1U << 16) : 0) |
            ring->cycle
    };
    (void)setup_physical;
    __asm__ volatile ("mfence" ::: "memory");
    controller->doorbells[device->slot_id] = 1;
    uint8_t completion = 0;
    return transfer_wait(
        controller,
        device->slot_id,
        status_physical,
        &completion);
}

static bool usb_control(
    xhci_controller_t *controller,
    xhci_device_t *device,
    uint8_t request_type,
    uint8_t request,
    uint16_t value,
    uint16_t index,
    void *data,
    uint16_t length,
    bool data_in
)
{
    uint8_t setup[8] = {
        request_type,
        request,
        (uint8_t)value,
        (uint8_t)(value >> 8),
        (uint8_t)index,
        (uint8_t)(index >> 8),
        (uint8_t)length,
        (uint8_t)(length >> 8)
    };
    return control_transfer(
        controller, device, setup, data, length, data_in);
}

static bool address_device(
    xhci_controller_t *controller,
    xhci_device_t *device
)
{
    void *memory;
    uint64_t physical;
    if (!allocate_dma_page(&physical, &memory))
        return false;
    device->device_context = memory;
    controller->device_context_array[device->slot_id] = physical;

    if (!allocate_dma_page(&physical, &memory))
        return false;
    device->input_context = memory;
    if (!allocate_ring(&device->control_ring))
        return false;

    uint32_t *input = (uint32_t *)device->input_context;
    uint8_t dwords = controller->context_size / 4;
    uint32_t *slot = input + dwords;
    uint32_t *ep0 = slot + dwords;
    input[1] = 0x3;
    slot[0] = ((uint32_t)device->speed << 20) | (1U << 27);
    slot[1] = (uint32_t)device->port << 16;
    uint16_t initial_packet =
        device->speed == 3 ? 64 :
        device->speed == 4 ? 512 : 8;
    ep0[1] = (3U << 1) | (4U << 3) |
        ((uint32_t)initial_packet << 16);
    ep0[2] = (uint32_t)device->control_ring.physical | 1U;
    ep0[3] = (uint32_t)(device->control_ring.physical >> 32);
    ep0[4] = 8;

    uint64_t input_physical =
        (uint64_t)(uintptr_t)device->input_context -
        paging_get_hhdm_offset();
    return command_submit(
        controller,
        TRB_TYPE_ADDRESS_DEVICE,
        input_physical,
        0,
        (uint32_t)device->slot_id << 24,
        NULL);
}

static bool evaluate_ep0_packet_size(
    xhci_controller_t *controller,
    xhci_device_t *device,
    uint16_t packet_size
)
{
    uint32_t *input = (uint32_t *)device->input_context;
    uint8_t dwords = controller->context_size / 4;
    uint32_t *ep0 = input + 2 * dwords;
    input[1] = 1U << 1;
    ep0[1] = (3U << 1) | (4U << 3) |
        ((uint32_t)packet_size << 16);
    uint64_t physical =
        (uint64_t)(uintptr_t)device->input_context -
        paging_get_hhdm_offset();
    return command_submit(
        controller,
        TRB_TYPE_EVALUATE_CONTEXT,
        physical,
        0,
        (uint32_t)device->slot_id << 24,
        NULL);
}

static bool get_descriptor(
    xhci_controller_t *controller,
    xhci_device_t *device,
    uint8_t descriptor_type,
    uint8_t descriptor_index,
    uint16_t length,
    uint8_t *buffer
)
{
    return usb_control(
        controller,
        device,
        0x80,
        6,
        (uint16_t)((uint16_t)descriptor_type << 8) | descriptor_index,
        0,
        buffer,
        length,
        true);
}

static bool configure_interrupt_endpoint(
    xhci_controller_t *controller,
    xhci_device_t *device,
    uint8_t endpoint_address,
    uint16_t max_packet,
    uint8_t interval
)
{
    uint8_t endpoint_number = endpoint_address & 0x0F;
    bool direction_in = (endpoint_address & 0x80) != 0;
    uint8_t context_index =
        (uint8_t)(endpoint_number * 2 + (direction_in ? 1 : 0));
    if (!endpoint_number || !direction_in ||
        context_index >= 31 || max_packet > 1024)
        return false;

    if (!allocate_ring(&device->interrupt_ring))
        return false;
    void *report;
    uint64_t ignored_physical;
    if (!allocate_dma_page(&ignored_physical, &report))
        return false;
    device->report_buffer = report;

    uint8_t dwords = controller->context_size / 4;
    uint32_t *input = (uint32_t *)device->input_context;
    uint32_t *slot = input + dwords;
    uint32_t *endpoint = input + (context_index + 1) * dwords;
    uint32_t *output_slot = (uint32_t *)device->device_context;
    for (uint8_t i = 0; i < dwords; i++)
        slot[i] = output_slot[i];
    uint8_t interval_value = interval;
    if (device->speed < 3)
    {
        uint32_t microframes = (uint32_t)(interval ? interval : 1) * 8;
        interval_value = 0;
        while (microframes)
        {
            interval_value++;
            microframes >>= 1;
        }
    }
    else if (!interval_value)
        interval_value = 1;
    if (interval_value > 15)
        interval_value = 15;

    input[1] = 1U | (1U << context_index);
    slot[0] &= ~(0x1FU << 27);
    slot[0] |= (uint32_t)context_index << 27;
    endpoint[0] = (uint32_t)interval_value << 16;
    endpoint[1] = (3U << 1) | (7U << 3) |
        ((uint32_t)max_packet << 16);
    endpoint[2] =
        (uint32_t)device->interrupt_ring.physical | 1U;
    endpoint[3] =
        (uint32_t)(device->interrupt_ring.physical >> 32);
    endpoint[4] = ((uint32_t)max_packet << 16) | max_packet;

    uint64_t input_physical =
        (uint64_t)(uintptr_t)device->input_context -
        paging_get_hhdm_offset();
    if (!command_submit(
            controller,
            TRB_TYPE_CONFIGURE_ENDPOINT,
            input_physical,
            0,
            (uint32_t)device->slot_id << 24,
            NULL))
        return false;

    device->endpoint_id = context_index;
    device->max_packet = max_packet;
    device->interval = interval;
    return true;
}

static bool configure_bulk_endpoints(
    xhci_controller_t *controller,
    xhci_device_t *device,
    uint8_t in_address,
    uint16_t in_packet,
    uint8_t out_address,
    uint16_t out_packet
)
{
    uint8_t in_number = in_address & 0x0F;
    uint8_t out_number = out_address & 0x0F;
    uint8_t in_context = (uint8_t)(in_number * 2 + 1);
    uint8_t out_context = (uint8_t)(out_number * 2);
    if (!(in_address & 0x80) || (out_address & 0x80) ||
        !in_number || !out_number || in_context >= 31 ||
        out_context >= 31 || !in_packet || !out_packet ||
        in_packet > 1024 || out_packet > 1024)
        return false;

    if (!allocate_ring(&device->storage_in_ring) ||
        !allocate_ring(&device->storage_out_ring))
        return false;

    uint8_t dwords = controller->context_size / 4;
    uint32_t *input = (uint32_t *)device->input_context;
    uint32_t *slot = input + dwords;
    uint32_t *in_endpoint = input + (in_context + 1) * dwords;
    uint32_t *out_endpoint = input + (out_context + 1) * dwords;
    uint32_t *output_slot = (uint32_t *)device->device_context;
    for (uint8_t i = 0; i < dwords; i++)
        slot[i] = output_slot[i];

    input[1] = 1U | (1U << in_context) | (1U << out_context);
    uint8_t highest_context =
        in_context > out_context ? in_context : out_context;
    slot[0] &= ~(0x1FU << 27);
    slot[0] |= (uint32_t)highest_context << 27;

    in_endpoint[1] = (3U << 1) | (6U << 3) |
        ((uint32_t)in_packet << 16);
    in_endpoint[2] =
        (uint32_t)device->storage_in_ring.physical | 1U;
    in_endpoint[3] =
        (uint32_t)(device->storage_in_ring.physical >> 32);
    in_endpoint[4] = in_packet;

    out_endpoint[1] = (3U << 1) | (2U << 3) |
        ((uint32_t)out_packet << 16);
    out_endpoint[2] =
        (uint32_t)device->storage_out_ring.physical | 1U;
    out_endpoint[3] =
        (uint32_t)(device->storage_out_ring.physical >> 32);
    out_endpoint[4] = out_packet;

    uint64_t input_physical =
        (uint64_t)(uintptr_t)device->input_context -
        paging_get_hhdm_offset();
    if (!command_submit(
            controller,
            TRB_TYPE_CONFIGURE_ENDPOINT,
            input_physical,
            0,
            (uint32_t)device->slot_id << 24,
            NULL))
        return false;

    device->storage_in_endpoint_id = in_context;
    device->storage_out_endpoint_id = out_context;
    device->storage_in_max_packet = in_packet;
    device->storage_out_max_packet = out_packet;
    return true;
}

static void write_le32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static uint32_t read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
        ((uint32_t)bytes[1] << 8) |
        ((uint32_t)bytes[2] << 16) |
        ((uint32_t)bytes[3] << 24);
}

static uint32_t read_be32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24) |
        ((uint32_t)bytes[1] << 16) |
        ((uint32_t)bytes[2] << 8) |
        (uint32_t)bytes[3];
}

static uint64_t read_be64(const uint8_t *bytes)
{
    return ((uint64_t)read_be32(bytes) << 32) |
        read_be32(bytes + 4);
}

static bool storage_scsi_command(
    xhci_device_t *device,
    const uint8_t *cdb,
    uint8_t cdb_length,
    bool data_in,
    void *data,
    uint32_t data_length
)
{
    if (!device || !device->mass_storage || !cdb ||
        !cdb_length || cdb_length > 16 ||
        data_length > XHCI_PAGE_SIZE ||
        (data_length && !data))
        return false;

    uint8_t *command = device->storage_command_buffer;
    uint8_t *status = command + 32;
    uint8_t *dma_data = device->storage_data_buffer;
    for (uint32_t i = 0; i < 31; i++)
        command[i] = 0;
    for (uint32_t i = 0; i < 13; i++)
        status[i] = 0;

    uint32_t tag = ++device->storage_tag;
    write_le32(command, 0x43425355U);
    write_le32(command + 4, tag);
    write_le32(command + 8, data_length);
    command[12] = data_in ? 0x80 : 0;
    command[13] = 0;
    command[14] = cdb_length;
    for (uint32_t i = 0; i < cdb_length; i++)
        command[15 + i] = cdb[i];

    if (!data_in && data_length)
    {
        const uint8_t *source = data;
        for (uint32_t i = 0; i < data_length; i++)
            dma_data[i] = source[i];
    }

    if (!bulk_transfer(
            device->controller,
            device,
            false,
            command,
            31))
        return false;
    if (data_length &&
        !bulk_transfer(
            device->controller,
            device,
            data_in,
            dma_data,
            data_length))
        return false;
    if (!bulk_transfer(
            device->controller,
            device,
            true,
            status,
            13))
        return false;

    if (read_le32(status) != 0x53425355U ||
        read_le32(status + 4) != tag ||
        read_le32(status + 8) != 0 ||
        status[12] != 0)
        return false;

    if (data_in && data_length)
    {
        uint8_t *destination = data;
        for (uint32_t i = 0; i < data_length; i++)
            destination[i] = dma_data[i];
    }
    return true;
}

static bool usb_storage_read(
    void *context,
    uint64_t lba,
    uint32_t count,
    void *buffer
)
{
    xhci_device_t *device = context;
    uint8_t *output = buffer;
    while (count)
    {
        uint16_t sectors = count > 8 ? 8 : (uint16_t)count;
        uint8_t cdb[16] = {0};
        uint8_t cdb_length;
        if (lba <= UINT32_MAX &&
            (uint64_t)sectors - 1 <= UINT32_MAX - lba)
        {
            cdb[0] = 0x28;
            cdb[2] = (uint8_t)(lba >> 24);
            cdb[3] = (uint8_t)(lba >> 16);
            cdb[4] = (uint8_t)(lba >> 8);
            cdb[5] = (uint8_t)lba;
            cdb[7] = (uint8_t)(sectors >> 8);
            cdb[8] = (uint8_t)sectors;
            cdb_length = 10;
        }
        else
        {
            cdb[0] = 0x88;
            for (uint32_t i = 0; i < 8; i++)
                cdb[2 + i] = (uint8_t)(lba >> ((7 - i) * 8));
            cdb[10] = (uint8_t)(sectors >> 24);
            cdb[11] = (uint8_t)(sectors >> 16);
            cdb[12] = (uint8_t)(sectors >> 8);
            cdb[13] = (uint8_t)sectors;
            cdb_length = 16;
        }
        uint32_t bytes = (uint32_t)sectors * BLOCK_SECTOR_SIZE;
        if (!storage_scsi_command(
                device, cdb, cdb_length, true, output, bytes))
            return false;
        lba += sectors;
        count -= sectors;
        output += bytes;
    }
    return true;
}

static bool usb_storage_write(
    void *context,
    uint64_t lba,
    uint32_t count,
    const void *buffer
)
{
    xhci_device_t *device = context;
    const uint8_t *input = buffer;
    while (count)
    {
        uint16_t sectors = count > 8 ? 8 : (uint16_t)count;
        uint8_t cdb[16] = {0};
        uint8_t cdb_length;
        if (lba <= UINT32_MAX &&
            (uint64_t)sectors - 1 <= UINT32_MAX - lba)
        {
            cdb[0] = 0x2A;
            cdb[2] = (uint8_t)(lba >> 24);
            cdb[3] = (uint8_t)(lba >> 16);
            cdb[4] = (uint8_t)(lba >> 8);
            cdb[5] = (uint8_t)lba;
            cdb[7] = (uint8_t)(sectors >> 8);
            cdb[8] = (uint8_t)sectors;
            cdb_length = 10;
        }
        else
        {
            cdb[0] = 0x8A;
            for (uint32_t i = 0; i < 8; i++)
                cdb[2 + i] = (uint8_t)(lba >> ((7 - i) * 8));
            cdb[10] = (uint8_t)(sectors >> 24);
            cdb[11] = (uint8_t)(sectors >> 16);
            cdb[12] = (uint8_t)(sectors >> 8);
            cdb[13] = (uint8_t)sectors;
            cdb_length = 16;
        }
        uint32_t bytes = (uint32_t)sectors * BLOCK_SECTOR_SIZE;
        if (!storage_scsi_command(
                device, cdb, cdb_length, false, (void *)input, bytes))
            return false;
        lba += sectors;
        count -= sectors;
        input += bytes;
    }
    return true;
}

static bool initialize_usb_storage(xhci_device_t *device)
{
    uint64_t physical;
    void *memory;
    if (!allocate_dma_page(&physical, &memory))
        return false;
    device->storage_command_buffer = memory;
    if (!allocate_dma_page(&physical, &memory))
        return false;
    device->storage_data_buffer = memory;

    uint8_t cdb[16] = {0};
    cdb[0] = 0x00;
    for (uint32_t attempt = 0; attempt < 3; attempt++)
    {
        if (storage_scsi_command(device, cdb, 6, false, NULL, 0))
            break;
        uint8_t sense_cdb[16] = {0};
        uint8_t sense_data[18];
        sense_cdb[0] = 0x03;
        sense_cdb[4] = sizeof(sense_data);
        (void)storage_scsi_command(
            device, sense_cdb, 6, true, sense_data, sizeof(sense_data));
        if (attempt == 2)
            return false;
    }

    uint8_t capacity_cdb[16] = {0};
    uint8_t capacity_data[32];
    capacity_cdb[0] = 0x25;
    if (!storage_scsi_command(
            device,
            capacity_cdb,
            10,
            true,
            capacity_data,
            8))
        return false;

    uint32_t last_lba = read_be32(capacity_data);
    uint32_t sector_size = read_be32(capacity_data + 4);
    if (last_lba == UINT32_MAX)
    {
        capacity_cdb[0] = 0x9E;
        capacity_cdb[1] = 0x10;
        capacity_cdb[13] = sizeof(capacity_data);
        if (!storage_scsi_command(
                device,
                capacity_cdb,
                sizeof(capacity_cdb),
                true,
                capacity_data,
                sizeof(capacity_data)))
            return false;
        uint64_t last_lba_64 = read_be64(capacity_data);
        if (last_lba_64 == UINT64_MAX)
            return false;
        device->storage_sector_count = last_lba_64 + 1;
        sector_size = read_be32(capacity_data + 8);
    }
    else
    {
        device->storage_sector_count = (uint64_t)last_lba + 1;
    }
    if (sector_size != BLOCK_SECTOR_SIZE)
        return false;
    if (!device->storage_sector_count)
        return false;

    block_device_t block = {0};
    static const char prefix[] = "usb";
    uint32_t number = usb_disk_count;
    block.name[0] = prefix[0];
    block.name[1] = prefix[1];
    block.name[2] = prefix[2];
    if (number < 10)
        block.name[3] = (char)('0' + number);
    else
    {
        block.name[3] = (char)('0' + (number / 10) % 10);
        block.name[4] = (char)('0' + number % 10);
    }
    block.sector_count = device->storage_sector_count;
    block.sector_size = BLOCK_SECTOR_SIZE;
    block.writable = true;
    block.context = device;
    block.read = usb_storage_read;
    block.write = usb_storage_write;
    if (!block_register(&block))
        return false;
    usb_disk_count++;

    serial_log_begin(SERIAL_LOG_INFO);
    serial_write("USB mass-storage disk ");
    serial_write(block.name);
    serial_write(" ready (");
    serial_write_dec(device->storage_sector_count);
    serial_write(" sectors)");
    serial_log_end();
    return true;
}

static bool enumerate_usb_device(
    xhci_controller_t *controller,
    uint8_t port,
    uint8_t speed
)
{
#define ENUMERATION_FAILED(reason) \
    do { \
        serial_log_begin(SERIAL_LOG_WARN); \
        serial_write("USB enumeration failed: "); \
        serial_write(reason); \
        serial_log_end(); \
        return false; \
    } while (0)
    uint8_t slot_id = 0;
    if (!command_submit(
            controller,
            TRB_TYPE_ENABLE_SLOT,
            0,
            0,
            0,
            &slot_id) ||
        !slot_id || slot_id > controller->max_slots ||
        slot_id >= XHCI_DEVICE_LIMIT)
        ENUMERATION_FAILED("enable slot");

    xhci_device_t *device = &controller->devices[slot_id];
    *device = (xhci_device_t){
        .slot_id = slot_id,
        .port = port,
        .speed = speed,
        .controller = controller
    };
    if (!address_device(controller, device))
        ENUMERATION_FAILED("address device");

    uint64_t buffer_physical;
    void *buffer;
    if (!allocate_dma_page(&buffer_physical, &buffer))
        ENUMERATION_FAILED("allocate descriptor buffer");
    device->descriptor_buffer = buffer;
    if (!allocate_dma_page(&buffer_physical, &buffer))
        ENUMERATION_FAILED("allocate configuration buffer");
    device->configuration_buffer = buffer;

    uint8_t *descriptor = device->descriptor_buffer;
    if (!get_descriptor(
            controller, device, 1, 0, 18, descriptor) ||
        descriptor[0] < 18 ||
        descriptor[1] != 1)
        ENUMERATION_FAILED("read device descriptor");

    uint16_t packet_size = descriptor[7];
    if (device->speed == 4)
        packet_size = 512;
    if (!packet_size ||
        !evaluate_ep0_packet_size(controller, device, packet_size))
        ENUMERATION_FAILED("evaluate control endpoint");

    uint8_t *config_header = device->descriptor_buffer;
    if (!get_descriptor(
            controller, device, 2, 0, 9, config_header) ||
        config_header[0] < 9 ||
        config_header[1] != 2)
        ENUMERATION_FAILED("read configuration header");
    uint16_t total_length =
        (uint16_t)config_header[2] |
        ((uint16_t)config_header[3] << 8);
    if (total_length < 9 ||
        total_length > XHCI_MAX_CONFIG_SIZE)
        ENUMERATION_FAILED("invalid configuration length");

    uint8_t *configuration = device->configuration_buffer;
    if (!get_descriptor(
            controller,
            device,
            2,
            0,
            total_length,
            configuration))
        ENUMERATION_FAILED("read configuration descriptors");

    uint8_t configuration_value = config_header[5];
    bool current_hid = false;
    bool current_storage = false;
    bool found_hid = false;
    bool found_storage = false;
    uint8_t interface_number = 0;
    uint8_t hid_interface_number = 0;
    uint8_t storage_interface_number = 0;
    uint8_t protocol = 0;
    uint8_t hid_endpoint_address = 0;
    uint16_t hid_endpoint_packet = 0;
    uint8_t endpoint_interval = 0;
    uint8_t storage_in_address = 0;
    uint8_t storage_out_address = 0;
    uint16_t storage_in_packet = 0;
    uint16_t storage_out_packet = 0;
    size_t offset = 0;
    while (offset + 2 <= total_length)
    {
        uint8_t descriptor_length = configuration[offset];
        uint8_t descriptor_type = configuration[offset + 1];
        if (descriptor_length < 2 ||
            descriptor_length > total_length - offset)
            ENUMERATION_FAILED("malformed configuration descriptors");
        const uint8_t *entry = configuration + offset;
        if (descriptor_type == 4 && descriptor_length >= 9)
        {
            current_hid = entry[5] == 3 &&
                entry[6] == 1 &&
                (entry[7] == 1 || entry[7] == 2);
            current_storage = entry[5] == 8 &&
                entry[6] == 6 &&
                entry[7] == 0x50;
            if (current_hid)
            {
                interface_number = entry[2];
                protocol = entry[7];
            }
            if (current_storage)
                interface_number = entry[2];
        }
        else if (descriptor_type == 5 && descriptor_length >= 7 &&
            current_hid && (entry[2] & 0x80) &&
            (entry[3] & 3) == 3 && !hid_endpoint_address)
        {
            hid_endpoint_address = entry[2];
            hid_endpoint_packet = (uint16_t)entry[4] |
                ((uint16_t)(entry[5] & 0x07) << 8);
            endpoint_interval = entry[6];
            hid_interface_number = interface_number;
            found_hid = true;
        }
        else if (descriptor_type == 5 && descriptor_length >= 7 &&
            current_storage && (entry[3] & 3) == 2)
        {
            uint16_t packet = (uint16_t)entry[4] |
                ((uint16_t)(entry[5] & 0x07) << 8);
            if (entry[2] & 0x80)
            {
                if (!storage_in_address)
                {
                    storage_in_address = entry[2];
                    storage_in_packet = packet;
                }
            }
            else if (!storage_out_address)
            {
                storage_out_address = entry[2];
                storage_out_packet = packet;
            }
            storage_interface_number = interface_number;
        }
        offset += descriptor_length;
    }
    found_storage = storage_in_address && storage_out_address &&
        storage_in_packet && storage_out_packet;
    found_hid = found_hid && hid_endpoint_address &&
        hid_endpoint_packet && hid_endpoint_packet <= XHCI_PAGE_SIZE;
    if (!found_storage && !found_hid)
        ENUMERATION_FAILED("no supported HID or mass-storage interface");

    if (!usb_control(
            controller,
            device,
            0,
            9,
            configuration_value,
            0,
            NULL,
            0,
            false))
        ENUMERATION_FAILED("set USB configuration");

    if (!found_storage &&
        (!usb_control(
            controller,
            device,
            0x21,
            11,
            0,
            hid_interface_number,
            NULL,
            0,
            false) ||
        !usb_control(
            controller,
            device,
            0x21,
            10,
            0,
            hid_interface_number,
            NULL,
            0,
            false)))
        ENUMERATION_FAILED("configure boot HID interface");

    if (found_storage)
    {
        device->mass_storage = true;
        if (!configure_bulk_endpoints(
                controller,
                device,
                storage_in_address,
                storage_in_packet,
                storage_out_address,
                storage_out_packet))
            ENUMERATION_FAILED("configure mass-storage bulk endpoints");
        device->interface_number = storage_interface_number;
        if (!initialize_usb_storage(device))
            ENUMERATION_FAILED("initialize mass-storage device");
        device->used = true;
        serial_log_begin(SERIAL_LOG_INFO);
        serial_write("USB mass storage configured on xHCI port ");
        serial_write_dec(port);
        serial_log_end();
    }
    else
    {
        if (!configure_interrupt_endpoint(
                controller,
                device,
                hid_endpoint_address,
                hid_endpoint_packet,
                endpoint_interval))
            ENUMERATION_FAILED("configure HID interrupt endpoint");

        device->interface_number = hid_interface_number;
        device->keyboard = protocol == 1;
        device->mouse = protocol == 2;
        device->used = true;
        serial_log_begin(SERIAL_LOG_INFO);
        serial_write("USB HID ");
        serial_write(device->keyboard ? "keyboard" : "mouse");
        serial_write(" configured on xHCI port ");
        serial_write_dec(port);
        serial_log_end();
    }
#undef ENUMERATION_FAILED
    return true;
}

static void scan_ports(xhci_controller_t *controller)
{
    volatile uint8_t *ports = controller->operational + 0x400;
    for (uint8_t port = 1; port <= controller->info.max_ports; port++)
    {
        volatile uint8_t *port_register =
            ports + (uint32_t)(port - 1) * 0x10;
        uint32_t status = register_read32(port_register, 0);
        if (!(status & XHCI_PORT_CONNECTED))
            continue;

        serial_log_begin(SERIAL_LOG_INFO);
        serial_write("USB device connected on xHCI port ");
        serial_write_dec(port);
        serial_log_end();
        uint8_t speed = (uint8_t)((status >> 10) & 0x0F);
        uint32_t reset = speed == 4
            ? XHCI_PORT_WARM_RESET
            : XHCI_PORT_RESET;
        register_write32(
            (volatile uint8_t *)port_register,
            0,
            (status & (1U << 9)) | reset);

        uint64_t started = timer_get_milliseconds();
        for (uint32_t iteration = 0;
             iteration < 100000 &&
                 timer_get_milliseconds() - started < 500;
             iteration++)
        {
            status = register_read32(port_register, 0);
            if (!(status & (XHCI_PORT_RESET | XHCI_PORT_WARM_RESET)))
                break;
            __asm__ volatile ("pause");
        }
        status = register_read32(port_register, 0);
        if (!(status & XHCI_PORT_ENABLED))
        {
            serial_log_begin(SERIAL_LOG_WARN);
            serial_write("xHCI port ");
            serial_write_dec(port);
            serial_write(" did not enable");
            serial_log_end();
            continue;
        }
        speed = (uint8_t)((status >> 10) & 0x0F);
        if (!enumerate_usb_device(controller, port, speed))
        {
            serial_log_begin(SERIAL_LOG_INFO);
            serial_write("No supported USB interface on xHCI port ");
            serial_write_dec(port);
            serial_log_end();
        }
    }
}

static void start_interrupt_transfer(
    xhci_controller_t *controller,
    xhci_device_t *device
)
{
    uint16_t index = ring_reserve(&device->interrupt_ring);
    xhci_trb_t *trb = &device->interrupt_ring.trbs[index];
    device->pending_trb_physical =
        trb_physical(&device->interrupt_ring, index);
    trb->parameter =
        (uint64_t)(uintptr_t)device->report_buffer -
        paging_get_hhdm_offset();
    trb->status = device->max_packet;
    trb->control = (TRB_TYPE_NORMAL << 10) | (1U << 5) |
        device->interrupt_ring.cycle;
    __asm__ volatile ("mfence" ::: "memory");
    controller->doorbells[device->slot_id] = device->endpoint_id;
    device->transfer_pending = true;
}

static void poll_controller(xhci_controller_t *controller)
{
    if (!controller->running)
        return;
    for (uint32_t count = 0; count < XHCI_RING_TRBS; count++)
    {
        xhci_trb_t event;
        if (!event_next(controller, &event))
            break;
        uint8_t type = (uint8_t)((event.control >> 10) & 0x3F);
        if (type != TRB_TYPE_TRANSFER_EVENT)
            continue;
        uint8_t slot = (uint8_t)(event.control >> 24);
        if (!slot || slot >= XHCI_DEVICE_LIMIT)
            continue;
        xhci_device_t *device = &controller->devices[slot];
        if (!device->used || !device->transfer_pending ||
            (event.parameter & ~0xFULL) !=
                (device->pending_trb_physical & ~0xFULL))
            continue;
        device->transfer_pending = false;
        if ((uint8_t)(event.status >> 24) != 1)
            continue;
        if (device->keyboard)
            (void)hid_handle_boot_keyboard_report(
                device->report_buffer);
        else if (device->mouse)
            (void)hid_handle_boot_mouse_report(
                device->report_buffer);
    }

    for (uint32_t i = 1; i < XHCI_DEVICE_LIMIT; i++)
    {
        xhci_device_t *device = &controller->devices[i];
        if (device->used &&
            (device->keyboard || device->mouse) &&
            !device->transfer_pending)
            start_interrupt_transfer(controller, device);
    }
}

bool xhci_matches_device(const pci_device_t *device)
{
    return device &&
        device->class_code == 0x0C &&
        device->subclass == 0x03 &&
        device->programming_interface == 0x30;
}

static bool controller_register_base(
    const pci_device_t *device,
    uint64_t *base
)
{
    if (!device || !base || !device->bar_count)
        return false;
    uint32_t bar = device->bars[0];
    if (!bar || (bar & 1))
        return false;
    uint64_t address = bar & ~0xFULL;
    if (((bar >> 1) & 3) == 2)
    {
        if (device->bar_count < 2)
            return false;
        address |= (uint64_t)device->bars[1] << 32;
    }
    if (!address)
        return false;
    *base = address;
    return true;
}

static bool initialize_controller(
    const pci_device_t *device,
    xhci_controller_t *controller
)
{
    uint64_t base;
    if (!pci_enable_memory(device) ||
        !controller_register_base(device, &base))
        return false;

    volatile uint8_t *registers = ioremap(base, XHCI_MMIO_SIZE);
    if (!registers)
        return false;
    uint8_t capability_length = registers[0];
    uint16_t version = (uint16_t)registers[2] |
        ((uint16_t)registers[3] << 8);
    uint32_t structural = register_read32(registers, 0x04);
    uint32_t capabilities = register_read32(registers, 0x10);
    uint8_t max_slots = (uint8_t)structural;
    uint8_t max_interrupters = (uint8_t)(structural >> 8);
    uint8_t max_ports = (uint8_t)(structural >> 24);
    if (capability_length < 0x20 || !max_slots || !max_ports)
        return false;

    controller->info = (xhci_controller_info_t){
        .pci_device = device,
        .register_base = base,
        .version = version,
        .capability_length = capability_length,
        .max_slots = max_slots,
        .max_interrupters = max_interrupters,
        .max_ports = max_ports
    };
    controller->registers = registers;
    controller->operational = registers + capability_length;
    controller->context_size = (capabilities & (1U << 2)) ? 64 : 32;
    controller->max_slots = max_slots;
    controller->event_cycle = 1;

    if (!acquire_firmware_ownership(controller) ||
        !controller_start(controller))
        return false;

    serial_log_begin(SERIAL_LOG_INFO);
    serial_write("xHCI controller running: ");
    serial_write_dec(max_ports);
    serial_write(" ports, ");
    serial_write_dec(max_slots);
    serial_write(" slots");
    serial_log_end();
    scan_ports(controller);
    return true;
}

bool xhci_probe(void)
{
    if (!pci_is_initialized())
        return false;

    controller_count = 0;
    for (uint32_t i = 0; i < pci_device_count(); i++)
    {
        const pci_device_t *device = pci_device_get(i);
        if (!xhci_matches_device(device))
            continue;
        if (controller_count == XHCI_CONTROLLER_LIMIT)
            break;

        xhci_controller_t *controller =
            &controllers[controller_count];
        *controller = (xhci_controller_t){0};
        if (!initialize_controller(device, controller))
        {
            serial_log(
                SERIAL_LOG_WARN,
                "xHCI controller initialization failed");
            continue;
        }
        controller_count++;
    }
    if (!controller_count)
        serial_log(SERIAL_LOG_INFO, "No usable xHCI controller found");
    return true;
}

void xhci_poll(void)
{
    for (uint32_t i = 0; i < controller_count; i++)
        poll_controller(&controllers[i]);
}

uint32_t xhci_controller_count(void)
{
    return controller_count;
}

const xhci_controller_info_t *xhci_controller_get(uint32_t index)
{
    return index < controller_count ? &controllers[index].info : NULL;
}
