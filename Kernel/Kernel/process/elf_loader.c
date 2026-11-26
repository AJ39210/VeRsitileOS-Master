#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "elf_loader.h"
#include "heap.h"
#include "paging.h"
#include "pmm.h"
#include "serial.h"
#include "syscall.h"
#include "vfs.h"

#define ELF_CLASS_64 2
#define ELF_DATA_LSB 1
#define ELF_VERSION_CURRENT 1
#define ELF_TYPE_EXEC 2
#define ELF_TYPE_DYN 3
#define ELF_MACHINE_X86_64 62
#define ELF_PT_LOAD 1
#define ELF_PT_DYNAMIC 2
#define ELF_PT_INTERP 3
#define ELF_PF_X 1
#define ELF_PF_W 2
#define ELF_MAX_PROGRAM_HEADERS 128
#define ELF_MAX_FILE_SIZE (16ULL * 1024 * 1024)
#define ELF_MAX_MEMORY_SIZE (64ULL * 1024 * 1024)
#define ELF_USER_MIN 0x10000ULL
#define ELF_USER_MAX 0x00007F00000000ULL
#define ELF_USER_STACK_BASE 0x7FFFC000ULL
#define ELF_EXEC_BASE 0x00400000ULL
#define ELF_LIBC_BASE 0x50000000ULL
#define ELF_MAX_DYNAMIC_ENTRIES 4096
#define ELF_MAX_SYMBOLS 65536
#define ELF_MAX_RELOCATIONS 131072
#define ELF_MAX_SYMBOL_NAME 256
#define ELF_SHN_UNDEF 0
#define ELF_SHN_ABS 0xFFF1
#define ELF_ST_BIND(info) ((info) >> 4)

#define DT_NULL 0
#define DT_NEEDED 1
#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_RELAENT 9
#define DT_STRSZ 10
#define DT_SYMENT 11
#define DT_JMPREL 23
#define DT_PLTRELSZ 2
#define DT_PLTREL 20

#define R_X86_64_64 1
#define R_X86_64_PC32 2
#define R_X86_64_COPY 5
#define R_X86_64_GLOB_DAT 6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE 8
#define R_X86_64_32 10
#define R_X86_64_32S 11

typedef struct __attribute__((packed))
{
    uint8_t ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t program_header_offset;
    uint64_t section_header_offset;
    uint32_t flags;
    uint16_t header_size;
    uint16_t program_header_entry_size;
    uint16_t program_header_count;
    uint16_t section_header_entry_size;
    uint16_t section_header_count;
    uint16_t section_name_index;
} elf64_header_t;

typedef struct __attribute__((packed))
{
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t virtual_address;
    uint64_t physical_address;
    uint64_t file_size;
    uint64_t memory_size;
    uint64_t alignment;
} elf64_program_header_t;

typedef struct __attribute__((packed))
{
    int64_t tag;
    uint64_t value;
} elf64_dynamic_t;

typedef struct __attribute__((packed))
{
    uint64_t offset;
    uint64_t info;
    int64_t addend;
} elf64_rela_t;

typedef struct __attribute__((packed))
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
    uint64_t bias;
    uint64_t entry;
    uint64_t dynamic_address;
    uint64_t dynamic_size;
    uint64_t string_table;
    uint64_t string_size;
    uint64_t symbol_table;
    uint64_t symbol_entry_size;
    uint64_t hash_table;
    uint64_t rela_table;
    uint64_t rela_size;
    uint64_t rela_entry_size;
    uint64_t plt_rela_table;
    uint64_t plt_rela_size;
    bool has_dynamic;
    bool needs_libc;
} elf_object_t;

static void bytes_copy(void *destination, const void *source, uint64_t size)
{
    uint8_t *dst = destination;
    const uint8_t *src = source;
    for (uint64_t i = 0; i < size; i++)
        dst[i] = src[i];
}

static bool range_within(uint64_t offset, uint64_t size, uint64_t limit)
{
    return offset <= limit && size <= limit - offset;
}

static bool add_overflow(uint64_t left, uint64_t right, uint64_t *result)
{
    *result = left + right;
    return *result < left;
}

