#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "heap.h"
#include "e1000.h"
#include "drm.h"
#include "module.h"
#include "paging.h"
#include "pci.h"
#include "pmm.h"
#include "radeonhd.h"
#include "serial.h"
#include "syscall.h"
#include "task.h"
#include "vfs.h"
#include "vesa.h"
#include "vmm.h"
#include "virtio_gpu.h"
#include "virtio_legacy.h"
#include "virtio_net.h"
#include "virtio_pci.h"
#include "rtl810x.h"

#define ELF_CLASS_64 2
#define ELF_DATA_LSB 1
#define ELF_VERSION_CURRENT 1
#define ELF_TYPE_REL 1
#define ELF_MACHINE_X86_64 62
#define ELF_SECTION_NULL 0
#define ELF_SECTION_PROGBITS 1
#define ELF_SECTION_SYMTAB 2
#define ELF_SECTION_STRTAB 3
#define ELF_SECTION_RELA 4
#define ELF_SECTION_NOBITS 8
#define ELF_SECTION_REL 9
#define ELF_SECTION_DYNSYM 11
#define ELF_SECTION_INIT_ARRAY 14
#define ELF_SECTION_ALLOC 0x2
#define ELF_SECTION_WRITE 0x1
#define ELF_SECTION_EXECINSTR 0x4
#define ELF_SYMBOL_UNDEFINED 0
#define ELF_SYMBOL_ABSOLUTE 0xFFF1
#define ELF_SYMBOL_BIND_WEAK 2
#define ELF_RELOC_X86_64_64 1
#define ELF_RELOC_X86_64_PC32 2
#define ELF_RELOC_X86_64_PLT32 4
#define ELF_RELOC_X86_64_32 10
#define ELF_RELOC_X86_64_32S 11

#define MODULE_ABI_VERSION 1
#define MODULE_MAX_COUNT 16
#define MODULE_MAX_FILE_SIZE (16ULL * 1024 * 1024)
#define MODULE_MAX_SECTIONS 256
#define MODULE_MAX_SECTION_ALIGNMENT (1024ULL * 1024)
#define MODULE_REGION_BASE 0xFFFFFFFF90000000ULL
#define MODULE_REGION_LIMIT 0xFFFFFFFFA0000000ULL

typedef struct
{
    unsigned char ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t program_offset;
    uint64_t section_offset;
    uint32_t flags;
    uint16_t header_size;
    uint16_t program_entry_size;
    uint16_t program_count;
    uint16_t section_entry_size;
    uint16_t section_count;
    uint16_t string_section_index;
} elf64_header_t;

typedef struct
{
    uint32_t name;
    uint32_t type;
    uint64_t flags;
    uint64_t address;
    uint64_t offset;
    uint64_t size;
    uint32_t link;
    uint32_t info;
    uint64_t alignment;
    uint64_t entry_size;
} elf64_section_t;

typedef struct
{
    uint32_t name;
    uint8_t info;
    uint8_t other;
    uint16_t section_index;
    uint64_t value;
    uint64_t size;
} elf64_symbol_t;

typedef struct
{
    uint64_t offset;
    uint64_t info;
    int64_t addend;
} elf64_rela_t;

typedef struct
{
    const char *name;
    uint64_t address;
} module_export_t;

typedef struct
{
    char path[256];
    uint64_t base;
    uint64_t size;
    uint64_t page_count;
    uint64_t *physical_pages;
} loaded_module_t;

