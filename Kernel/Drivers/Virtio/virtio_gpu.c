#include <stdbool.h>
#include <stdint.h>

#include "paging.h"
#include "pmm.h"
#include "serial.h"
#include "vesa.h"
#include "virtio_gpu.h"
#include "virtio_legacy.h"
#include "virtio_pci.h"

#define VIRTIO_GPU_STATUS_ACKNOWLEDGE 0x01
#define VIRTIO_GPU_STATUS_DRIVER 0x02
#define VIRTIO_GPU_STATUS_DRIVER_OK 0x04
#define VIRTIO_GPU_STATUS_FAILED 0x80
#define VIRTIO_GPU_RESOURCE_ID 1
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM 2
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO 0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D 0x0101
#define VIRTIO_GPU_CMD_SET_SCANOUT 0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH 0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D 0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO 0x1101
#define VIRTIO_GPU_RESP_OK_NODATA 0x1100
#define VIRTIO_GPU_MAX_FRAMEBUFFER_BYTES (32ULL * 1024 * 1024)
#define VIRTIO_GPU_COMMAND_BUFFER_SIZE 4096
#define VIRTIO_GPU_TIMEOUT 10000000U

typedef struct __attribute__((packed))
{
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t context_id;
    uint32_t padding;
} virtio_gpu_header_t;

typedef struct __attribute__((packed))
{
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} virtio_gpu_rect_t;

typedef struct
{
    uint16_t io_base;
    bool modern;
    const virtio_pci_device_t *transport;
    virtio_legacy_queue_t control_queue;
    uint64_t request_physical;
    uint8_t *request_memory;
    uint64_t response_physical;
    uint8_t *response_memory;
    uint64_t framebuffer_physical;
    uint32_t *framebuffer_memory;
    uint64_t framebuffer_pages;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint16_t available_cursor;
    bool initialized;
    bool presenting;
} virtio_gpu_device_t;

static virtio_gpu_device_t device;

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

static uint32_t *request_words(void)
{
    return (uint32_t *)device.request_memory;
}

static void set_u64_words(uint32_t *words, uint32_t index, uint64_t value)
{
    words[index] = (uint32_t)value;
    words[index + 1] = (uint32_t)(value >> 32);
}

static bool gpu_command(uint32_t request_size)
{
    if (request_size > VIRTIO_GPU_COMMAND_BUFFER_SIZE - 256 ||
        !device.control_queue.size)
        return false;
    virtio_legacy_memory_zero(
        device.response_memory, VIRTIO_GPU_COMMAND_BUFFER_SIZE);
    virtio_descriptor_t *descriptors =
        device.control_queue.descriptors;
    descriptors[0] = (virtio_descriptor_t){
        .address = device.request_physical,
        .length = request_size,
        .flags = VIRTIO_DESC_F_NEXT,
        .next = 1
    };
    descriptors[1] = (virtio_descriptor_t){
        .address = device.response_physical,
        .length = VIRTIO_GPU_COMMAND_BUFFER_SIZE,
        .flags = VIRTIO_DESC_F_WRITE
    };
    uint16_t old_used = *device.control_queue.used_index;
    uint16_t available = device.available_cursor++;
    device.control_queue.available_ring[
        available % device.control_queue.size] = 0;
    __asm__ volatile ("mfence" ::: "memory");
    *device.control_queue.available_index = available + 1;
    if (device.modern)
        virtio_pci_modern_notify(device.transport, 0);
    else
        virtio_legacy_notify(device.io_base, 0);
    for (uint32_t iteration = 0; iteration < VIRTIO_GPU_TIMEOUT; iteration++)
    {
        __asm__ volatile ("mfence" ::: "memory");
        if (*device.control_queue.used_index != old_used)
        {
            virtio_used_element_t element =
                device.control_queue.used_ring[
                    old_used % device.control_queue.size];
            return element.id == 0 &&
                element.length >= sizeof(virtio_gpu_header_t);
        }
        __asm__ volatile ("pause");
    }
    return false;
}

static bool gpu_command_ok(uint32_t request_size)
{
    if (!gpu_command(request_size))
        return false;
    const virtio_gpu_header_t *response =
        (const virtio_gpu_header_t *)device.response_memory;
    return response->type == VIRTIO_GPU_RESP_OK_NODATA;
}

static bool gpu_modeset(void)
{
    uint32_t *words = request_words();
    virtio_legacy_memory_zero(device.request_memory, 256);
    ((virtio_gpu_header_t *)words)->type =
        VIRTIO_GPU_CMD_GET_DISPLAY_INFO;
    if (!gpu_command(sizeof(virtio_gpu_header_t)))
        return false;
    const uint32_t *response = (const uint32_t *)device.response_memory;
    if (response[0] != VIRTIO_GPU_RESP_OK_DISPLAY_INFO)
        return false;

    uint32_t scanout_width = response[8];
    uint32_t scanout_height = response[9];
    if (!scanout_width || !scanout_height ||
        scanout_width < device.width || scanout_height < device.height)
        return false;

    virtio_legacy_memory_zero(device.request_memory, 256);
    words[0] = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    words[6] = VIRTIO_GPU_RESOURCE_ID;
    words[7] = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
    words[8] = device.width;
    words[9] = device.height;
    if (!gpu_command_ok(40))
        return false;

    virtio_legacy_memory_zero(device.request_memory, 256);
    words[0] = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    words[6] = VIRTIO_GPU_RESOURCE_ID;
    words[7] = 1;
    set_u64_words(words, 8, device.framebuffer_physical);
    words[10] = device.width * device.height * sizeof(uint32_t);
    if (!gpu_command_ok(48))
        return false;

    virtio_legacy_memory_zero(device.request_memory, 256);
    words[0] = VIRTIO_GPU_CMD_SET_SCANOUT;
    words[6] = 0;
    words[7] = 0;
    words[8] = device.width;
    words[9] = device.height;
    words[10] = 0;
    words[11] = VIRTIO_GPU_RESOURCE_ID;
    return gpu_command_ok(48);
}