static bool align_page_up(uint64_t value, uint64_t *result)
{
    if (value > UINT64_MAX - (VMM_PAGE_SIZE - 1))
        return false;
    *result = (value + VMM_PAGE_SIZE - 1) &
              ~(VMM_PAGE_SIZE - 1);
    return true;
}

static bool read_file(const char *path, uint8_t **image, uint64_t *size)
{
    vfs_file_t file;
    int64_t result = vfs_open(path, &file);
    if (result < 0)
        return false;
    if (!file.size || file.size > ELF_MAX_FILE_SIZE)
        return false;

    uint8_t *data = kmalloc(file.size);
    if (!data)
        return false;

    uint64_t loaded = 0;
    while (loaded < file.size)
    {
        uint64_t amount = file.size - loaded;
        if (amount > 512)
            amount = 512;

        result = vfs_read(&file, data + loaded, amount);
        if (result <= 0 || (uint64_t)result > amount)
        {
            kfree(data);
            return false;
        }
        loaded += (uint64_t)result;
    }

    *image = data;
    *size = file.size;
    return true;
}

static bool parse_header(
    const uint8_t *image,
    uint64_t image_size,
    elf64_header_t *header
)
{
    if (image_size < sizeof(*header))
        return false;
    bytes_copy(header, image, sizeof(*header));

    uint64_t ph_size =
        (uint64_t)header->program_header_count *
        header->program_header_entry_size;
    return header->ident[0] == 0x7F &&
        header->ident[1] == 'E' &&
        header->ident[2] == 'L' &&
        header->ident[3] == 'F' &&
        header->ident[4] == ELF_CLASS_64 &&
        header->ident[5] == ELF_DATA_LSB &&
        header->ident[6] == ELF_VERSION_CURRENT &&
        (header->type == ELF_TYPE_EXEC ||
         header->type == ELF_TYPE_DYN) &&
        header->machine == ELF_MACHINE_X86_64 &&
        header->version == ELF_VERSION_CURRENT &&
        header->header_size == sizeof(*header) &&
        header->program_header_entry_size ==
            sizeof(elf64_program_header_t) &&
        header->program_header_count > 0 &&
        header->program_header_count <= ELF_MAX_PROGRAM_HEADERS &&
        range_within(
            header->program_header_offset,
            ph_size,
            image_size
        );
}

static bool get_program_header(
    const uint8_t *image,
    uint64_t image_size,
    const elf64_header_t *header,
    uint16_t index,
    elf64_program_header_t *program
)
{
    uint64_t offset =
        header->program_header_offset +
        (uint64_t)index * header->program_header_entry_size;
    if (!range_within(offset, sizeof(*program), image_size))
        return false;

    bytes_copy(program, image + offset, sizeof(*program));
    return true;
}

static bool validate_segment(
    const elf64_program_header_t *program,
    uint64_t image_size,
    uint64_t bias,
    uint64_t *start,
    uint64_t *end
)
{
    if (program->file_size > program->memory_size ||
        !range_within(program->offset, program->file_size, image_size) ||
        program->memory_size == 0 ||
        ((program->flags & (ELF_PF_W | ELF_PF_X)) ==
            (ELF_PF_W | ELF_PF_X)))
        return false;

    uint64_t virtual_start;
    uint64_t virtual_end;
    if (add_overflow(bias, program->virtual_address, &virtual_start) ||
        add_overflow(virtual_start, program->memory_size, &virtual_end) ||
        virtual_start < ELF_USER_MIN ||
        virtual_end > ELF_USER_MAX ||
        virtual_end > ELF_USER_STACK_BASE)
        return false;

    if (program->alignment > 1)
    {
        uint64_t alignment = program->alignment;
        if ((alignment & (alignment - 1)) != 0 ||
            (program->virtual_address & (alignment - 1)) !=
                (program->offset & (alignment - 1)))
            return false;
    }

    *start = virtual_start & ~(VMM_PAGE_SIZE - 1);
    return align_page_up(virtual_end, end);
}