static const module_export_t module_exports[] = {
    { "kernel_module_register_ethernet",
      (uint64_t)(uintptr_t)&kernel_module_register_ethernet },
    { "kernel_module_register_realtek",
      (uint64_t)(uintptr_t)&kernel_module_register_realtek },
    { "kernel_module_register_virtio_net",
      (uint64_t)(uintptr_t)&kernel_module_register_virtio_net },
    { "kernel_module_register_virtio_gpu",
      (uint64_t)(uintptr_t)&kernel_module_register_virtio_gpu },
    { "ioremap", (uint64_t)(uintptr_t)&ioremap },
    { "kernel_module_register_radeonhd",
      (uint64_t)(uintptr_t)&kernel_module_register_radeonhd },
    { "drm_register_firmware_scanout",
      (uint64_t)(uintptr_t)&drm_register_firmware_scanout },
    { "pci_device_count", (uint64_t)(uintptr_t)&pci_device_count },
    { "pci_device_get", (uint64_t)(uintptr_t)&pci_device_get },
    { "pci_device_name", (uint64_t)(uintptr_t)&pci_device_name },
    { "pci_enable_device", (uint64_t)(uintptr_t)&pci_enable_device },
    { "pci_get_bar_info", (uint64_t)(uintptr_t)&pci_get_bar_info },
    { "pci_format_bdf", (uint64_t)(uintptr_t)&pci_format_bdf },
    { "pci_format_resources", (uint64_t)(uintptr_t)&pci_format_resources },
    { "pci_is_initialized", (uint64_t)(uintptr_t)&pci_is_initialized },
    { "paging_phys_to_virt", (uint64_t)(uintptr_t)&paging_phys_to_virt },
    { "pmm_alloc_contiguous_pages",
      (uint64_t)(uintptr_t)&pmm_alloc_contiguous_pages },
    { "serial_log", (uint64_t)(uintptr_t)&serial_log },
    { "vesa_get_height", (uint64_t)(uintptr_t)&vesa_get_height },
    { "vesa_get_width", (uint64_t)(uintptr_t)&vesa_get_width },
    { "vesa_read_rgb", (uint64_t)(uintptr_t)&vesa_read_rgb },
    { "virtio_legacy_alloc_page",
      (uint64_t)(uintptr_t)&virtio_legacy_alloc_page },
    { "virtio_legacy_memory_zero",
      (uint64_t)(uintptr_t)&virtio_legacy_memory_zero },
    { "virtio_legacy_notify", (uint64_t)(uintptr_t)&virtio_legacy_notify },
    { "virtio_legacy_queue_init",
      (uint64_t)(uintptr_t)&virtio_legacy_queue_init },
    { "virtio_pci_enable_device",
      (uint64_t)(uintptr_t)&virtio_pci_enable_device },
    { "virtio_pci_find_device",
      (uint64_t)(uintptr_t)&virtio_pci_find_device },
    { "virtio_pci_modern_driver_ok",
      (uint64_t)(uintptr_t)&virtio_pci_modern_driver_ok },
    { "virtio_pci_modern_init",
      (uint64_t)(uintptr_t)&virtio_pci_modern_init },
    { "virtio_pci_modern_notify",
      (uint64_t)(uintptr_t)&virtio_pci_modern_notify },
    { "virtio_pci_modern_queue_init",
      (uint64_t)(uintptr_t)&virtio_pci_modern_queue_init },
    { "pmm_alloc_page", (uint64_t)(uintptr_t)&pmm_alloc_page },
    { "pmm_free_page", (uint64_t)(uintptr_t)&pmm_free_page },
    { "serial_log_begin", (uint64_t)(uintptr_t)&serial_log_begin },
    { "serial_log_end", (uint64_t)(uintptr_t)&serial_log_end },
    { "serial_write", (uint64_t)(uintptr_t)&serial_write },
    { "serial_write_dec", (uint64_t)(uintptr_t)&serial_write_dec },
    { "serial_write_hex", (uint64_t)(uintptr_t)&serial_write_hex }
};

static loaded_module_t loaded_modules[MODULE_MAX_COUNT];
static uint32_t loaded_module_count;
static uint64_t next_module_address = MODULE_REGION_BASE;
static module_radeonhd_ops_t radeonhd_ops;
static module_ethernet_ops_t ethernet_ops;
static module_ethernet_ops_t realtek_ops;
static module_ethernet_ops_t virtio_net_ops;
static module_virtio_gpu_ops_t virtio_gpu_ops;

static bool range_valid(uint64_t offset, uint64_t size, uint64_t limit)
{
    return offset <= limit && size <= limit - offset;
}

static bool is_power_of_two(uint64_t value)
{
    return value && (value & (value - 1)) == 0;
}

static bool align_up(uint64_t value, uint64_t alignment, uint64_t *result)
{
    if (!is_power_of_two(alignment) ||
        value > UINT64_MAX - (alignment - 1))
        return false;
    *result = (value + alignment - 1) & ~(alignment - 1);
    return true;
}

static bool string_at(
    const uint8_t *strings,
    uint64_t size,
    uint32_t offset,
    const char **result
)
{
    if (offset >= size)
        return false;
    for (uint64_t i = offset; i < size; i++)
    {
        if (!strings[i])
        {
            *result = (const char *)&strings[offset];
            return true;
        }
    }
    return false;
}

static bool string_equals(const char *left, const char *right)
{
    while (*left && *right && *left == *right)
    {
        left++;
        right++;
    }
    return *left == *right;
}

bool kernel_module_register_radeonhd(const module_radeonhd_ops_t *ops)
{
    if (!ops || !ops->format_attribute || !ops->format_debug ||
        radeonhd_ops.format_attribute || radeonhd_ops.format_debug)
        return false;
    radeonhd_ops = *ops;
    return true;
}

bool kernel_module_register_ethernet(const module_ethernet_ops_t *ops)
{
    if (!ops || !ops->is_initialized || !ops->link_is_up ||
        !ops->mac_address || !ops->send || !ops->receive ||
        ethernet_ops.is_initialized)
        return false;
    ethernet_ops = *ops;
    return true;
}

bool kernel_module_register_realtek(const module_ethernet_ops_t *ops)
{
    if (!ops || !ops->is_initialized || !ops->link_is_up ||
        !ops->mac_address || !ops->send || !ops->receive ||
        realtek_ops.is_initialized)
        return false;
    realtek_ops = *ops;
    return true;
}

