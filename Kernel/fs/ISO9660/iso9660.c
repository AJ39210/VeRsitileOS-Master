
#include <stdint.h>
#include <stdbool.h>

#include "iso9660.h"

#define PVD_START_SECTOR 16
#define MAX_VOLUME_DESCRIPTORS 64

#define DIRECTORY_FLAG 0x02

static iso9660_sector_reader_t sector_reader = 0;
static void *reader_context = 0;

static iso9660_file_t root_directory;
static bool filesystem_ready = false;
static bool rock_ridge_enabled;
static uint8_t rock_ridge_skip;
static uint32_t volume_sector_count;

static uint8_t sector_buffer[ISO9660_SECTOR_SIZE];

/* Read a little-endian 32-bit value. */
static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* Read one ISO sector. */
static bool read_sector(uint32_t lba, uint8_t *buffer)
{
    if (!sector_reader)
        return false;

    return sector_reader(reader_context, lba, buffer);
}

static bool record_has_susp_sp(
    const uint8_t *record,
    uint8_t record_length
)
{
    if (record_length < 34)
        return false;
    uint8_t name_length = record[32];
    uint32_t position = 33 + name_length;
    if ((name_length & 1) == 0)
        position++;
    if (position + 7 > record_length ||
        record[position] != 'S' ||
        record[position + 1] != 'P' ||
        record[position + 2] != 7 ||
        record[position + 3] != 1 ||
        record[position + 4] != 0xBE ||
        record[position + 5] != 0xEF)
        return false;
    rock_ridge_skip = record[position + 6];
    return true;
}

static uint32_t read_rock_ridge_name(
    const uint8_t *record,
    uint8_t record_length,
    uint8_t name_length,
    char name[ISO9660_MAX_NAME]
)
{
    if (!rock_ridge_enabled)
    {
        name[0] = '\0';
        return 0;
    }

    uint32_t position = 33 + name_length;
    if ((name_length & 1) == 0)
        position++;
    position += rock_ridge_skip;

    uint32_t output_length = 0;
    while (position + 4 <= record_length)
    {
        const uint8_t *entry = record + position;
        uint8_t entry_length = entry[2];
        if (entry_length < 4 ||
            entry_length > record_length - position)
            break;
        if (entry[0] == 'N' && entry[1] == 'M' &&
            entry_length >= 5)
        {
            uint8_t flags = entry[4];
            if ((flags & 0x0E) == 0)
            {
                uint32_t fragment_length = entry_length - 5;
                if (fragment_length > ISO9660_MAX_NAME - 1 -
                    output_length)
                    fragment_length =
                        ISO9660_MAX_NAME - 1 - output_length;
                for (uint32_t i = 0; i < fragment_length; i++)
                    name[output_length++] = (char)entry[5 + i];
            }
        }
        position += entry_length;
    }

    name[output_length] = '\0';
    return output_length;
}

/* ASCII case-insensitive comparison. */
static char to_upper(char c)
{
    if (c >= 'a' && c <= 'z')
        return c - 32;

    return c;
}

static bool name_equals(const char *a, const char *b)
{
    while (*a && *b)
    {
        if (to_upper(*a) != to_upper(*b))
            return false;

        a++;
        b++;
    }

    return *a == '\0' && *b == '\0';
}

/* Copy a directory record into our file structure. */
static bool parse_record(
    const uint8_t *record,
    uint8_t record_length,
    iso9660_file_t *file
)
{
    if (record_length < 34)
        return false;

    uint8_t name_length = record[32];

    if (33 + name_length > record_length)
        return false;

    /* Skip special entries: current directory and parent directory. */
    if (name_length == 1 &&
        (record[33] == 0 || record[33] == 1))
        return false;

    file->lba = read_le32(record + 2);
    file->size = read_le32(record + 10);

    file->is_directory =
        (record[25] & DIRECTORY_FLAG) != 0;

    uint32_t output_length = 0;

    for (uint8_t i = 0; i < name_length; i++)
    {
        char c = (char)record[33 + i];

        /* Remove the ISO version suffix, e.g. ;1 */
        if (c == ';')
            break;

        if (output_length >= ISO9660_MAX_NAME - 1)
            break;

        file->name[output_length++] = c;
    }

    char rock_ridge_name[ISO9660_MAX_NAME];
    uint32_t rock_ridge_length = read_rock_ridge_name(
        record,
        record_length,
        name_length,
        rock_ridge_name);
    if (rock_ridge_length)
    {
        for (uint32_t i = 0; i <= rock_ridge_length; i++)
            file->name[i] = rock_ridge_name[i];
        return true;
    }

    if (output_length && file->name[output_length - 1] == '.')
        output_length--;

    file->name[output_length] = '\0';

    return true;
}