static bool segment_pages_overlap(
    const elf64_program_header_t *left,
    const elf64_program_header_t *right,
    uint64_t bias
)
{
    uint64_t left_address;
    uint64_t right_address;
    uint64_t left_end;
    uint64_t right_end;
    if (add_overflow(bias, left->virtual_address, &left_address) ||
        add_overflow(bias, right->virtual_address, &right_address) ||
        add_overflow(left_address, left->memory_size, &left_end) ||
        add_overflow(right_address, right->memory_size, &right_end) ||
        !align_page_up(left_end, &left_end) ||
        !align_page_up(right_end, &right_end))
        return true;

    uint64_t left_start = left_address & ~(VMM_PAGE_SIZE - 1);
    uint64_t right_start = right_address & ~(VMM_PAGE_SIZE - 1);
    return left_start < right_end && right_start < left_end;
}

static bool map_segment(
    vmm_address_space_t address_space,
    const uint8_t *image,
    const elf64_program_header_t *program,
    uint64_t bias
)
{
    uint64_t segment_address;
    if (add_overflow(bias, program->virtual_address, &segment_address))
        return false;

    uint64_t page_start = segment_address & ~(VMM_PAGE_SIZE - 1);
    uint64_t segment_end;
    uint64_t page_end;
    if (add_overflow(segment_address, program->memory_size, &segment_end) ||
        !align_page_up(segment_end, &page_end))
        return false;

    uint64_t flags = VMM_PRESENT | VMM_USER;
    if (program->flags & ELF_PF_W)
        flags |= VMM_WRITABLE;
    if (!(program->flags & ELF_PF_X) && paging_nx_supported())
        flags |= VMM_NO_EXECUTE;

    for (uint64_t address = page_start;
         address < page_end;
         address += VMM_PAGE_SIZE)
    {
        uint64_t physical = pmm_alloc_page();
        if (!physical)
            return false;
        if (!vmm_map_page_in(
                address_space,
                address,
                physical,
                flags))
        {
            pmm_free_page(physical);
            return false;
        }
        void *page = paging_phys_to_virt(physical);
        if (!page)
            return false;
        uint8_t *bytes = page;
        for (uint64_t i = 0; i < VMM_PAGE_SIZE; i++)
            bytes[i] = 0;
    }

    uint64_t copied = 0;
    while (copied < program->file_size)
    {
        uint64_t address = segment_address + copied;
        uint64_t physical = vmm_translate_in(address_space, address);
        if (!physical)
            return false;

        uint64_t amount =
            VMM_PAGE_SIZE - (address & (VMM_PAGE_SIZE - 1));
        if (amount > program->file_size - copied)
            amount = program->file_size - copied;

        uint8_t *destination = paging_phys_to_virt(physical);
        if (!destination)
            return false;
        bytes_copy(
            destination,
            image + program->offset + copied,
            amount
        );
        copied += amount;
    }

    return true;
}

static bool load_object(
    vmm_address_space_t address_space,
    const uint8_t *image,
    uint64_t image_size,
    uint64_t bias,
    bool allow_zero_entry,
    elf_object_t *object
)
{
    elf64_header_t header;
    if (!parse_header(image, image_size, &header))
        return false;
    if ((header.type == ELF_TYPE_EXEC && bias != 0) ||
        (header.type == ELF_TYPE_DYN && bias == 0))
        return false;

    *object = (elf_object_t){0};
    object->bias = bias;
    if (add_overflow(bias, header.entry, &object->entry))
        return false;

    bool has_load = false;
    bool entry_executable = false;
    uint64_t total_memory = 0;

    for (uint16_t i = 0; i < header.program_header_count; i++)
    {
        elf64_program_header_t program;
        if (!get_program_header(
                image, image_size, &header, i, &program))
            return false;

        if (program.type == ELF_PT_INTERP)
            return false;

        if (program.type == ELF_PT_DYNAMIC)
        {
            if (object->has_dynamic ||
                add_overflow(bias, program.virtual_address,
                             &object->dynamic_address))
                return false;
            object->dynamic_size = program.memory_size;
            object->has_dynamic = true;
        }

        if (program.type != ELF_PT_LOAD)
            continue;

        uint64_t page_start;
        uint64_t page_end;
        if (!validate_segment(
                &program,
                image_size,
                bias,
                &page_start,
                &page_end))
            return false;

        if (program.memory_size > ELF_MAX_MEMORY_SIZE - total_memory)
            return false;
        total_memory += program.memory_size;

        uint64_t start = bias + program.virtual_address;
        if ((program.flags & ELF_PF_X) &&
            object->entry >= start &&
            object->entry < start + program.memory_size)
            entry_executable = true;

        for (uint16_t j = 0; j < i; j++)
        {
            elf64_program_header_t previous;
            if (!get_program_header(
                    image, image_size, &header, j, &previous))
                return false;
            if (previous.type == ELF_PT_LOAD &&
                segment_pages_overlap(&previous, &program, bias))
                return false;
        }

        has_load = true;
    }

    if (!has_load ||
        (!entry_executable && !(allow_zero_entry && header.entry == 0)))
        return false;

    for (uint16_t i = 0; i < header.program_header_count; i++)
    {
        elf64_program_header_t program;
        if (!get_program_header(
                image, image_size, &header, i, &program))
            return false;
        if (program.type == ELF_PT_LOAD &&
            !map_segment(address_space, image, &program, bias))
            return false;
    }

    return !object->has_dynamic ||
        (object->dynamic_size >= sizeof(elf64_dynamic_t) &&
         object->dynamic_size <=
             ELF_MAX_DYNAMIC_ENTRIES * sizeof(elf64_dynamic_t));
}

