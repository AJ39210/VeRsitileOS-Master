#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pci.h"
#include "serial.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA 0xCFC
static pci_device_t devices[PCI_DEVICE_LIMIT];
static uint32_t device_count;
static bool initialized;
static bool inventory_truncated;

typedef struct
{
    uint16_t id;
    const char *name;
} pci_vendor_entry_t;

typedef struct
{
    uint16_t vendor;
    uint16_t device;
    const char *name;
} pci_product_entry_t;

static const pci_vendor_entry_t vendors[] = {
    { 0x1002, "Advanced Micro Devices" },
    { 0x1013, "Cirrus Logic" },
    { 0x10DE, "NVIDIA Corporation" },
    { 0x10EC, "Realtek Semiconductor" },
    { 0x1234, "QEMU" },
    { 0x14E4, "Broadcom" },
    { 0x15AD, "VMware" },
    { 0x1AF4, "Red Hat / Virtio" },
    { 0x8086, "Intel Corporation" },
    { 0x80EE, "Oracle / VirtualBox" }
};

static const pci_product_entry_t products[] = {
    { 0x1234, 0x1111, "QEMU Standard VGA" },
    { 0x8086, 0x100E, "82540EM Gigabit Ethernet Controller" },
    { 0x8086, 0x24FD, "Wireless 8265 / 8275" },
    { 0x8086, 0x1237, "440FX Host Bridge" },
    { 0x8086, 0x7000, "82371SB ISA Bridge" },
    { 0x8086, 0x7010, "82371SB IDE Controller" },
    { 0x8086, 0x7113, "82371AB/EB ACPI Controller" },
    { 0x8086, 0x7111, "82371AB/EB IDE Controller" },
    { 0x10EC, 0x8139, "RTL-8139 Ethernet Controller" },
    { 0x1AF4, 0x1000, "Virtio Network Device" },
    { 0x1AF4, 0x1001, "Virtio Block Device" },
    { 0x1AF4, 0x1050, "Virtio GPU" },
    { 0x80EE, 0xBEEF, "VirtualBox Graphics Adapter" }
};