/* Initialize ISO9660. */
bool iso9660_init(
    iso9660_sector_reader_t reader,
    void *context
)
{
    filesystem_ready = false;
    rock_ridge_enabled = false;
    rock_ridge_skip = 0;
    volume_sector_count = 0;
    sector_reader = reader;
    reader_context = context;

    if (!sector_reader)
        return false;

    for (uint32_t i = 0; i < MAX_VOLUME_DESCRIPTORS; i++)
    {
        uint32_t lba = PVD_START_SECTOR + i;

        if (!read_sector(lba, sector_buffer))
            return false;

        /* Every volume descriptor must contain CD001. */
        if (sector_buffer[1] != 'C' ||
            sector_buffer[2] != 'D' ||
            sector_buffer[3] != '0' ||
            sector_buffer[4] != '0' ||
            sector_buffer[5] != '1')
        {
            return false;
        }

        uint8_t descriptor_type = sector_buffer[0];

        /* Primary Volume Descriptor */
        if (descriptor_type == 1)
        {
            if (sector_buffer[6] != 1)
                return false;

            /*
             * Root directory record starts at byte 156.
             */
            uint8_t *root_record = sector_buffer + 156;
            uint8_t root_length = root_record[0];

            if (root_length < 34)
                return false;

            root_directory.lba =
                read_le32(root_record + 2);

            root_directory.size =
                read_le32(root_record + 10);

            root_directory.is_directory = true;
            root_directory.name[0] = '/';
            root_directory.name[1] = '\0';

            volume_sector_count = read_le32(sector_buffer + 80);
            filesystem_ready = true;
            if (read_sector(root_directory.lba, sector_buffer) &&
                record_has_susp_sp(
                    sector_buffer,
                    sector_buffer[0]))
                rock_ridge_enabled = true;
            return true;
        }

        /* Volume Descriptor Set Terminator */
        if (descriptor_type == 255)
            break;
    }

    return false;
}

bool iso9660_is_ready(void)
{
    return filesystem_ready;
}

uint32_t iso9660_get_volume_sectors(void)
{
    return filesystem_ready ? volume_sector_count : 0;
}

bool iso9660_get_root(iso9660_file_t *root)
{
    if (!filesystem_ready || !root)
        return false;

    *root = root_directory;
    return true;
}

/*
 * Find a child inside a directory.
 *
 * Directory records cannot cross sector boundaries.
 */
static bool find_child(
    const iso9660_file_t *directory,
    const char *name,
    iso9660_file_t *result
)
{
    if (!filesystem_ready ||
        !directory ||
        !directory->is_directory ||
        !name ||
        !result)
        return false;

    uint32_t position = 0;

    while (position < directory->size)
    {
        uint32_t sector_offset =
            position % ISO9660_SECTOR_SIZE;

        uint32_t sector_number =
            position / ISO9660_SECTOR_SIZE;

        uint32_t current_lba =
            directory->lba + sector_number;

        if (!read_sector(current_lba, sector_buffer))
            return false;

        uint8_t record_length = sector_buffer[sector_offset];

        /*
         * A zero record length means the rest of this
         * sector is padding.
         */
        if (record_length == 0)
        {
            position +=
                ISO9660_SECTOR_SIZE - sector_offset;

            continue;
        }

        if (record_length < 34 ||
            record_length > ISO9660_SECTOR_SIZE - sector_offset ||
            record_length > directory->size - position)
        {
            return false;
        }

        iso9660_file_t entry;

        if (parse_record(
                sector_buffer + sector_offset,
                record_length,
                &entry))
        {
            if (name_equals(entry.name, name))
            {
                *result = entry;
                return true;
            }
        }

        position += record_length;
    }

    return false;
}