static bool read_user_memory(
    vmm_address_space_t address_space,
    uint64_t address,
    void *destination,
    uint64_t size
)
{
    uint8_t *output = destination;
    while (size)
    {
        uint64_t physical = vmm_translate_in(address_space, address);
        if (!physical)
            return false;
        uint64_t amount =
            VMM_PAGE_SIZE - (address & (VMM_PAGE_SIZE - 1));
        if (amount > size)
            amount = size;
        const uint8_t *source = paging_phys_to_virt(physical);
        if (!source)
            return false;
        bytes_copy(output, source, amount);
        address += amount;
        output += amount;
        size -= amount;
    }
    return true;
}

static bool write_user_memory(
    vmm_address_space_t address_space,
    uint64_t address,
    const void *source,
    uint64_t size
)
{
    const uint8_t *input = source;
    while (size)
    {
        uint64_t physical = vmm_translate_in(address_space, address);
        if (!physical)
            return false;
        uint64_t amount =
            VMM_PAGE_SIZE - (address & (VMM_PAGE_SIZE - 1));
        if (amount > size)
            amount = size;
        uint8_t *destination = paging_phys_to_virt(physical);
        if (!destination)
            return false;
        bytes_copy(destination, input, amount);
        address += amount;
        input += amount;
        size -= amount;
    }
    return true;
}

static bool read_object_memory(
    vmm_address_space_t address_space,
    const elf_object_t *object,
    uint64_t address,
    void *destination,
    uint64_t size
)
{
    uint64_t absolute;
    return !add_overflow(object->bias, address, &absolute) &&
        read_user_memory(address_space, absolute, destination, size);
}