bool virtio_gpu_driver_init(void)
{
    device = (virtio_gpu_device_t){0};
    if (!vesa_get_width() || !vesa_get_height() ||
        vesa_get_width() > UINT32_MAX ||
        vesa_get_height() > UINT32_MAX)
        return false;
    const virtio_pci_device_t *transport =
        virtio_pci_find_device(16, 0);
    if (!transport ||
        !virtio_pci_enable_device(transport->pci_device))
        return false;

    device.transport = transport;
    device.modern = transport->modern;
    device.io_base = transport->io_base;
    device.width = (uint32_t)vesa_get_width();
    device.height = (uint32_t)vesa_get_height();
    device.pitch = device.width * sizeof(uint32_t);
    uint64_t framebuffer_bytes =
        (uint64_t)device.pitch * device.height;
    if (!framebuffer_bytes ||
        framebuffer_bytes > VIRTIO_GPU_MAX_FRAMEBUFFER_BYTES)
        return false;
    device.framebuffer_pages =
        (framebuffer_bytes + VIRTIO_LEGACY_PAGE_SIZE - 1) /
        VIRTIO_LEGACY_PAGE_SIZE;
    device.framebuffer_physical =
        pmm_alloc_contiguous_pages(device.framebuffer_pages);
    if (!device.framebuffer_physical)
        return false;
    device.framebuffer_memory =
        paging_phys_to_virt(device.framebuffer_physical);
    if (!device.framebuffer_memory)
        return false;

    if (device.modern)
    {
        if (!virtio_pci_modern_init(
                transport, 0, 1, NULL, NULL))
            goto failed;
    }
    else
    {
        outb(device.io_base + 0x12, 0);
        outb(device.io_base + 0x12, VIRTIO_GPU_STATUS_ACKNOWLEDGE);
        outb(device.io_base + 0x12,
            VIRTIO_GPU_STATUS_ACKNOWLEDGE |
            VIRTIO_GPU_STATUS_DRIVER);
        outl(device.io_base + 0x04, 0);
    }
    if (!(device.modern
            ? virtio_pci_modern_queue_init(
                transport, 0, &device.control_queue)
            : virtio_legacy_queue_init(
                device.io_base, 0, &device.control_queue)) ||
        device.control_queue.size < 2 ||
        !virtio_legacy_alloc_page(
            &device.request_physical, &device.request_memory) ||
        !virtio_legacy_alloc_page(
            &device.response_physical, &device.response_memory))
        goto failed;

    if (device.modern)
    {
        if (!virtio_pci_modern_driver_ok(transport))
            goto failed;
    }
    else
    {
        outb(device.io_base + 0x12,
            VIRTIO_GPU_STATUS_ACKNOWLEDGE |
            VIRTIO_GPU_STATUS_DRIVER |
            VIRTIO_GPU_STATUS_DRIVER_OK);
        if (!(inb(device.io_base + 0x12) &
                VIRTIO_GPU_STATUS_DRIVER_OK))
            goto failed;
    }
    if (!gpu_modeset())
        goto failed;

    device.initialized = true;
    if (!virtio_gpu_driver_present())
        goto failed;
    serial_log(SERIAL_LOG_INFO,
        "Initialized VirtIO GPU scanout and framebuffer");
    return true;

failed:
    if (!device.modern && device.io_base)
        outb(device.io_base + 0x12, VIRTIO_GPU_STATUS_FAILED);
    serial_log(SERIAL_LOG_WARN, "VirtIO GPU initialization failed");
    return false;
}

bool virtio_gpu_driver_is_initialized(void)
{
    return device.initialized;
}

bool virtio_gpu_driver_present(void)
{
    if (!device.initialized || device.presenting)
        return false;
    device.presenting = true;
    bool success = vesa_read_rgb(
        0,
        0,
        device.width,
        device.height,
        device.framebuffer_memory,
        device.width);
    if (success)
    {
        uint32_t *words = request_words();
        virtio_legacy_memory_zero(device.request_memory, 256);
        words[0] = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
        words[6] = 0;
        words[7] = 0;
        words[8] = device.width;
        words[9] = device.height;
        words[10] = 0;
        words[11] = 0;
        words[12] = VIRTIO_GPU_RESOURCE_ID;
        words[13] = 0;
        success = gpu_command_ok(56);
    }
    if (success)
    {
        uint32_t *words = request_words();
        virtio_legacy_memory_zero(device.request_memory, 256);
        words[0] = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
        words[6] = 0;
        words[7] = 0;
        words[8] = device.width;
        words[9] = device.height;
        words[10] = VIRTIO_GPU_RESOURCE_ID;
        words[11] = 0;
        success = gpu_command_ok(48);
    }
    device.presenting = false;
    return success;
}