bool kernel_module_register_virtio_net(const module_ethernet_ops_t *ops)
{
    if (!ops || !ops->is_initialized || !ops->link_is_up ||
        !ops->mac_address || !ops->send || !ops->receive ||
        virtio_net_ops.is_initialized)
        return false;
    virtio_net_ops = *ops;
    return true;
}

bool kernel_module_register_virtio_gpu(const module_virtio_gpu_ops_t *ops)
{
    if (!ops || !ops->initialize || !ops->is_initialized || !ops->present ||
        virtio_gpu_ops.initialize)
        return false;
    virtio_gpu_ops = *ops;
    return true;
}

bool virtio_gpu_init(void)
{
    return virtio_gpu_ops.initialize &&
        (virtio_gpu_ops.is_initialized() || virtio_gpu_ops.initialize());
}

bool virtio_gpu_is_initialized(void)
{
    return virtio_gpu_ops.is_initialized &&
        virtio_gpu_ops.is_initialized();
}

bool virtio_gpu_present(void)
{
    return virtio_gpu_ops.present && virtio_gpu_ops.present();
}

bool virtio_net_is_initialized(void)
{
    return virtio_net_ops.is_initialized &&
        virtio_net_ops.is_initialized();
}

bool virtio_net_link_is_up(void)
{
    return virtio_net_ops.link_is_up && virtio_net_ops.link_is_up();
}

const uint8_t *virtio_net_mac_address(void)
{
    return virtio_net_ops.mac_address
        ? virtio_net_ops.mac_address()
        : NULL;
}

bool virtio_net_send(const void *packet, size_t length)
{
    return virtio_net_ops.send &&
        virtio_net_ops.send(packet, length);
}

size_t virtio_net_receive(void *packet, size_t capacity)
{
    return virtio_net_ops.receive
        ? virtio_net_ops.receive(packet, capacity)
        : 0;
}

bool e1000_is_initialized(void)
{
    return ethernet_ops.is_initialized &&
        ethernet_ops.is_initialized();
}

bool e1000_link_is_up(void)
{
    return ethernet_ops.link_is_up && ethernet_ops.link_is_up();
}

const uint8_t *e1000_mac_address(void)
{
    return ethernet_ops.mac_address
        ? ethernet_ops.mac_address()
        : NULL;
}

bool e1000_send(const void *packet, size_t length)
{
    return ethernet_ops.send && ethernet_ops.send(packet, length);
}

size_t e1000_receive(void *packet, size_t capacity)
{
    return ethernet_ops.receive
        ? ethernet_ops.receive(packet, capacity)
        : 0;
}

bool realtek_is_initialized(void)
{
    return realtek_ops.is_initialized &&
        realtek_ops.is_initialized();
}

bool realtek_link_is_up(void)
{
    return realtek_ops.link_is_up && realtek_ops.link_is_up();
}

const uint8_t *realtek_mac_address(void)
{
    return realtek_ops.mac_address
        ? realtek_ops.mac_address()
        : NULL;
}

bool realtek_send(const void *packet, size_t length)
{
    return realtek_ops.send && realtek_ops.send(packet, length);
}

size_t realtek_receive(void *packet, size_t capacity)
{
    return realtek_ops.receive
        ? realtek_ops.receive(packet, capacity)
        : 0;
}

size_t radeonhd_format_attribute(
    const pci_device_t *pci_device,
    const char *attribute,
    char *buffer,
    size_t capacity
)
{
    if (radeonhd_ops.format_attribute)
        return radeonhd_ops.format_attribute(
            pci_device, attribute, buffer, capacity);

    if (!pci_device || !attribute || !buffer || !capacity)
        return 0;
    const char *value = 0;
    if (string_equals(attribute, "driver"))
        value = "unbound\n";
    else if (string_equals(attribute, "generation"))
        value = "unknown\n";
    else if (string_equals(attribute, "status"))
        value = "unbound\n";
    else if (string_equals(attribute, "debug"))
        return radeonhd_format_debug(pci_device, buffer, capacity);
    else
        return 0;

    size_t length = 0;
    while (value[length] && length < capacity)
    {
        buffer[length] = value[length];
        length++;
    }
    return value[length] ? 0 : length;
}

size_t radeonhd_format_debug(
    const pci_device_t *pci_device,
    char *buffer,
    size_t capacity
)
{
    static const char fallback[] =
        "driver: unbound\n"
        "hardware_init: not attempted\n";
    if (radeonhd_ops.format_debug)
        return radeonhd_ops.format_debug(pci_device, buffer, capacity);
    if (!pci_device || !buffer || sizeof(fallback) - 1 > capacity)
        return 0;
    for (size_t i = 0; i < sizeof(fallback) - 1; i++)
        buffer[i] = fallback[i];
    return sizeof(fallback) - 1;
}