static bool parse_dynamic(
    vmm_address_space_t address_space,
    elf_object_t *object,
    bool executable
)
{
    if (!object->has_dynamic)
        return true;

    bool terminated = false;
    for (uint64_t offset = 0;
         offset + sizeof(elf64_dynamic_t) <= object->dynamic_size;
         offset += sizeof(elf64_dynamic_t))
    {
        elf64_dynamic_t dynamic;
        if (!read_user_memory(
                address_space,
                object->dynamic_address + offset,
                &dynamic,
                sizeof(dynamic)))
            return false;

        if (dynamic.tag == DT_NULL)
        {
            terminated = true;
            break;
        }

        switch (dynamic.tag)
        {
            case DT_NEEDED:
                if (!executable)
                    return false;
                object->needs_libc = true;
                break;
            case DT_HASH:
                object->hash_table = dynamic.value;
                break;
            case DT_STRTAB:
                object->string_table = dynamic.value;
                break;
            case DT_STRSZ:
                object->string_size = dynamic.value;
                break;
            case DT_SYMTAB:
                object->symbol_table = dynamic.value;
                break;
            case DT_SYMENT:
                object->symbol_entry_size = dynamic.value;
                break;
            case DT_RELA:
                object->rela_table = dynamic.value;
                break;
            case DT_RELASZ:
                object->rela_size = dynamic.value;
                break;
            case DT_RELAENT:
                object->rela_entry_size = dynamic.value;
                break;
            case DT_JMPREL:
                object->plt_rela_table = dynamic.value;
                break;
            case DT_PLTRELSZ:
                object->plt_rela_size = dynamic.value;
                break;
            case DT_PLTREL:
                if (dynamic.value != DT_RELA)
                    return false;
                break;
            default:
                break;
        }
    }

    if (!terminated || !object->hash_table ||
        !object->string_table || !object->symbol_table ||
        object->symbol_entry_size != sizeof(elf64_symbol_t) ||
        (object->rela_size &&
         object->rela_entry_size != sizeof(elf64_rela_t)) ||
        object->rela_size % sizeof(elf64_rela_t) ||
        object->plt_rela_size % sizeof(elf64_rela_t) ||
        object->rela_size / sizeof(elf64_rela_t) >
            ELF_MAX_RELOCATIONS ||
        object->plt_rela_size / sizeof(elf64_rela_t) >
            ELF_MAX_RELOCATIONS ||
        object->string_size == 0)
        return false;
    return true;
}

static bool object_string_equals(
    vmm_address_space_t address_space,
    const elf_object_t *object,
    uint64_t offset,
    const char *expected
)
{
    if (offset >= object->string_size)
        return false;
    for (uint64_t i = 0; i < object->string_size - offset; i++)
    {
        char character;
        if (!read_object_memory(
                address_space,
                object,
                object->string_table + offset + i,
                &character,
                sizeof(character)))
            return false;
        if (character != expected[i])
            return false;
        if (character == '\0')
            return true;
    }
    return false;
}

static bool executable_needs_libc(
    vmm_address_space_t address_space,
    elf_object_t *object
)
{
    if (!object->needs_libc)
        return false;

    for (uint64_t offset = 0;
         offset + sizeof(elf64_dynamic_t) <= object->dynamic_size;
         offset += sizeof(elf64_dynamic_t))
    {
        elf64_dynamic_t dynamic;
        if (!read_user_memory(
                address_space,
                object->dynamic_address + offset,
                &dynamic,
                sizeof(dynamic)))
            return false;
        if (dynamic.tag == DT_NULL)
            break;
        if (dynamic.tag == DT_NEEDED &&
            !object_string_equals(
                address_space,
                object,
                    dynamic.value,
                "libc.so"))
            return false;
    }
    return true;
}

static bool read_symbol(
    vmm_address_space_t address_space,
    const elf_object_t *object,
    uint32_t index,
    elf64_symbol_t *symbol
)
{
    if (!object->hash_table || index >= ELF_MAX_SYMBOLS)
        return false;

    uint32_t hash_header[2];
    if (!read_object_memory(
            address_space,
            object,
            object->hash_table,
            hash_header,
            sizeof(hash_header)) ||
        index >= hash_header[1])
        return false;

    uint64_t address =
        object->symbol_table +
        (uint64_t)index * object->symbol_entry_size;
    return read_object_memory(
        address_space,
        object,
        address,
        symbol,
        sizeof(*symbol)
    );
}

static uint32_t elf_hash(const char *string)
{
    uint32_t hash = 0;
    while (*string)
    {
        hash = (hash << 4) + (uint8_t)*string++;
        uint32_t high = hash & 0xF0000000;
        if (high)
            hash ^= high >> 24;
        hash &= ~high;
    }
    return hash;
}

static bool symbol_name_equals(
    vmm_address_space_t address_space,
    const elf_object_t *object,
    const elf64_symbol_t *symbol,
    const char *name
)
{
    if (symbol->name >= object->string_size)
        return false;

    uint64_t address =
        object->string_table + symbol->name;
    for (uint64_t i = 0; i < object->string_size - symbol->name; i++)
    {
        char character;
        if (!read_object_memory(
                address_space,
                object,
                address + i,
                &character,
                sizeof(character)))
            return false;
        if (character != name[i])
            return false;
        if (!character)
            return true;
    }
    return false;
}