static inline void outl(uint16_t port, uint32_t value)
{
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t value;
    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static uint32_t config_read32(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint8_t offset
)
{
    uint32_t address = 0x80000000U |
        ((uint32_t)bus << 16) |
        ((uint32_t)device << 11) |
        ((uint32_t)function << 8) |
        (offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

static uint16_t config_read16(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint8_t offset
)
{
    uint32_t value = config_read32(bus, device, function, offset);
    return (uint16_t)(value >> ((offset & 2) * 8));
}

static void config_write16(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint8_t offset,
    uint16_t value
)
{
    uint32_t address = 0x80000000U |
        ((uint32_t)bus << 16) |
        ((uint32_t)device << 11) |
        ((uint32_t)function << 8) |
        (offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS, address);
    __asm__ volatile (
        "outw %0, %1"
        :
        : "a"(value), "Nd"((uint16_t)(PCI_CONFIG_DATA + (offset & 2)))
    );
}

static void config_write32(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint8_t offset,
    uint32_t value
)
{
    uint32_t address = 0x80000000U |
        ((uint32_t)bus << 16) |
        ((uint32_t)device << 11) |
        ((uint32_t)function << 8) |
        (offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, value);
}

static bool append_text(
    char *buffer,
    size_t capacity,
    size_t *position,
    const char *text
)
{
    while (*text)
    {
        if (*position >= capacity)
            return false;
        buffer[(*position)++] = *text++;
    }
    return true;
}

static bool append_hex(
    char *buffer,
    size_t capacity,
    size_t *position,
    uint32_t value,
    uint8_t digits
)
{
    static const char hex[] = "0123456789abcdef";
    if (*position > capacity || digits > capacity - *position)
        return false;
    for (uint8_t i = 0; i < digits; i++)
    {
        uint8_t shift = (uint8_t)((digits - i - 1) * 4);
        buffer[(*position)++] = hex[(value >> shift) & 0xF];
    }
    return true;
}

static bool string_equal(const char *left, const char *right)
{
    while (*left && *right && *left == *right)
    {
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

bool pci_format_bdf(
    const pci_device_t *device,
    char *buffer,
    size_t capacity
)
{
    if (!device || !buffer || capacity < PCI_BDF_NAME_LENGTH)
        return false;
    size_t position = 0;
    return append_hex(buffer, capacity, &position, 0, 4) &&
        append_text(buffer, capacity, &position, ":") &&
        append_hex(buffer, capacity, &position, device->bus, 2) &&
        append_text(buffer, capacity, &position, ":") &&
        append_hex(buffer, capacity, &position, device->device, 2) &&
        append_text(buffer, capacity, &position, ".") &&
        append_hex(buffer, capacity, &position, device->function, 1) &&
        position < capacity &&
        (buffer[position] = '\0', true);
}

bool pci_init(void)
{
    device_count = 0;
    inventory_truncated = false;
    initialized = false;

    for (uint32_t bus = 0; bus < 256; bus++)
    {
        for (uint8_t slot = 0; slot < 32; slot++)
        {
            uint16_t vendor0 = config_read16(
                (uint8_t)bus, slot, 0, 0x00);
            if (vendor0 == 0xFFFF)
                continue;

            uint8_t header0 = (uint8_t)(
                config_read32((uint8_t)bus, slot, 0, 0x0C) >> 16);
            uint8_t functions =
                (header0 & 0x80) ? 8 : 1;
            for (uint8_t function = 0; function < functions; function++)
            {
                uint32_t id = config_read32(
                    (uint8_t)bus, slot, function, 0x00);
                uint16_t vendor = (uint16_t)id;
                if (vendor == 0xFFFF)
                    continue;

                if (device_count == PCI_DEVICE_LIMIT)
                {
                    inventory_truncated = true;
                    continue;
                }

                uint32_t class_revision = config_read32(
                    (uint8_t)bus, slot, function, 0x08);
                uint32_t class_header = config_read32(
                    (uint8_t)bus, slot, function, 0x0C);
                uint32_t command_status = config_read32(
                    (uint8_t)bus, slot, function, 0x04);
                uint32_t interrupt = config_read32(
                    (uint8_t)bus, slot, function, 0x3C);
                pci_device_t discovered = {
                    .bus = (uint8_t)bus,
                    .device = slot,
                    .function = function,
                    .revision = (uint8_t)class_revision,
                    .programming_interface =
                        (uint8_t)(class_revision >> 8),
                    .subclass = (uint8_t)(class_revision >> 16),
                    .class_code = (uint8_t)(class_revision >> 24),
                    .header_type = (uint8_t)(class_header >> 16),
                    .vendor_id = vendor,
                    .device_id = (uint16_t)(id >> 16),
                    .command = (uint16_t)command_status,
                    .status = (uint16_t)(command_status >> 16),
                    .interrupt_line = (uint8_t)interrupt,
                    .interrupt_pin = (uint8_t)(interrupt >> 8)
                };
                uint8_t header_type = discovered.header_type & 0x7F;
                if (header_type == 0)
                {
                    discovered.bar_count = PCI_BAR_LIMIT;
                    uint32_t subsystem = config_read32(
                        (uint8_t)bus, slot, function, 0x2C);
                    discovered.subsystem_vendor_id = (uint16_t)subsystem;
                    discovered.subsystem_id = (uint16_t)(subsystem >> 16);
                }
                else if (header_type == 1)
                {
                    discovered.bar_count = 2;
                }
                for (uint8_t bar = 0; bar < discovered.bar_count; bar++)
                {
                    discovered.bars[bar] = config_read32(
                        (uint8_t)bus,
                        slot,
                        function,
                        (uint8_t)(0x10 + bar * sizeof(uint32_t)));
                }
                devices[device_count++] = discovered;
            }
        }
    }

    initialized = true;
    for (uint32_t i = 0; i < device_count; i++)
    {
        const pci_device_t *device = &devices[i];
        serial_log_begin(SERIAL_LOG_INFO);
        serial_write("PCI ");
        serial_write_hex(((uint64_t)device->bus << 16) |
            ((uint64_t)device->device << 8) | device->function);
        serial_write(": ");
        serial_write(pci_vendor_name(device->vendor_id));
        serial_write(" ");
        serial_write(pci_device_name(device));
        serial_write(" [");
        serial_write(pci_class_name(
            device->class_code, device->subclass));
        serial_write("]");
        serial_log_end();
    }
    if (inventory_truncated)
        serial_log(
            SERIAL_LOG_WARN,
            "PCI inventory reached its 256-device limit; additional devices omitted");
    serial_log_begin(SERIAL_LOG_INFO);
    serial_write_dec(device_count);
    serial_write(" PCI function(s) discovered");
    serial_log_end();
    return true;
}

bool pci_is_initialized(void)
{
    return initialized;
}

uint32_t pci_device_count(void)
{
    return initialized ? device_count : 0;
}

const pci_device_t *pci_device_get(uint32_t index)
{
    if (!initialized || index >= device_count)
        return NULL;
    return &devices[index];
}

bool pci_enable_memory(const pci_device_t *device)
{
    return pci_enable_device(device, true, true);
}

static int32_t pci_device_index(const pci_device_t *device)
{
    if (!device || !initialized)
        return -1;
    for (uint32_t i = 0; i < device_count; i++)
    {
        if (&devices[i] == device)
            return (int32_t)i;
    }
    return -1;
}

bool pci_enable_device(const pci_device_t *device, bool memory_space,
                       bool bus_master)
{
    const int32_t index = pci_device_index(device);
    if (index < 0 || (!memory_space && !bus_master))
        return false;

    uint16_t command = config_read16(
        device->bus,
        device->device,
        device->function,
        0x04);
    if (memory_space)
        command |= 1U << 1;
    if (bus_master)
        command |= 1U << 2;
    if (command != config_read16(device->bus, device->device,
                                 device->function, 0x04))
        config_write16(
            device->bus,
            device->device,
            device->function,
            0x04,
            command);

    const uint16_t enabled = config_read16(
        device->bus, device->device, device->function, 0x04);
    devices[index].command = enabled;
    return (!memory_space || (enabled & (1U << 1))) &&
        (!bus_master || (enabled & (1U << 2)));
}

bool pci_get_bar_info(const pci_device_t *device, uint8_t bar_index,
                      pci_bar_info_t *info)
{
    if (pci_device_index(device) < 0 || !info ||
        bar_index >= device->bar_count)
        return false;

    const uint8_t header_type = device->header_type & 0x7F;
    if (header_type != 0 && header_type != 1)
        return false;
    const uint8_t bar_count = header_type == 0 ? PCI_BAR_LIMIT : 2;
    if (bar_index > 0)
    {
        const uint8_t previous_offset = (uint8_t)(0x10 + (bar_index - 1) * 4);
        const uint32_t previous = config_read32(
            device->bus, device->device, device->function, previous_offset);
        if (!(previous & 1U) && ((previous >> 1) & 3U) == 2U)
            return false;
    }
    const uint8_t offset = (uint8_t)(0x10 + bar_index * 4);
    const uint32_t original_low = config_read32(
        device->bus, device->device, device->function, offset);
    const bool is_io = (original_low & 1U) != 0;
    const uint8_t memory_type = (uint8_t)((original_low >> 1) & 3U);
    const bool is_64bit = !is_io && memory_type == 2;
    if (!is_io && memory_type == 3)
        return false;
    if (is_64bit && bar_index + 1 >= bar_count)
        return false;

    const uint8_t command_offset = 0x04;
    const uint16_t original_command = config_read16(
        device->bus, device->device, device->function, command_offset);
    const uint32_t original_high = is_64bit
        ? config_read32(device->bus, device->device, device->function,
                        (uint8_t)(offset + 4))
        : 0;

    config_write16(device->bus, device->device, device->function,
                   command_offset, (uint16_t)(original_command & ~3U));
    config_write32(device->bus, device->device, device->function,
                   offset, UINT32_MAX);
    if (is_64bit)
        config_write32(device->bus, device->device, device->function,
                       (uint8_t)(offset + 4), UINT32_MAX);
    const uint32_t mask_low = config_read32(
        device->bus, device->device, device->function, offset);
    const uint32_t mask_high = is_64bit
        ? config_read32(device->bus, device->device, device->function,
                        (uint8_t)(offset + 4))
        : 0;

    config_write32(device->bus, device->device, device->function,
                   offset, original_low);
    if (is_64bit)
        config_write32(device->bus, device->device, device->function,
                       (uint8_t)(offset + 4), original_high);
    config_write16(device->bus, device->device, device->function,
                   command_offset, original_command);

    const uint64_t address_mask = is_io
        ? (uint64_t)(mask_low & ~3U)
        : is_64bit
            ? (((uint64_t)mask_high << 32) | (mask_low & ~0xFU))
            : (uint64_t)(mask_low & ~0xFU);
    const uint64_t base = is_io
        ? (uint64_t)(original_low & ~3U)
        : is_64bit
            ? (((uint64_t)original_high << 32) | (original_low & ~0xFU))
            : (uint64_t)(original_low & ~0xFU);
    const uint64_t size = is_64bit
        ? (~address_mask + 1U)
        : (uint64_t)(uint32_t)(~(uint32_t)address_mask + 1U);
    if (!address_mask || !base || !size)
        return false;

    *info = (pci_bar_info_t){
        .base = base,
        .size = size,
        .is_io = is_io,
        .is_64bit = is_64bit,
        .prefetchable = !is_io && (original_low & 8U)
    };
    return true;
}

uint32_t pci_class_device_count(uint8_t class_code, uint8_t subclass)
{
    uint32_t count = 0;
    if (!initialized)
        return 0;
    for (uint32_t i = 0; i < device_count; i++)
    {
        if (devices[i].class_code == class_code &&
            (subclass == 0xFF || devices[i].subclass == subclass))
            count++;
    }
    return count;
}

const pci_device_t *pci_class_device_get(
    uint8_t class_code,
    uint8_t subclass,
    uint32_t index
)
{
    if (!initialized)
        return NULL;
    for (uint32_t i = 0; i < device_count; i++)
    {
        if (devices[i].class_code != class_code ||
            (subclass != 0xFF && devices[i].subclass != subclass))
            continue;
        if (!index--)
            return &devices[i];
    }
    return NULL;
}

const pci_device_t *pci_find_bdf(const char *bdf)
{
    if (!initialized || !bdf)
        return NULL;
    char current[PCI_BDF_NAME_LENGTH];
    for (uint32_t i = 0; i < device_count; i++)
    {
        if (pci_format_bdf(&devices[i], current, sizeof(current)))
        {
            size_t j = 0;
            while (current[j] && bdf[j] && current[j] == bdf[j])
                j++;
            if (!current[j] && !bdf[j])
                return &devices[i];
        }
    }
    return NULL;
}

const char *pci_vendor_name(uint16_t vendor_id)
{
    for (size_t i = 0; i < sizeof(vendors) / sizeof(vendors[0]); i++)
    {
        if (vendors[i].id == vendor_id)
            return vendors[i].name;
    }
    return "Unknown PCI vendor";
}

const char *pci_device_name(const pci_device_t *device)
{
    if (!device)
        return "Unknown PCI device";
    for (size_t i = 0; i < sizeof(products) / sizeof(products[0]); i++)
    {
        if (products[i].vendor == device->vendor_id &&
            products[i].device == device->device_id)
            return products[i].name;
    }
    return pci_class_name(device->class_code, device->subclass);
}

const char *pci_class_name(uint8_t class_code, uint8_t subclass)
{
    if (class_code == 0x00)
        return "Unclassified device";
    if (class_code == 0x01)
    {
        if (subclass == 0x01) return "IDE controller";
        if (subclass == 0x06) return "SATA controller";
        if (subclass == 0x08) return "Non-volatile memory controller";
        return "Mass storage controller";
    }
    if (class_code == 0x02) return "Network controller";
    if (class_code == 0x03)
    {
        if (subclass == 0x00) return "VGA-compatible controller";
        if (subclass == 0x02) return "3D controller";
        return "Display controller";
    }
    if (class_code == 0x04)
    {
        if (subclass == 0x01) return "Audio device";
        if (subclass == 0x03) return "Audio device";
        return "Multimedia controller";
    }
    if (class_code == 0x06)
    {
        if (subclass == 0x00) return "Host bridge";
        if (subclass == 0x01) return "ISA bridge";
        if (subclass == 0x04) return "PCI bridge";
        return "Bridge device";
    }
    if (class_code == 0x0C && subclass == 0x03)
        return "USB controller";
    if (class_code == 0x0C && subclass == 0x05)
        return "SMBus controller";
    return "PCI device";
}

static size_t format_hex_value(
    char *buffer,
    size_t capacity,
    const char *label,
    uint32_t value,
    uint8_t digits
)
{
    size_t position = 0;
    if (!append_text(buffer, capacity, &position, label) ||
        !append_hex(buffer, capacity, &position, value, digits) ||
        !append_text(buffer, capacity, &position, "\n"))
        return 0;
    return position;
}

static size_t format_text_value(
    char *buffer,
    size_t capacity,
    const char *label,
    const char *value
)
{
    size_t position = 0;
    if (!append_text(buffer, capacity, &position, label) ||
        !append_text(buffer, capacity, &position, value) ||
        !append_text(buffer, capacity, &position, "\n"))
        return 0;
    return position;
}

static bool append_hex64(
    char *buffer,
    size_t capacity,
    size_t *position,
    uint64_t value,
    uint8_t digits
)
{
    static const char hex[] = "0123456789abcdef";
    if (*position > capacity || digits > capacity - *position)
        return false;
    for (uint8_t i = 0; i < digits; i++)
    {
        uint8_t shift = (uint8_t)((digits - i - 1) * 4);
        buffer[(*position)++] = hex[(value >> shift) & 0xF];
    }
    return true;
}

size_t pci_format_resources(
    const pci_device_t *device,
    char *buffer,
    size_t capacity
)
{
    if (!device || !buffer || !capacity)
        return 0;

    size_t position = 0;
    bool found_resource = false;
    for (uint8_t i = 0; i < device->bar_count; i++)
    {
        uint8_t bar_index = i;
        uint32_t bar = device->bars[i];
        if (!bar)
            continue;
        found_resource = true;

        if (bar & 1)
        {
            if (!append_text(buffer, capacity, &position, "BAR") ||
                !append_hex(buffer, capacity, &position, bar_index, 1) ||
                !append_text(buffer, capacity, &position, ": io 0x") ||
                !append_hex(
                    buffer, capacity, &position, bar & ~3U, 4) ||
                !append_text(buffer, capacity, &position, "\n"))
                return 0;
            continue;
        }

        uint8_t type = (uint8_t)((bar >> 1) & 3);
        uint64_t address = bar & ~0xFU;
        uint8_t digits = 8;
        if (type == 2 && i + 1 < device->bar_count)
        {
            address |= (uint64_t)device->bars[i + 1] << 32;
            digits = 16;
            i++;
        }
        if (!append_text(buffer, capacity, &position, "BAR") ||
            !append_hex(buffer, capacity, &position, bar_index, 1) ||
            !append_text(buffer, capacity, &position, ": memory 0x") ||
            !append_hex64(buffer, capacity, &position, address, digits) ||
            !append_text(buffer, capacity, &position,
                (bar & 8) ? " prefetchable\n" : "\n"))
            return 0;
    }

    if (!found_resource &&
        !append_text(buffer, capacity, &position, "No BARs\n"))
        return 0;
    return position;
}

size_t pci_format_attribute(
    const pci_device_t *device,
    const char *attribute,
    char *buffer,
    size_t capacity
)
{
    if (!device || !attribute || !buffer || !capacity)
        return 0;
    if (string_equal(attribute, "vendor"))
        return format_hex_value(buffer, capacity, "0x", device->vendor_id, 4);
    if (string_equal(attribute, "device"))
        return format_hex_value(buffer, capacity, "0x", device->device_id, 4);
    if (string_equal(attribute, "class"))
    {
        uint32_t class_value =
            ((uint32_t)device->class_code << 16) |
            ((uint32_t)device->subclass << 8) |
            device->programming_interface;
        return format_hex_value(buffer, capacity, "0x", class_value, 6);
    }
    if (string_equal(attribute, "subsystem_vendor"))
        return format_hex_value(
            buffer, capacity, "0x", device->subsystem_vendor_id, 4);
    if (string_equal(attribute, "subsystem_device"))
        return format_hex_value(
            buffer, capacity, "0x", device->subsystem_id, 4);
    if (string_equal(attribute, "manufacturer"))
        return format_text_value(
            buffer, capacity, "", pci_vendor_name(device->vendor_id));
    if (string_equal(attribute, "device_name"))
        return format_text_value(
            buffer, capacity, "", pci_device_name(device));
    if (string_equal(attribute, "class_name"))
        return format_text_value(
            buffer, capacity, "", pci_class_name(
                device->class_code, device->subclass));
    if (string_equal(attribute, "revision"))
        return format_hex_value(buffer, capacity, "0x", device->revision, 2);
    if (string_equal(attribute, "interrupt_line"))
        return format_hex_value(
            buffer, capacity, "0x", device->interrupt_line, 2);
    if (string_equal(attribute, "interrupt_pin"))
        return format_hex_value(
            buffer, capacity, "0x", device->interrupt_pin, 2);
    if (string_equal(attribute, "command"))
        return format_hex_value(buffer, capacity, "0x", device->command, 4);
    if (string_equal(attribute, "status"))
        return format_hex_value(buffer, capacity, "0x", device->status, 4);
    if (string_equal(attribute, "resource"))
        return pci_format_resources(device, buffer, capacity);
    return 0;
}

size_t pci_format_device(
    const pci_device_t *device,
    char *buffer,
    size_t capacity
)
{
    if (!device || !buffer || !capacity)
        return 0;
    size_t position = 0;
    char bdf[PCI_BDF_NAME_LENGTH];
    if (!pci_format_bdf(device, bdf, sizeof(bdf)) ||
        !append_text(buffer, capacity, &position, "PCI address: ") ||
        !append_text(buffer, capacity, &position, bdf) ||
        !append_text(buffer, capacity, &position, "\nVendor: ") ||
        !append_text(buffer, capacity, &position,
            pci_vendor_name(device->vendor_id)) ||
        !append_text(buffer, capacity, &position, " (") ||
        !append_hex(buffer, capacity, &position, device->vendor_id, 4) ||
        !append_text(buffer, capacity, &position, ")\nDevice: ") ||
        !append_text(buffer, capacity, &position,
            pci_device_name(device)) ||
        !append_text(buffer, capacity, &position, " (") ||
        !append_hex(buffer, capacity, &position, device->device_id, 4) ||
        !append_text(buffer, capacity, &position, ")\nClass: ") ||
        !append_text(buffer, capacity, &position,
            pci_class_name(device->class_code, device->subclass)) ||
        !append_text(buffer, capacity, &position, " (") ||
        !append_hex(buffer, capacity, &position, device->class_code, 2) ||
        !append_hex(buffer, capacity, &position, device->subclass, 2) ||
        !append_text(buffer, capacity, &position, ")\nRevision: ") ||
        !append_hex(buffer, capacity, &position, device->revision, 2) ||
        !append_text(buffer, capacity, &position, "\n"))
        return 0;
    if ((device->header_type & 0x7F) == 0 &&
        (!append_text(buffer, capacity, &position, "Subsystem: ") ||
         !append_hex(
             buffer, capacity, &position, device->subsystem_vendor_id, 4) ||
         !append_text(buffer, capacity, &position, ":") ||
         !append_hex(buffer, capacity, &position, device->subsystem_id, 4) ||
         !append_text(buffer, capacity, &position, "\n")))
        return 0;
    if (!append_text(buffer, capacity, &position, "Interrupt: ") ||
        !append_hex(buffer, capacity, &position, device->interrupt_line, 2) ||
        !append_text(buffer, capacity, &position, " (pin ") ||
        !append_hex(buffer, capacity, &position, device->interrupt_pin, 2) ||
        !append_text(buffer, capacity, &position, ")\nCommand/Status: ") ||
        !append_hex(buffer, capacity, &position, device->command, 4) ||
        !append_text(buffer, capacity, &position, "/") ||
        !append_hex(buffer, capacity, &position, device->status, 4) ||
        !append_text(buffer, capacity, &position, "\n"))
        return 0;
    size_t resource_length = pci_format_resources(
        device, buffer + position, capacity - position);
    if (!resource_length)
        return 0;
    position += resource_length;
    return position;
}