static uint64_t find_export(const char *name)
{
    for (size_t i = 0; i < sizeof(module_exports) / sizeof(module_exports[0]); i++)
        if (string_equals(name, module_exports[i].name))
            return module_exports[i].address;
    return 0;
}

static bool module_path_allowed(const char *path)
{
    static const char prefix[] = "/lib/modules/" KERNEL_VERSION "/";
    size_t i = 0;
    while (prefix[i] && path[i] == prefix[i])
        i++;
    if (prefix[i])
        return false;
    if (!path[i] || path[i] == '/' || path[i] == '.')
        return false;

    size_t length = 0;
    while (path[i + length])
    {
        if (path[i + length] == '/' || length >= 255)
            return false;
        length++;
    }
    return length > 3 &&
           path[i + length - 3] == '.' &&
           path[i + length - 2] == 'k' &&
           path[i + length - 1] == 'o';
}

static void release_module_pages(
    uint64_t base,
    uint64_t page_count,
    uint64_t *physical_pages
)
{
    if (!physical_pages)
        return;
    for (uint64_t i = 0; i < page_count; i++)
    {
        uint64_t address = base + i * VMM_PAGE_SIZE;
        if (physical_pages[i])
        {
            (void)vmm_unmap_page(address);
            pmm_free_page(physical_pages[i]);
        }
    }
    kfree(physical_pages);
    if (page_count &&
        base + page_count * VMM_PAGE_SIZE == next_module_address)
        next_module_address = base;
}

static bool allocate_module_pages(
    uint64_t size,
    uint64_t alignment,
    uint64_t *base,
    uint64_t **physical_pages,
    uint64_t *page_count
)
{
    uint64_t aligned_size;
    if (alignment < VMM_PAGE_SIZE)
        alignment = VMM_PAGE_SIZE;
    if (!is_power_of_two(alignment) ||
        !align_up(size, VMM_PAGE_SIZE, &aligned_size) ||
        !aligned_size || aligned_size > MODULE_REGION_LIMIT - MODULE_REGION_BASE)
        return false;

    uint64_t aligned_base;
    if (!align_up(next_module_address, alignment, &aligned_base) ||
        aligned_base > MODULE_REGION_LIMIT ||
        aligned_size > MODULE_REGION_LIMIT - aligned_base)
        return false;

    uint64_t count = aligned_size / VMM_PAGE_SIZE;
    uint64_t *pages = kcalloc(count, sizeof(*pages));
    if (!pages)
        return false;

    *base = aligned_base;
    *physical_pages = pages;
    *page_count = count;
    for (uint64_t i = 0; i < count; i++)
    {
        uint64_t physical = pmm_alloc_page();
        if (!physical)
        {
            release_module_pages(*base, i, pages);
            *physical_pages = NULL;
            *page_count = 0;
            return false;
        }

        pages[i] = physical;
        uint8_t *page = paging_phys_to_virt(physical);
        if (!page || !vmm_map_page(
                aligned_base + i * VMM_PAGE_SIZE,
                physical,
                VMM_WRITABLE | VMM_GLOBAL | VMM_NO_EXECUTE))
        {
            pmm_free_page(physical);
            pages[i] = 0;
            release_module_pages(*base, i, pages);
            *physical_pages = NULL;
            *page_count = 0;
            return false;
        }
        for (uint64_t byte = 0; byte < VMM_PAGE_SIZE; byte++)
            page[byte] = 0;
    }

    next_module_address = aligned_base + aligned_size;
    return true;
}

static bool resolve_symbol(
    const uint8_t *file_data,
    uint64_t file_size,
    const elf64_section_t *sections,
    uint32_t section_count,
    const uint64_t *section_offsets,
    uint64_t module_base,
    const elf64_section_t *symbol_table,
    uint64_t symbol_index,
    uint64_t *result
)
{
    if (symbol_table->entry_size != sizeof(elf64_symbol_t) ||
        symbol_index >= symbol_table->size / sizeof(elf64_symbol_t) ||
        !range_valid(
            symbol_table->offset,
            symbol_table->size,
            file_size) ||
        symbol_table->link >= section_count)
        return false;

    const elf64_section_t *strings_section =
        &sections[symbol_table->link];
    if (strings_section->type != ELF_SECTION_STRTAB ||
        !range_valid(strings_section->offset, strings_section->size, file_size))
        return false;

    const elf64_symbol_t *symbols =
        (const elf64_symbol_t *)&file_data[symbol_table->offset];
    const elf64_symbol_t *symbol = &symbols[symbol_index];
    if (symbol->section_index == ELF_SYMBOL_UNDEFINED)
    {
        const char *name;
        const uint8_t *strings = &file_data[strings_section->offset];
        if (!string_at(
                strings,
                strings_section->size,
                symbol->name,
                &name))
            return false;
        uint64_t address = find_export(name);
        if (!address && (symbol->info >> 4) != ELF_SYMBOL_BIND_WEAK)
            return false;
        *result = address;
        return true;
    }

    if (symbol->section_index == ELF_SYMBOL_ABSOLUTE)
    {
        *result = symbol->value;
        return true;
    }
    if (symbol->section_index >= section_count ||
        !(sections[symbol->section_index].flags & ELF_SECTION_ALLOC) ||
        symbol->value > sections[symbol->section_index].size)
        return false;

    if (section_offsets[symbol->section_index] == UINT64_MAX)
        return false;
    *result = module_base +
              section_offsets[symbol->section_index] +
              symbol->value;
    return true;
}