static bool find_symbol(
    vmm_address_space_t address_space,
    const elf_object_t *object,
    const char *name,
    uint64_t *value,
    bool *weak
)
{
    if (!object->has_dynamic || !object->hash_table)
        return false;

    uint32_t hash_header[2];
    if (!read_object_memory(
            address_space,
            object,
            object->hash_table,
            hash_header,
            sizeof(hash_header)) ||
        hash_header[0] == 0 ||
        hash_header[1] > ELF_MAX_SYMBOLS)
        return false;

    uint32_t bucket_index = elf_hash(name) % hash_header[0];
    uint64_t buckets_address =
        object->hash_table + sizeof(hash_header) +
        (uint64_t)bucket_index * sizeof(uint32_t);
    uint32_t symbol_index;
    if (!read_object_memory(
            address_space,
            object,
            buckets_address,
            &symbol_index,
            sizeof(symbol_index)))
        return false;

    uint64_t chains_address =
        object->hash_table + sizeof(hash_header) +
        (uint64_t)hash_header[0] * sizeof(uint32_t);

    uint32_t steps = 0;
    while (symbol_index && symbol_index < hash_header[1] &&
           steps++ < hash_header[1])
    {
        elf64_symbol_t symbol;
        if (!read_symbol(address_space, object, symbol_index, &symbol))
            return false;

        if (symbol.section_index != ELF_SHN_UNDEF &&
            ELF_ST_BIND(symbol.info) != 0 &&
            symbol_name_equals(address_space, object, &symbol, name))
        {
            uint64_t symbol_value = symbol.value;
            if (symbol.section_index != ELF_SHN_ABS &&
                add_overflow(object->bias, symbol.value, &symbol_value))
                return false;
            *value = symbol_value;
            *weak = ELF_ST_BIND(symbol.info) == 2;
            return true;
        }

        if (!read_object_memory(
                address_space,
                object,
                chains_address +
                    (uint64_t)symbol_index * sizeof(uint32_t),
                &symbol_index,
                sizeof(symbol_index)))
            return false;
    }
    return false;
}

static bool read_symbol_name(
    vmm_address_space_t address_space,
    const elf_object_t *object,
    uint32_t symbol_index,
    elf64_symbol_t *symbol,
    char name[ELF_MAX_SYMBOL_NAME]
);

static bool resolve_symbol(
    vmm_address_space_t address_space,
    const elf_object_t *reference_object,
    const elf_object_t *executable,
    const elf_object_t *library,
    uint32_t symbol_index,
    uint64_t *value
)
{
    if (symbol_index == 0)
    {
        *value = 0;
        return true;
    }

    elf64_symbol_t reference;
    char name[ELF_MAX_SYMBOL_NAME];
    if (!read_symbol_name(
            address_space,
            reference_object,
            symbol_index,
            &reference,
            name))
        return false;

    bool weak = false;
    if (find_symbol(
            address_space,
            executable,
            name,
            value,
            &weak))
        return true;
    if (find_symbol(
            address_space,
            library,
            name,
            value,
            &weak))
        return true;

    if (ELF_ST_BIND(reference.info) == 2)
    {
        *value = 0;
        return true;
    }
    return false;
}

static bool read_symbol_name(
    vmm_address_space_t address_space,
    const elf_object_t *object,
    uint32_t symbol_index,
    elf64_symbol_t *symbol,
    char name[ELF_MAX_SYMBOL_NAME]
)
{
    if (!read_symbol(address_space, object, symbol_index, symbol) ||
        symbol->name >= object->string_size)
        return false;

    uint64_t length = 0;
    for (; length + 1 < ELF_MAX_SYMBOL_NAME &&
           symbol->name + length < object->string_size;
         length++)
    {
        if (!read_object_memory(
                address_space,
                object,
                object->string_table + symbol->name + length,
                &name[length],
                sizeof(name[length])))
            return false;
        if (!name[length])
            break;
    }
    return length + 1 < ELF_MAX_SYMBOL_NAME && name[length] == '\0';
}