bool iso9660_list(
    const char *path,
    uint64_t offset,
    iso9660_dirent_t *entries,
    uint64_t capacity,
    uint64_t *total
)
{
    iso9660_file_t directory;
    if (!total || (capacity && !entries) ||
        !iso9660_find(path, &directory) ||
        !directory.is_directory)
        return false;

    uint64_t position = 0;
    uint64_t index = 0;
    uint64_t written = 0;
    while (position < directory.size)
    {
        uint32_t sector_offset = position % ISO9660_SECTOR_SIZE;
        if (!read_sector(
                directory.lba + position / ISO9660_SECTOR_SIZE,
                sector_buffer))
            return false;

        uint8_t record_length = sector_buffer[sector_offset];
        if (record_length == 0)
        {
            position += ISO9660_SECTOR_SIZE - sector_offset;
            continue;
        }
        if (record_length < 34 ||
            record_length > ISO9660_SECTOR_SIZE - sector_offset ||
            record_length > directory.size - position)
            return false;

        iso9660_file_t entry;
        if (parse_record(
                sector_buffer + sector_offset,
                record_length,
                &entry))
        {
            if (index >= offset && written < capacity)
            {
                uint32_t name_index = 0;
                while (name_index < sizeof(entry.name) &&
                       entry.name[name_index])
                {
                    entries[written].name[name_index] =
                        entry.name[name_index];
                    name_index++;
                }
                entries[written].name[name_index] = '\0';
                entries[written].size = entry.size;
                entries[written].is_directory = entry.is_directory;
                written++;
            }
            index++;
        }
        position += record_length;
    }

    *total = index;
    return true;
}

/* Find a file using a path such as /BOOT/KERNEL.ELF */
bool iso9660_find(
    const char *path,
    iso9660_file_t *result
)
{
    if (!filesystem_ready || !path || !result)
        return false;

    while (*path == '/')
        path++;

    iso9660_file_t current = root_directory;

    if (*path == '\0')
    {
        *result = root_directory;
        return true;
    }

    while (*path)
    {
        char component[ISO9660_MAX_NAME];
        uint32_t length = 0;

        while (*path && *path != '/')
        {
            if (length >= ISO9660_MAX_NAME - 1)
                return false;

            component[length++] = *path++;
        }

        component[length] = '\0';

        while (*path == '/')
            path++;

        if (length == 0)
            continue;

        if (!find_child(&current, component, &current))
            return false;

        if (*path && !current.is_directory)
            return false;
    }

    *result = current;
    return true;
}

/* Read arbitrary bytes from a file. */
bool iso9660_read(
    const iso9660_file_t *file,
    uint32_t offset,
    uint32_t size,
    void *buffer
)
{
    if (!filesystem_ready ||
        !file ||
        !buffer ||
        file->is_directory)
        return false;

    if (offset > file->size ||
        size > file->size - offset)
        return false;

    uint8_t *output = (uint8_t *)buffer;

    while (size > 0)
    {
        uint32_t sector_number =
            offset / ISO9660_SECTOR_SIZE;

        uint32_t sector_offset =
            offset % ISO9660_SECTOR_SIZE;

        uint32_t lba = file->lba + sector_number;

        if (!read_sector(lba, sector_buffer))
            return false;

        uint32_t available =
            ISO9660_SECTOR_SIZE - sector_offset;

        uint32_t amount =
            size < available ? size : available;

        for (uint32_t i = 0; i < amount; i++)
            output[i] = sector_buffer[sector_offset + i];

        output += amount;
        offset += amount;
        size -= amount;
    }

    return true;
}