static bool apply_relocations(
    const uint8_t *file_data,
    uint64_t file_size,
    const elf64_section_t *sections,
    uint32_t section_count,
    const uint64_t *section_offsets,
    uint64_t module_base
)
{
    for (uint32_t i = 0; i < section_count; i++)
    {
        const elf64_section_t *relocations = &sections[i];
        if (relocations->type != ELF_SECTION_RELA)
        {
            if (relocations->type == ELF_SECTION_REL &&
                relocations->info < section_count &&
                (sections[relocations->info].flags & ELF_SECTION_ALLOC))
                return false;
            continue;
        }
        if (relocations->info >= section_count ||
            relocations->link >= section_count)
            return false;
        if (!(sections[relocations->info].flags & ELF_SECTION_ALLOC))
            continue;
        if (
            sections[relocations->info].type == ELF_SECTION_NOBITS ||
            sections[relocations->link].type != ELF_SECTION_SYMTAB ||
            relocations->entry_size != sizeof(elf64_rela_t) ||
            relocations->size % sizeof(elf64_rela_t) ||
            !range_valid(relocations->offset, relocations->size, file_size))
            return false;

        const elf64_section_t *target = &sections[relocations->info];
        const elf64_section_t *symbols = &sections[relocations->link];
        const elf64_rela_t *entries =
            (const elf64_rela_t *)&file_data[relocations->offset];
        uint64_t count = relocations->size / sizeof(*entries);
        for (uint64_t j = 0; j < count; j++)
        {
            uint32_t type = (uint32_t)entries[j].info;
            uint64_t symbol_index = entries[j].info >> 32;
            uint64_t width = type == ELF_RELOC_X86_64_64 ? 8 : 4;
            if (type != ELF_RELOC_X86_64_64 &&
                type != ELF_RELOC_X86_64_PC32 &&
                type != ELF_RELOC_X86_64_PLT32 &&
                type != ELF_RELOC_X86_64_32 &&
                type != ELF_RELOC_X86_64_32S)
                return false;
            if (!range_valid(entries[j].offset, width, target->size) ||
                section_offsets[relocations->info] == UINT64_MAX)
                return false;

            uint64_t symbol_address;
            if (!resolve_symbol(
                    file_data,
                    file_size,
                    sections,
                    section_count,
                    section_offsets,
                    module_base,
                    symbols,
                    symbol_index,
                    &symbol_address))
                return false;

            uint64_t place = module_base +
                section_offsets[relocations->info] +
                entries[j].offset;
            __int128 value;
            if (type == ELF_RELOC_X86_64_PC32 ||
                type == ELF_RELOC_X86_64_PLT32)
                value = (__int128)symbol_address +
                        entries[j].addend - place;
            else
                value = (__int128)symbol_address + entries[j].addend;

            uint8_t *destination = (uint8_t *)place;
            if (type == ELF_RELOC_X86_64_64)
            {
                if (value < 0 || value > UINT64_MAX)
                    return false;
                *(uint64_t *)destination = (uint64_t)value;
            }
            else if (type == ELF_RELOC_X86_64_32)
            {
                if (value < 0 || value > UINT32_MAX)
                    return false;
                *(uint32_t *)destination = (uint32_t)value;
            }
            else if (type == ELF_RELOC_X86_64_32S)
            {
                if (value < 0 || value > UINT64_MAX)
                    return false;
                uint64_t full_value = (uint64_t)value;
                int32_t truncated_value = (int32_t)(uint32_t)full_value;
                if ((uint64_t)(int64_t)truncated_value != full_value)
                    return false;
                *(int32_t *)destination = truncated_value;
            }
            else
            {
                if (value < INT32_MIN || value > INT32_MAX)
                    return false;
                *(int32_t *)destination = (int32_t)value;
            }
        }
    }
    return true;
}