static bool apply_relocation_table(
    vmm_address_space_t address_space,
    const elf_object_t *target_object,
    const elf_object_t *executable,
    const elf_object_t *library,
    uint64_t table,
    uint64_t size
)
{
    if (!size)
        return true;

    for (uint64_t offset = 0; offset < size;
         offset += sizeof(elf64_rela_t))
    {
        elf64_rela_t relocation;
        if (!read_object_memory(
                address_space,
                target_object,
                table + offset,
                &relocation,
                sizeof(relocation)))
            return false;

        uint32_t type = (uint32_t)relocation.info;
        uint32_t symbol_index = (uint32_t)(relocation.info >> 32);
        if (type == 0)
            continue;

        uint64_t place;
        if (add_overflow(
                target_object->bias,
                relocation.offset,
                &place))
            return false;

        if (type == R_X86_64_RELATIVE)
        {
            if (symbol_index != 0)
                return false;
            uint64_t value = target_object->bias +
                             (uint64_t)relocation.addend;
            if (!vmm_user_range_valid(
                    address_space, place, sizeof(value), true) ||
                !write_user_memory(
                    address_space, place, &value, sizeof(value)))
                return false;
            continue;
        }

        if (type == R_X86_64_COPY)
        {
            elf64_symbol_t reference;
            char name[ELF_MAX_SYMBOL_NAME];
            uint64_t source;
            bool weak = false;
            if (target_object != executable ||
                !read_symbol_name(
                    address_space,
                    target_object,
                    symbol_index,
                    &reference,
                    name) ||
                !reference.size ||
                reference.size > ELF_MAX_MEMORY_SIZE ||
                !find_symbol(
                    address_space,
                    library,
                    name,
                    &source,
                    &weak) ||
                !vmm_user_range_valid(
                    address_space, source, reference.size, false) ||
                !vmm_user_range_valid(
                    address_space, place, reference.size, true))
                return false;

            uint8_t copy_buffer[128];
            uint64_t copied = 0;
            while (copied < reference.size)
            {
                uint64_t amount = reference.size - copied;
                if (amount > sizeof(copy_buffer))
                    amount = sizeof(copy_buffer);
                if (!read_user_memory(
                        address_space,
                        source + copied,
                        copy_buffer,
                        amount) ||
                    !write_user_memory(
                        address_space,
                        place + copied,
                        copy_buffer,
                        amount))
                    return false;
                copied += amount;
            }
            continue;
        }

        if (type != R_X86_64_64 &&
            type != R_X86_64_PC32 &&
            type != R_X86_64_GLOB_DAT &&
            type != R_X86_64_JUMP_SLOT &&
            type != R_X86_64_32 &&
            type != R_X86_64_32S)
            return false;

        uint64_t symbol_value;
        if (!resolve_symbol(
                address_space,
                target_object,
                executable,
                library,
                symbol_index,
                &symbol_value))
            return false;

        if (type == R_X86_64_PC32)
        {
            uint64_t result = symbol_value +
                (uint64_t)relocation.addend - place;
            int64_t signed_result = (int64_t)result;
            if (signed_result < INT32_MIN ||
                signed_result > INT32_MAX ||
                !vmm_user_range_valid(
                    address_space, place, sizeof(int32_t), true))
                return false;
            int32_t value = (int32_t)signed_result;
            if (!write_user_memory(
                    address_space, place, &value, sizeof(value)))
                return false;
            continue;
        }

        uint64_t value = symbol_value +
                         (uint64_t)relocation.addend;
        if (type == R_X86_64_32)
        {
            if (value > UINT32_MAX ||
                !vmm_user_range_valid(
                    address_space, place, sizeof(uint32_t), true))
                return false;
            uint32_t value32 = (uint32_t)value;
            if (!write_user_memory(
                    address_space, place, &value32, sizeof(value32)))
                return false;
        }
        else if (type == R_X86_64_32S)
        {
            int64_t signed_value = (int64_t)value;
            if (signed_value < INT32_MIN ||
                signed_value > INT32_MAX ||
                !vmm_user_range_valid(
                    address_space, place, sizeof(int32_t), true))
                return false;
            int32_t value32 = (int32_t)signed_value;
            if (!write_user_memory(
                    address_space, place, &value32, sizeof(value32)))
                return false;
        }
        else
        {
            if (!vmm_user_range_valid(
                    address_space, place, sizeof(value), true) ||
                !write_user_memory(
                    address_space, place, &value, sizeof(value)))
                return false;
        }
    }
    return true;
}