static bool protect_module_sections(
    const elf64_section_t *sections,
    uint32_t section_count,
    const uint64_t *section_offsets,
    uint64_t base,
    uint64_t page_count
)
{
    vmm_address_space_t address_space = vmm_current_address_space();
    for (uint64_t i = 0; i < page_count; i++)
        if (!vmm_protect_page_in(
                address_space,
                base + i * VMM_PAGE_SIZE,
                VMM_NO_EXECUTE))
            return false;

    for (uint32_t i = 0; i < section_count; i++)
    {
        const elf64_section_t *section = &sections[i];
        if (!(section->flags & ELF_SECTION_ALLOC) || !section->size)
            continue;

        uint64_t flags = 0;
        if (section->flags & ELF_SECTION_WRITE)
            flags |= VMM_WRITABLE;
        if (!(section->flags & ELF_SECTION_EXECINSTR))
            flags |= VMM_NO_EXECUTE;

        uint64_t first_page = section_offsets[i] / VMM_PAGE_SIZE;
        uint64_t section_pages =
            (section->size + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE;
        if (first_page > page_count ||
            section_pages > page_count - first_page)
            return false;
        for (uint64_t page = 0; page < section_pages; page++)
            if (!vmm_protect_page_in(
                    address_space,
                    base + (first_page + page) * VMM_PAGE_SIZE,
                    flags))
                return false;
    }
    return true;
}

static bool module_address_is_executable(
    const elf64_section_t *sections,
    uint32_t section_count,
    const uint64_t *section_offsets,
    uint64_t base,
    uint64_t address
)
{
    for (uint32_t i = 0; i < section_count; i++)
    {
        const elf64_section_t *section = &sections[i];
        if ((section->flags & (ELF_SECTION_ALLOC | ELF_SECTION_EXECINSTR)) !=
                (ELF_SECTION_ALLOC | ELF_SECTION_EXECINSTR) ||
            !section->size || section_offsets[i] == UINT64_MAX)
            continue;
        uint64_t section_base = base + section_offsets[i];
        if (address >= section_base &&
            address - section_base < section->size)
            return true;
    }
    return false;
}

static bool module_initializers_valid(
    const elf64_section_t *sections,
    uint32_t section_count,
    const uint64_t *section_offsets,
    uint64_t base
)
{
    for (uint32_t i = 0; i < section_count; i++)
    {
        const elf64_section_t *section = &sections[i];
        if (section->type != ELF_SECTION_INIT_ARRAY || !section->size)
            continue;
        if (!(section->flags & ELF_SECTION_ALLOC) ||
            section_offsets[i] == UINT64_MAX ||
            section->entry_size != sizeof(uint64_t) ||
            section->size % sizeof(uint64_t))
            return false;

        const uint64_t *initializers =
            (const uint64_t *)(uintptr_t)(base + section_offsets[i]);
        uint64_t count = section->size / sizeof(*initializers);
        for (uint64_t j = 0; j < count; j++)
        {
            uint64_t address = initializers[j];
            if (address && address != UINT64_MAX &&
                !module_address_is_executable(
                    sections,
                    section_count,
                    section_offsets,
                    base,
                    address))
                return false;
        }
    }
    return true;
}

static void run_module_initializers(
    const elf64_section_t *sections,
    uint32_t section_count,
    const uint64_t *section_offsets,
    uint64_t base
)
{
    for (uint32_t i = 0; i < section_count; i++)
    {
        const elf64_section_t *section = &sections[i];
        if (section->type != ELF_SECTION_INIT_ARRAY)
            continue;
        const uint64_t *initializers =
            (const uint64_t *)(uintptr_t)(base + section_offsets[i]);
        uint64_t count = section->size / sizeof(*initializers);
        for (uint64_t j = 0; j < count; j++)
        {
            uint64_t address = initializers[j];
            if (address && address != UINT64_MAX)
                ((void (*)(void))(uintptr_t)address)();
        }
    }
}

static bool load_elf_module(const char *path)
{
    vfs_file_t file;
    if (vfs_open(path, &file) < 0)
    {
        serial_log(SERIAL_LOG_ERROR, "Kernel module could not be opened");
        return false;
    }
    if (!file.size || file.size > MODULE_MAX_FILE_SIZE)
    {
        vfs_close_file(&file);
        serial_log(SERIAL_LOG_ERROR, "Kernel module has an invalid file size");
        return false;
    }

    uint64_t file_size = file.size;
    uint8_t *file_data = kmalloc(file_size);
    if (!file_data)
    {
        vfs_close_file(&file);
        serial_log(SERIAL_LOG_ERROR, "Kernel module file buffer allocation failed");
        return false;
    }
    uint64_t loaded = 0;
    while (loaded < file_size)
    {
        int64_t count = vfs_read(
            &file,
            file_data + loaded,
            file_size - loaded);
        if (count <= 0 || (uint64_t)count > file_size - loaded)
        {
            vfs_close_file(&file);
            kfree(file_data);
            serial_log(SERIAL_LOG_ERROR, "Kernel module read failed");
            return false;
        }
        loaded += (uint64_t)count;
    }
    vfs_close_file(&file);

    if (file_size < sizeof(elf64_header_t))
    {
        kfree(file_data);
        serial_log(SERIAL_LOG_ERROR, "Kernel module ELF header is truncated");
        return false;
    }

    const elf64_header_t *header = (const elf64_header_t *)file_data;
    if (header->ident[0] != 0x7F ||
        header->ident[1] != 'E' ||
        header->ident[2] != 'L' ||
        header->ident[3] != 'F' ||
        header->ident[4] != ELF_CLASS_64 ||
        header->ident[5] != ELF_DATA_LSB ||
        header->ident[6] != ELF_VERSION_CURRENT ||
        header->type != ELF_TYPE_REL ||
        header->machine != ELF_MACHINE_X86_64 ||
        header->version != ELF_VERSION_CURRENT ||
        header->header_size != sizeof(*header) ||
        header->section_entry_size != sizeof(elf64_section_t) ||
        !header->section_count ||
        header->section_count > MODULE_MAX_SECTIONS ||
        header->string_section_index >= header->section_count ||
        !range_valid(
            header->section_offset,
            (uint64_t)header->section_count * sizeof(elf64_section_t),
            file_size))
    {
        kfree(file_data);
        serial_log(SERIAL_LOG_ERROR, "Kernel module ELF metadata is invalid");
        return false;
    }

    const elf64_section_t *sections =
        (const elf64_section_t *)&file_data[header->section_offset];
    if (sections[header->string_section_index].type != ELF_SECTION_STRTAB ||
        !range_valid(
            sections[header->string_section_index].offset,
            sections[header->string_section_index].size,
            file_size))
    {
        kfree(file_data);
        serial_log(SERIAL_LOG_ERROR, "Kernel module section names are invalid");
        return false;
    }

    uint64_t *section_offsets =
        kmalloc((uint64_t)header->section_count * sizeof(*section_offsets));
    if (!section_offsets)
    {
        kfree(file_data);
        serial_log(SERIAL_LOG_ERROR, "Kernel module section map allocation failed");
        return false;
    }

    uint64_t image_size = 0;
    uint64_t image_alignment = VMM_PAGE_SIZE;
    bool layout_valid = true;
    for (uint32_t i = 0; i < header->section_count; i++)
    {
        section_offsets[i] = UINT64_MAX;
        const elf64_section_t *section = &sections[i];
        if (!(section->flags & ELF_SECTION_ALLOC) || !section->size)
            continue;
        uint64_t alignment = section->alignment ? section->alignment : 1;
        if (!is_power_of_two(alignment) ||
            alignment > MODULE_MAX_SECTION_ALIGNMENT ||
            !align_up(image_size,
                alignment > VMM_PAGE_SIZE ? alignment : VMM_PAGE_SIZE,
                &image_size))
        {
            layout_valid = false;
            break;
        }
        if (alignment > image_alignment)
            image_alignment = alignment;
        section_offsets[i] = image_size;
        if (section->size > UINT64_MAX - image_size)
        {
            layout_valid = false;
            break;
        }
        image_size += section->size;
        if (!align_up(image_size, VMM_PAGE_SIZE, &image_size))
        {
            layout_valid = false;
            break;
        }
        if (section->type != ELF_SECTION_NOBITS &&
            !range_valid(section->offset, section->size, file_size))
        {
            layout_valid = false;
            break;
        }
    }
    if (!layout_valid || !image_size)
    {
        kfree(section_offsets);
        kfree(file_data);
        serial_log(SERIAL_LOG_ERROR, "Kernel module section layout is invalid");
        return false;
    }

    uint64_t base = 0;
    uint64_t page_count = 0;
    uint64_t *physical_pages = NULL;
    if (!allocate_module_pages(
            image_size,
            image_alignment,
            &base,
            &physical_pages,
            &page_count))
    {
        kfree(section_offsets);
        kfree(file_data);
        serial_log(SERIAL_LOG_ERROR, "Kernel module memory allocation failed");
        return false;
    }

    for (uint32_t i = 0; i < header->section_count; i++)
    {
        const elf64_section_t *section = &sections[i];
        if (!(section->flags & ELF_SECTION_ALLOC) ||
            !section->size ||
            section->type == ELF_SECTION_NOBITS)
            continue;
        uint8_t *destination =
            (uint8_t *)(base + section_offsets[i]);
        const uint8_t *source = &file_data[section->offset];
        for (uint64_t byte = 0; byte < section->size; byte++)
            destination[byte] = source[byte];
    }

    bool relocated = apply_relocations(
        file_data,
        file_size,
        sections,
        header->section_count,
        section_offsets,
        base);
    if (!relocated)
    {
        release_module_pages(base, page_count, physical_pages);
        kfree(section_offsets);
        kfree(file_data);
        serial_log(SERIAL_LOG_ERROR, "Kernel module relocation failed");
        return false;
    }

    uint64_t abi_version_address = 0;
    uint64_t init_address = 0;
    bool has_abi = false;
    bool has_init = false;
    bool abi_symbol_valid = false;
    bool init_symbol_valid = false;
    for (uint32_t i = 0; i < header->section_count; i++)
    {
        const elf64_section_t *section = &sections[i];
        if (section->type != ELF_SECTION_SYMTAB)
            continue;
        if (section->entry_size != sizeof(elf64_symbol_t) ||
            section->size % sizeof(elf64_symbol_t) ||
            !range_valid(section->offset, section->size, file_size))
            continue;
        const elf64_symbol_t *symbols =
            (const elf64_symbol_t *)&file_data[section->offset];
        uint64_t count = section->size / sizeof(*symbols);
        if (section->link >= header->section_count ||
            sections[section->link].type != ELF_SECTION_STRTAB ||
            !range_valid(
                sections[section->link].offset,
                sections[section->link].size,
                file_size))
            continue;
        const uint8_t *strings =
            &file_data[sections[section->link].offset];
        for (uint64_t j = 0; j < count; j++)
        {
            const char *name;
            if (symbols[j].section_index == ELF_SYMBOL_UNDEFINED ||
                !string_at(
                    strings,
                    sections[section->link].size,
                    symbols[j].name,
                    &name))
                continue;
            uint64_t address;
            if (!resolve_symbol(
                    file_data,
                    file_size,
                    sections,
                    header->section_count,
                    section_offsets,
                    base,
                    section,
                    j,
                    &address))
                continue;
            if (string_equals(name, "vos_module_abi_version"))
            {
                abi_version_address = address;
                has_abi = true;
                abi_symbol_valid =
                    symbols[j].section_index < header->section_count &&
                    (sections[symbols[j].section_index].flags &
                        (ELF_SECTION_ALLOC | ELF_SECTION_EXECINSTR)) ==
                        ELF_SECTION_ALLOC &&
                    symbols[j].size >= sizeof(uint32_t);
            }
            else if (string_equals(name, "vos_module_init"))
            {
                init_address = address;
                has_init = true;
                init_symbol_valid =
                    symbols[j].section_index < header->section_count &&
                    (sections[symbols[j].section_index].flags &
                        (ELF_SECTION_ALLOC | ELF_SECTION_EXECINSTR)) ==
                        (ELF_SECTION_ALLOC | ELF_SECTION_EXECINSTR) &&
                    symbols[j].size != 0;
            }
        }
    }

    if (!has_abi || !has_init || !abi_symbol_valid || !init_symbol_valid ||
        abi_version_address < base ||
        abi_version_address > base + image_size - sizeof(uint32_t) ||
        *(const uint32_t *)(uintptr_t)abi_version_address !=
            MODULE_ABI_VERSION ||
        !protect_module_sections(
            sections,
            header->section_count,
            section_offsets,
            base,
            page_count))
    {
        release_module_pages(base, page_count, physical_pages);
        kfree(section_offsets);
        kfree(file_data);
        serial_log(SERIAL_LOG_ERROR, "Kernel module ABI or permissions invalid");
        return false;
    }

    if (!module_initializers_valid(
            sections,
            header->section_count,
            section_offsets,
            base))
    {
        release_module_pages(base, page_count, physical_pages);
        kfree(section_offsets);
        kfree(file_data);
        serial_log(SERIAL_LOG_ERROR,
                   "Kernel module initializer metadata is invalid");
        return false;
    }
    run_module_initializers(
        sections,
        header->section_count,
        section_offsets,
        base);

    bool (*module_init)(void) =
        (bool (*)(void))(uintptr_t)init_address;
    if (!module_init())
    {
        release_module_pages(base, page_count, physical_pages);
        kfree(section_offsets);
        kfree(file_data);
        serial_log(SERIAL_LOG_ERROR, "Kernel module initialization failed");
        return false;
    }

    loaded_module_t *module = &loaded_modules[loaded_module_count++];
    size_t path_length = 0;
    while (path[path_length] && path_length < sizeof(module->path) - 1)
    {
        module->path[path_length] = path[path_length];
        path_length++;
    }
    module->path[path_length] = '\0';
    module->base = base;
    module->size = image_size;
    module->page_count = page_count;
    module->physical_pages = physical_pages;

    kfree(section_offsets);
    kfree(file_data);
    serial_log(SERIAL_LOG_INFO, "Kernel module loaded and initialized");
    return true;
}

int64_t kernel_module_load(const char *path)
{
    if (task_current_pid() != 1)
        return -SYSCALL_EPERM;
    if (!path || !module_path_allowed(path))
        return -SYSCALL_EACCES;
    if (loaded_module_count >= MODULE_MAX_COUNT)
        return -SYSCALL_ENOSPC;
    for (uint32_t i = 0; i < loaded_module_count; i++)
        if (string_equals(path, loaded_modules[i].path))
            return -SYSCALL_EEXIST;

    return load_elf_module(path) ? 0 : -SYSCALL_ENOEXEC;
}