static bool apply_relocations(
    vmm_address_space_t address_space,
    const elf_object_t *target,
    const elf_object_t *executable,
    const elf_object_t *library
)
{
    if (!target->has_dynamic)
        return true;

    return apply_relocation_table(
            address_space,
            target,
            executable,
            library,
            target->rela_table,
            target->rela_size) &&
        apply_relocation_table(
            address_space,
            target,
            executable,
            library,
            target->plt_rela_table,
            target->plt_rela_size);
}

int64_t elf_load_executable(
    vmm_address_space_t address_space,
    const char *path,
    uint64_t *entry_point
)
{
    if (!address_space || !path || !entry_point)
        return -SYSCALL_EINVAL;

    uint8_t *program_image;
    uint64_t program_size;
    if (!read_file(path, &program_image, &program_size))
    {
        serial_write("ELF loader: could not read executable: ");
        serial_write(path);
        serial_putc('\n');
        return -SYSCALL_ENOEXEC;
    }

    elf64_header_t program_header;
    if (!parse_header(program_image, program_size, &program_header))
    {
        serial_write("ELF loader: invalid executable header: ");
        serial_write(path);
        serial_putc('\n');
        kfree(program_image);
        return -SYSCALL_ENOEXEC;
    }

    uint64_t program_bias =
        program_header.type == ELF_TYPE_DYN ? ELF_EXEC_BASE : 0;
    elf_object_t executable;
    if (!load_object(
            address_space,
            program_image,
            program_size,
            program_bias,
            false,
            &executable))
    {
        serial_write("ELF loader: invalid executable segments: ");
        serial_write(path);
        serial_putc('\n');
        kfree(program_image);
        return -SYSCALL_ENOEXEC;
    }

    bool dynamic = executable.has_dynamic;
    if (dynamic != (program_header.type == ELF_TYPE_DYN))
    {
        serial_write("ELF loader: executable dynamic type mismatch: ");
        serial_write(path);
        serial_putc('\n');
        kfree(program_image);
        return -SYSCALL_ENOEXEC;
    }

    if (!dynamic)
    {
        *entry_point = executable.entry;
        kfree(program_image);
        return 0;
    }

    uint8_t *library_image = NULL;
    uint64_t library_size = 0;
    elf_object_t library = {0};

    if (!parse_dynamic(address_space, &executable, true))
    {
        serial_write("ELF loader: invalid executable dynamic section: ");
        serial_write(path);
        serial_putc('\n');
        kfree(program_image);
        return -SYSCALL_ENOEXEC;
    }
    if (!executable_needs_libc(address_space, &executable))
    {
        serial_write("ELF loader: unsupported executable dependencies: ");
        serial_write(path);
        serial_putc('\n');
        kfree(program_image);
        return -SYSCALL_ENOEXEC;
    }
    if (!read_file("/lib/libc.so", &library_image, &library_size))
    {
        serial_write("ELF loader: could not read /lib/libc.so\n");
        kfree(program_image);
        return -SYSCALL_ENOEXEC;
    }

    elf64_header_t library_header;
    if (!parse_header(library_image, library_size, &library_header) ||
        library_header.type != ELF_TYPE_DYN ||
        !load_object(
            address_space,
            library_image,
            library_size,
            ELF_LIBC_BASE,
            true,
            &library) ||
        !library.has_dynamic ||
        !parse_dynamic(address_space, &library, false))
    {
        serial_write("ELF loader: invalid libc image or dynamic section\n");
        kfree(library_image);
        kfree(program_image);
        return -SYSCALL_ENOEXEC;
    }

    bool relocated =
        apply_relocations(
            address_space, &library, &executable, &library) &&
        apply_relocations(
            address_space, &executable, &executable, &library);

    kfree(library_image);
    kfree(program_image);
    if (!relocated)
    {
        serial_write("ELF loader: executable relocation failed: ");
        serial_write(path);
        serial_putc('\n');
        return -SYSCALL_ENOEXEC;
    }

    *entry_point = executable.entry;
    return 0;
}
