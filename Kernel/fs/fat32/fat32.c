#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "block.h"
#include "fat32.h"
#include "serial.h"
#include "syscall.h"

#define FAT32_EOC_MIN 0x0FFFFFF8U
#define FAT32_BAD_CLUSTER 0x0FFFFFF7U
#define FAT32_RESERVED_MIN 0x0FFFFFF0U
#define FAT32_DIRECTORY_END 0x00
#define FAT32_DIRECTORY_DELETED 0xE5
#define FAT32_ATTRIBUTE_DIRECTORY 0x10
#define FAT32_ATTRIBUTE_VOLUME 0x08
#define FAT32_ATTRIBUTE_LONG_NAME 0x0F
#define FAT32_DIRECTORY_ENTRY_SIZE 32
#define FAT32_LFN_CHARACTERS 13
#define FAT32_CLUSTER_LIMIT 0x0FFFFFEFU
#define FAT32_FORMAT_WRITE_BATCH 8

static const char *last_format_error;

typedef struct
{
    char name[FAT32_NAME_MAX + 1];
    uint8_t expected_sequence;
    uint8_t checksum;
    bool valid;
} fat32_long_name_t;

typedef bool (*fat32_entry_callback_t)(
    const fat32_dirent_t *entry,
    void *context
);

static uint16_t read_le16(const uint8_t *bytes)
{
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static uint32_t read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
        ((uint32_t)bytes[1] << 8) |
        ((uint32_t)bytes[2] << 16) |
        ((uint32_t)bytes[3] << 24);
}

static void write_le16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void write_le32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static void bytes_zero(void *buffer, size_t size)
{
    uint8_t *bytes = buffer;
    for (size_t i = 0; i < size; i++)
        bytes[i] = 0;
}

static uint32_t divide_round_up(uint64_t numerator, uint32_t denominator)
{
    return (uint32_t)(numerator / denominator +
        (numerator % denominator != 0));
}

static bool format_write(
    uint32_t device_index,
    uint64_t lba,
    uint32_t count,
    const void *buffer,
    const char *operation
)
{
    if (block_write(device_index, lba, count, buffer))
        return true;

    last_format_error = operation;
    serial_log_begin(SERIAL_LOG_ERROR);
    serial_write("FAT32 format: ");
    serial_write(operation);
    serial_write(" at LBA ");
    serial_write_dec(lba);
    serial_write(" (sectors ");
    serial_write_dec(count);
    serial_write(")");
    serial_log_end();
    return false;
}

static bool format_zero_sectors(
    uint32_t device_index,
    uint64_t lba,
    uint32_t count,
    const char *operation
)
{
    static const uint8_t zero[
        FAT32_FORMAT_WRITE_BATCH * BLOCK_SECTOR_SIZE];
    while (count)
    {
        uint32_t batch = count > FAT32_FORMAT_WRITE_BATCH
            ? FAT32_FORMAT_WRITE_BATCH
            : count;
        if (!format_write(device_index, lba, batch, zero, operation))
            return false;
        lba += batch;
        count -= batch;
    }
    return true;
}

static bool string_equal(const char *left, const char *right)
{
    while (*left && *right)
    {
        char a = *left++;
        char b = *right++;
        if (a >= 'A' && a <= 'Z')
            a = (char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z')
            b = (char)(b + ('a' - 'A'));
        if (a != b)
            return false;
    }
    return *left == '\0' && *right == '\0';
}

static bool cluster_valid(const fat32_volume_t *volume, uint32_t cluster)
{
    return cluster >= 2 && cluster < volume->cluster_count + 2 &&
        cluster <= FAT32_CLUSTER_LIMIT;
}

static bool partition_read(
    const fat32_volume_t *volume,
    uint64_t relative_lba,
    void *buffer
)
{
    if (relative_lba >= volume->partition_sectors ||
        volume->partition_lba > UINT64_MAX - relative_lba)
        return false;
    return block_read(
        volume->device_index,
        volume->partition_lba + relative_lba,
        1,
        buffer);
}

static bool cluster_lba(
    const fat32_volume_t *volume,
    uint32_t cluster,
    uint32_t sector_in_cluster,
    uint64_t *lba
)
{
    if (!cluster_valid(volume, cluster) ||
        sector_in_cluster >= volume->sectors_per_cluster)
        return false;
    uint64_t first = volume->first_data_sector +
        (uint64_t)(cluster - 2) * volume->sectors_per_cluster;
    if (first > UINT64_MAX - sector_in_cluster)
        return false;
    first += sector_in_cluster;
    if (first >= volume->partition_sectors)
        return false;
    *lba = first;
    return true;
}

static bool fat_next_cluster(
    const fat32_volume_t *volume,
    uint32_t cluster,
    uint32_t *next,
    bool *end
)
{
    if (!cluster_valid(volume, cluster) || !next || !end)
        return false;
    uint64_t byte_offset = (uint64_t)cluster * sizeof(uint32_t);
    uint64_t fat_sector = volume->reserved_sectors +
        (uint64_t)volume->active_fat * volume->fat_sectors +
        byte_offset / BLOCK_SECTOR_SIZE;
    uint32_t offset = (uint32_t)(byte_offset % BLOCK_SECTOR_SIZE);
    uint8_t sector[BLOCK_SECTOR_SIZE];
    if (offset > BLOCK_SECTOR_SIZE - sizeof(uint32_t) ||
        !partition_read(volume, fat_sector, sector))
        return false;

    uint32_t value = read_le32(sector + offset) & 0x0FFFFFFFU;
    if (value >= FAT32_EOC_MIN)
    {
        *end = true;
        *next = 0;
        return true;
    }
    if (value == 0 || value == 1 || value == FAT32_BAD_CLUSTER ||
        (value >= FAT32_RESERVED_MIN && value < FAT32_EOC_MIN) ||
        !cluster_valid(volume, value))
        return false;
    *end = false;
    *next = value;
    return true;
}

static void reset_long_name(fat32_long_name_t *name)
{
    name->name[0] = '\0';
    name->expected_sequence = 0;
    name->checksum = 0;
    name->valid = false;
}

static void collect_long_name(
    fat32_long_name_t *name,
    const uint8_t entry[FAT32_DIRECTORY_ENTRY_SIZE]
)
{
    uint8_t sequence = entry[0] & 0x1F;
    if (entry[0] & 0x40)
    {
        for (size_t i = 0; i < sizeof(name->name); i++)
            name->name[i] = '\0';
        name->expected_sequence = sequence;
        name->checksum = entry[13];
        name->valid = sequence > 0 && sequence <= 20 &&
            entry[12] == 0 && read_le16(entry + 26) == 0;
    }
    if (!name->valid || sequence != name->expected_sequence ||
        entry[13] != name->checksum || entry[12] != 0 ||
        read_le16(entry + 26) != 0)
    {
        reset_long_name(name);
        return;
    }

    static const uint8_t offsets[FAT32_LFN_CHARACTERS] = {
        1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30
    };
    size_t base = (size_t)(sequence - 1) * FAT32_LFN_CHARACTERS;
    for (uint32_t i = 0; i < FAT32_LFN_CHARACTERS; i++)
    {
        uint16_t character = read_le16(entry + offsets[i]);
        size_t index = base + i;
        if (index >= FAT32_NAME_MAX)
            continue;
        if (character == 0 || character == 0xFFFF)
            name->name[index] = '\0';
        else
            name->name[index] = character < 0x80
                ? (char)character
                : '?';
    }
    name->expected_sequence--;
}

static uint8_t short_name_checksum(const uint8_t *name)
{
    uint8_t value = 0;
    for (uint32_t i = 0; i < 11; i++)
        value = (uint8_t)(((value & 1) << 7) + (value >> 1) + name[i]);
    return value;
}

static void short_name_to_text(
    const uint8_t *entry,
    char output[FAT32_NAME_MAX + 1]
)
{
    size_t length = 0;
    for (uint32_t i = 0; i < 8 && entry[i] != ' '; i++)
    {
        uint8_t character = entry[i];
        if (i == 0 && character == 0x05)
            character = 0xE5;
        output[length++] = character < 0x80 ? (char)character : '?';
    }
    if (entry[8] != ' ')
    {
        output[length++] = '.';
        for (uint32_t i = 8; i < 11 && entry[i] != ' '; i++)
            output[length++] = entry[i] < 0x80 ? (char)entry[i] : '?';
    }
    output[length] = '\0';
}

static bool is_dot_entry(const char *name)
{
    return (name[0] == '.' && name[1] == '\0') ||
        (name[0] == '.' && name[1] == '.' && name[2] == '\0');
}

static bool iterate_directory(
    fat32_volume_t *volume,
    uint32_t first_cluster,
    fat32_entry_callback_t callback,
    void *context
)
{
    if (!volume || !volume->mounted || !callback ||
        !cluster_valid(volume, first_cluster))
        return false;

    uint8_t sector[BLOCK_SECTOR_SIZE];
    fat32_long_name_t long_name;
    reset_long_name(&long_name);
    uint32_t cluster = first_cluster;
    for (uint32_t hops = 0; hops < volume->cluster_count; hops++)
    {
        for (uint32_t sector_index = 0;
             sector_index < volume->sectors_per_cluster;
             sector_index++)
        {
            uint64_t lba;
            if (!cluster_lba(volume, cluster, sector_index, &lba) ||
                !partition_read(volume, lba, sector))
                return false;
            for (uint32_t offset = 0;
                 offset < BLOCK_SECTOR_SIZE;
                 offset += FAT32_DIRECTORY_ENTRY_SIZE)
            {
                const uint8_t *raw = sector + offset;
                if (raw[0] == FAT32_DIRECTORY_END)
                    return true;
                if (raw[0] == FAT32_DIRECTORY_DELETED)
                {
                    reset_long_name(&long_name);
                    continue;
                }
                if (raw[11] == FAT32_ATTRIBUTE_LONG_NAME)
                {
                    collect_long_name(&long_name, raw);
                    continue;
                }

                fat32_dirent_t entry = {0};
                if (long_name.valid && !long_name.expected_sequence &&
                    long_name.checksum == short_name_checksum(raw))
                {
                    size_t i = 0;
                    while (i < FAT32_NAME_MAX && long_name.name[i])
                    {
                        entry.name[i] = long_name.name[i];
                        i++;
                    }
                    entry.name[i] = '\0';
                }
                else
                    short_name_to_text(raw, entry.name);
                reset_long_name(&long_name);

                if ((raw[11] & FAT32_ATTRIBUTE_VOLUME) ||
                    is_dot_entry(entry.name))
                    continue;
                entry.first_cluster =
                    ((uint32_t)read_le16(raw + 20) << 16) |
                    read_le16(raw + 26);
                entry.size = read_le32(raw + 28);
                entry.is_directory =
                    (raw[11] & FAT32_ATTRIBUTE_DIRECTORY) != 0;
                entry.entry_lba = lba;
                entry.entry_offset = (uint16_t)offset;
                if (!callback(&entry, context))
                    return true;
            }
        }

        uint32_t next;
        bool end;
        if (!fat_next_cluster(volume, cluster, &next, &end))
            return false;
        if (end)
            return true;
        cluster = next;
    }
    return false;
}

typedef struct
{
    const char *name;
    fat32_dirent_t result;
    bool found;
} find_context_t;

static bool find_entry(
    const fat32_dirent_t *entry,
    void *opaque
)
{
    find_context_t *context = opaque;
    if (string_equal(entry->name, context->name))
    {
        context->result = *entry;
        context->found = true;
        return false;
    }
    return true;
}

static bool find_in_directory(
    fat32_volume_t *volume,
    uint32_t directory_cluster,
    const char *name,
    fat32_dirent_t *result
)
{
    find_context_t context = {
        .name = name,
        .found = false
    };
    if (!iterate_directory(
            volume,
            directory_cluster,
            find_entry,
            &context))
        return false;
    if (!context.found)
        return false;
    *result = context.result;
    return true;
}

static bool lookup_path(
    fat32_volume_t *volume,
    const char *path,
    fat32_dirent_t *result
)
{
    if (!volume || !volume->mounted || !path || !result)
        return false;
    while (*path == '/')
        path++;
    *result = (fat32_dirent_t) {
        .first_cluster = volume->root_cluster,
        .is_directory = true
    };
    if (!*path)
        return true;

    char component[FAT32_NAME_MAX + 1];
    uint32_t current_cluster = volume->root_cluster;
    while (*path)
    {
        size_t length = 0;
        while (*path && *path != '/')
        {
            if (length >= FAT32_NAME_MAX)
                return false;
            component[length++] = *path++;
        }
        while (*path == '/')
            path++;
        if (!length || (length == 1 && component[0] == '.') ||
            (length == 2 && component[0] == '.' &&
             component[1] == '.'))
            return false;
        component[length] = '\0';

        fat32_dirent_t found;
        if (!find_in_directory(
                volume,
                current_cluster,
                component,
                &found))
            return false;
        if (*path && !found.is_directory)
            return false;
        *result = found;
        current_cluster = found.first_cluster;
    }
    return true;
}

bool fat32_mount(
    fat32_volume_t *volume,
    uint32_t device_index,
    uint64_t partition_lba,
    uint64_t partition_sectors,
    const char *mount_path
)
{
    if (!volume || !mount_path || mount_path[0] != '/' ||
        !partition_sectors)
        return false;
    const block_device_t *device = block_device_get(device_index);
    if (!device || partition_lba >= device->sector_count ||
        partition_sectors > device->sector_count - partition_lba)
        return false;

    uint8_t boot_sector[BLOCK_SECTOR_SIZE];
    if (!block_read(device_index, partition_lba, 1, boot_sector) ||
        boot_sector[510] != 0x55 || boot_sector[511] != 0xAA)
        return false;

    uint32_t bytes_per_sector = read_le16(boot_sector + 11);
    uint32_t sectors_per_cluster = boot_sector[13];
    uint32_t reserved_sectors = read_le16(boot_sector + 14);
    uint32_t fat_count = boot_sector[16];
    uint32_t root_entries = read_le16(boot_sector + 17);
    uint32_t total_sectors = read_le16(boot_sector + 19);
    if (!total_sectors)
        total_sectors = read_le32(boot_sector + 32);
    uint32_t fat_sectors = read_le32(boot_sector + 36);
    uint32_t root_cluster = read_le32(boot_sector + 44) & 0x0FFFFFFFU;
    uint32_t ext_flags = read_le16(boot_sector + 40);
    if (bytes_per_sector != BLOCK_SECTOR_SIZE ||
        !sectors_per_cluster ||
        (sectors_per_cluster & (sectors_per_cluster - 1)) ||
        sectors_per_cluster > 128 ||
        !reserved_sectors || !fat_count || fat_count > 2 ||
        root_entries || read_le16(boot_sector + 22) ||
        !fat_sectors || !total_sectors ||
        total_sectors > partition_sectors)
        return false;

    uint32_t active_fat = 0;
    if (ext_flags & 0x80)
    {
        active_fat = ext_flags & 0x0F;
        if (active_fat >= fat_count)
            return false;
    }
    uint64_t first_data_sector = reserved_sectors +
        (uint64_t)fat_count * fat_sectors;
    if (first_data_sector >= total_sectors)
        return false;
    uint32_t cluster_count =
        (uint32_t)((total_sectors - first_data_sector) /
            sectors_per_cluster);
    uint64_t fat_entries = (uint64_t)fat_sectors *
        BLOCK_SECTOR_SIZE / sizeof(uint32_t);
    if (!cluster_count || cluster_count > FAT32_CLUSTER_LIMIT - 1 ||
        fat_entries < (uint64_t)cluster_count + 2 ||
        root_cluster < 2 || root_cluster >= cluster_count + 2)
        return false;

    *volume = (fat32_volume_t) {
        .device_index = device_index,
        .partition_lba = partition_lba,
        .partition_sectors = total_sectors,
        .sectors_per_cluster = sectors_per_cluster,
        .reserved_sectors = reserved_sectors,
        .fat_sectors = fat_sectors,
        .first_data_sector = (uint32_t)first_data_sector,
        .cluster_count = cluster_count,
        .root_cluster = root_cluster,
        .active_fat = active_fat,
        .fat_count = fat_count,
        .next_free_cluster = 2,
        .free_clusters = UINT32_MAX,
        .fs_info_sector = read_le16(boot_sector + 48),
        .backup_boot_sector = read_le16(boot_sector + 50),
        .fat_mirroring = (ext_flags & 0x80) == 0,
        .mounted = true,
        .writable = device->writable
    };
    size_t i = 0;
    while (i < FAT32_VOLUME_NAME_MAX - 1 && mount_path[i])
    {
        volume->mount_path[i] = mount_path[i];
        i++;
    }
    if (mount_path[i])
    {
        volume->mounted = false;
        return false;
    }
    volume->mount_path[i] = '\0';

    if (volume->fs_info_sector &&
        volume->fs_info_sector < volume->reserved_sectors)
    {
        uint8_t fs_info[BLOCK_SECTOR_SIZE];
        if (partition_read(volume, volume->fs_info_sector, fs_info) &&
            read_le32(fs_info) == 0x41615252U &&
            read_le32(fs_info + 484) == 0x61417272U &&
            read_le32(fs_info + 508) == 0xAA550000U)
        {
            uint32_t free_clusters = read_le32(fs_info + 488);
            if (free_clusters == UINT32_MAX ||
                free_clusters <= volume->cluster_count)
                volume->free_clusters = free_clusters;
            uint32_t next_free = read_le32(fs_info + 492);
            if (next_free >= 2 &&
                next_free < volume->cluster_count + 2)
                volume->next_free_cluster = next_free;
        }
    }

    uint32_t next;
    bool end;
    if (!fat_next_cluster(volume, volume->root_cluster, &next, &end))
    {
        volume->mounted = false;
        return false;
    }
    (void)next;
    (void)end;
    return true;
}

void fat32_unmount(fat32_volume_t *volume)
{
    if (volume)
        volume->mounted = false;
}

bool fat32_path_matches(
    const fat32_volume_t *volume,
    const char *path,
    const char **relative_path
)
{
    if (!volume || !volume->mounted || !path ||
        !volume->mount_path[0])
        return false;
    size_t length = 0;
    while (volume->mount_path[length])
        length++;
    for (size_t i = 0; i < length; i++)
    {
        char a = path[i];
        char b = volume->mount_path[i];
        if (!a)
            return false;
        if (a >= 'A' && a <= 'Z')
            a = (char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z')
            b = (char)(b + ('a' - 'A'));
        if (a != b)
            return false;
    }
    if (path[length] && path[length] != '/')
        return false;
    if (relative_path)
        *relative_path = path + length;
    return true;
}

bool fat32_is_directory(fat32_volume_t *volume, const char *path)
{
    fat32_dirent_t entry;
    return lookup_path(volume, path, &entry) && entry.is_directory;
}

int64_t fat32_open(
    fat32_volume_t *volume,
    const char *path,
    fat32_file_t *file
)
{
    if (!volume || !volume->mounted || !path || !file)
        return -1;
    fat32_dirent_t entry;
    if (!lookup_path(volume, path, &entry))
        return -1;
    *file = (fat32_file_t) {
        .volume = volume,
        .first_cluster = entry.first_cluster,
        .size = entry.size,
        .entry_lba = entry.entry_lba,
        .entry_offset = entry.entry_offset,
        .is_directory = entry.is_directory
    };
    return entry.is_directory ? -1 : 0;
}

int64_t fat32_read(fat32_file_t *file, void *buffer, uint64_t size)
{
    if (!file || !file->volume || !file->volume->mounted ||
        file->is_directory || (!buffer && size))
        return -1;
    if (file->offset >= file->size || !size)
        return 0;
    if (size > file->size - file->offset)
        size = file->size - file->offset;
    if (!file->first_cluster)
        return -1;

    fat32_volume_t *volume = file->volume;
    uint64_t cluster_size =
        (uint64_t)volume->sectors_per_cluster * BLOCK_SECTOR_SIZE;
    uint64_t total = 0;
    uint8_t sector[BLOCK_SECTOR_SIZE];
    uint64_t cluster_index = 0;
    uint32_t cluster = file->first_cluster;
    uint64_t target_cluster = file->offset / cluster_size;
    if (target_cluster >= volume->cluster_count)
        return -1;
    while (cluster_index < target_cluster)
    {
        uint32_t next;
        bool end;
        if (!fat_next_cluster(volume, cluster, &next, &end) || end)
            return -1;
        cluster = next;
        cluster_index++;
    }
    while (total < size)
    {
        uint64_t position = file->offset + total;
        uint64_t target = position / cluster_size;
        if (target != cluster_index)
        {
            uint32_t next;
            bool end;
            if (target != cluster_index + 1 ||
                !fat_next_cluster(volume, cluster, &next, &end) || end)
                return total ? (int64_t)total : -1;
            cluster = next;
            cluster_index++;
        }
        uint32_t cluster_offset = (uint32_t)(position % cluster_size);

        uint32_t sector_index = cluster_offset / BLOCK_SECTOR_SIZE;
        uint32_t sector_offset = cluster_offset % BLOCK_SECTOR_SIZE;
        uint64_t lba;
        if (!cluster_lba(volume, cluster, sector_index, &lba) ||
            !partition_read(volume, lba, sector))
            return total ? (int64_t)total : -1;
        uint64_t amount = BLOCK_SECTOR_SIZE - sector_offset;
        if (amount > size - total)
            amount = size - total;
        for (uint64_t i = 0; i < amount; i++)
            ((uint8_t *)buffer)[total + i] = sector[sector_offset + i];
        total += amount;
    }
    file->offset += total;
    return (int64_t)total;
}

typedef struct
{
    uint64_t offset;
    uint64_t capacity;
    uint64_t count;
    fat32_dirent_t *entries;
} list_context_t;

static bool list_entry(const fat32_dirent_t *entry, void *opaque)
{
    list_context_t *context = opaque;
    if (context->count >= context->capacity)
        return false;
    if (context->offset)
    {
        context->offset--;
        return true;
    }
    context->entries[context->count++] = *entry;
    return context->count < context->capacity;
}

int64_t fat32_list(
    fat32_volume_t *volume,
    const char *path,
    uint64_t offset,
    fat32_dirent_t *entries,
    uint64_t capacity
)
{
    if (!volume || !volume->mounted || !path ||
        (capacity && !entries))
        return -1;
    fat32_dirent_t directory;
    if (!lookup_path(volume, path, &directory) ||
        !directory.is_directory)
        return -1;
    list_context_t context = {
        .offset = offset,
        .capacity = capacity,
        .count = 0,
        .entries = entries
    };
    if (!iterate_directory(
            volume,
            directory.first_cluster,
            list_entry,
            &context))
        return context.count ? (int64_t)context.count : -1;
    return (int64_t)context.count;
}

static bool partition_write(
    const fat32_volume_t *volume,
    uint64_t relative_lba,
    const void *buffer
)
{
    if (!volume || !volume->writable ||
        relative_lba >= volume->partition_sectors ||
        volume->partition_lba > UINT64_MAX - relative_lba)
        return false;
    return block_write(
        volume->device_index,
        volume->partition_lba + relative_lba,
        1,
        buffer);
}

static bool fat32_update_fs_info(const fat32_volume_t *volume)
{
    if (!volume || volume->free_clusters == UINT32_MAX ||
        !volume->fs_info_sector ||
        volume->fs_info_sector >= volume->reserved_sectors)
        return true;
    uint8_t sector[BLOCK_SECTOR_SIZE];
    if (!partition_read(volume, volume->fs_info_sector, sector) ||
        read_le32(sector) != 0x41615252U ||
        read_le32(sector + 484) != 0x61417272U ||
        read_le32(sector + 508) != 0xAA550000U)
        return false;
    write_le32(sector + 488, volume->free_clusters);
    write_le32(sector + 492, volume->next_free_cluster);
    if (!partition_write(volume, volume->fs_info_sector, sector))
        return false;

    uint64_t backup_info =
        (uint64_t)volume->backup_boot_sector + volume->fs_info_sector;
    if (volume->backup_boot_sector &&
        volume->backup_boot_sector < volume->reserved_sectors &&
        backup_info < volume->reserved_sectors)
    {
        if (!partition_read(volume, backup_info, sector) ||
            read_le32(sector) != 0x41615252U ||
            read_le32(sector + 484) != 0x61417272U ||
            read_le32(sector + 508) != 0xAA550000U)
            return false;
        write_le32(sector + 488, volume->free_clusters);
        write_le32(sector + 492, volume->next_free_cluster);
        if (!partition_write(volume, backup_info, sector))
            return false;
    }
    return true;
}

static bool fat_write_entry(
    fat32_volume_t *volume,
    uint32_t cluster,
    uint32_t value
)
{
    if (!volume || !volume->writable ||
        !cluster_valid(volume, cluster))
        return false;
    uint64_t byte_offset = (uint64_t)cluster * sizeof(uint32_t);
    uint64_t sector_offset = byte_offset / BLOCK_SECTOR_SIZE;
    uint32_t offset = (uint32_t)(byte_offset % BLOCK_SECTOR_SIZE);
    uint32_t first_fat = volume->fat_mirroring ? 0 : volume->active_fat;
    uint32_t fat_limit = volume->fat_mirroring
        ? volume->fat_count
        : volume->active_fat + 1;
    for (uint32_t fat = first_fat; fat < fat_limit; fat++)
    {
        uint8_t sector[BLOCK_SECTOR_SIZE];
        uint64_t lba = volume->reserved_sectors +
            (uint64_t)fat * volume->fat_sectors + sector_offset;
        if (offset > BLOCK_SECTOR_SIZE - sizeof(uint32_t) ||
            !partition_read(volume, lba, sector))
            return false;
        write_le32(sector + offset, value & 0x0FFFFFFFU);
        if (!partition_write(volume, lba, sector))
            return false;
    }
    return true;
}

static bool fat_get_value(
    const fat32_volume_t *volume,
    uint32_t cluster,
    uint32_t *value
)
{
    if (!volume || !value || !cluster_valid(volume, cluster))
        return false;
    uint64_t byte_offset = (uint64_t)cluster * sizeof(uint32_t);
    uint64_t lba = volume->reserved_sectors +
        (uint64_t)volume->active_fat * volume->fat_sectors +
        byte_offset / BLOCK_SECTOR_SIZE;
    uint32_t offset = (uint32_t)(byte_offset % BLOCK_SECTOR_SIZE);
    uint8_t sector[BLOCK_SECTOR_SIZE];
    if (offset > BLOCK_SECTOR_SIZE - sizeof(uint32_t) ||
        !partition_read(volume, lba, sector))
        return false;
    *value = read_le32(sector + offset) & 0x0FFFFFFFU;
    return true;
}

static bool fat32_write_cluster(
    fat32_volume_t *volume,
    uint32_t cluster,
    uint32_t sector_index,
    const uint8_t sector[BLOCK_SECTOR_SIZE]
)
{
    uint64_t lba;
    return cluster_lba(volume, cluster, sector_index, &lba) &&
        partition_write(volume, lba, sector);
}

static bool fat32_allocate_cluster(
    fat32_volume_t *volume,
    uint32_t *allocated_cluster
)
{
    if (!volume || !volume->mounted || !volume->writable ||
        !allocated_cluster)
        return false;
    uint32_t first = volume->next_free_cluster;
    if (first < 2 || first >= volume->cluster_count + 2)
        first = 2;
    for (uint32_t checked = 0; checked < volume->cluster_count; checked++)
    {
        uint32_t cluster = 2 +
            (first - 2 + checked) % volume->cluster_count;
        uint32_t value;
        if (!fat_get_value(volume, cluster, &value))
            return false;
        if (value)
            continue;
        if (!fat_write_entry(volume, cluster, 0x0FFFFFFFU))
            return false;
        uint8_t zero[BLOCK_SECTOR_SIZE] = {0};
        for (uint32_t sector = 0;
             sector < volume->sectors_per_cluster;
             sector++)
        {
            if (!fat32_write_cluster(volume, cluster, sector, zero))
            {
                (void)fat_write_entry(volume, cluster, 0);
                return false;
            }
        }
        volume->next_free_cluster = cluster + 1;
        if (volume->next_free_cluster >= volume->cluster_count + 2)
            volume->next_free_cluster = 2;
        if (volume->free_clusters != UINT32_MAX)
        {
            if (!volume->free_clusters)
            {
                (void)fat_write_entry(volume, cluster, 0);
                return false;
            }
            volume->free_clusters--;
            if (!fat32_update_fs_info(volume))
            {
                volume->free_clusters++;
                (void)fat_write_entry(volume, cluster, 0);
                return false;
            }
        }
        *allocated_cluster = cluster;
        return true;
    }
    return false;
}

static bool fat32_free_chain(fat32_volume_t *volume, uint32_t first_cluster)
{
    if (!first_cluster)
        return true;
    uint32_t cluster = first_cluster;
    uint32_t first_freed_cluster = first_cluster;
    uint32_t freed_clusters = 0;
    for (uint32_t hops = 0; hops < volume->cluster_count; hops++)
    {
        uint32_t next;
        bool end;
        if (!fat_next_cluster(volume, cluster, &next, &end) ||
            !fat_write_entry(volume, cluster, 0))
            return false;
        freed_clusters++;
        if (end)
        {
            if (volume->next_free_cluster < 2 ||
                first_freed_cluster < volume->next_free_cluster)
                volume->next_free_cluster = first_freed_cluster;
            if (volume->free_clusters != UINT32_MAX)
            {
                if (freed_clusters >
                    volume->cluster_count - volume->free_clusters)
                    return false;
                volume->free_clusters += freed_clusters;
            }
            return fat32_update_fs_info(volume);
        }
        cluster = next;
    }
    return false;
}

static bool fat32_cluster_for_index(
    fat32_volume_t *volume,
    uint32_t first_cluster,
    uint64_t target_index,
    uint32_t *target_cluster
)
{
    if (!volume || !target_cluster ||
        !cluster_valid(volume, first_cluster) ||
        target_index >= volume->cluster_count)
        return false;
    uint32_t cluster = first_cluster;
    for (uint64_t index = 0; index < target_index; index++)
    {
        uint32_t next;
        bool end;
        if (!fat_next_cluster(volume, cluster, &next, &end) || end)
            return false;
        cluster = next;
    }
    *target_cluster = cluster;
    return true;
}

typedef struct
{
    uint64_t lba;
    uint16_t offset;
} fat32_slot_t;

static uint8_t short_name_checksum(const uint8_t *name);

static bool fat32_insert_directory_entry(
    fat32_volume_t *volume,
    uint32_t directory_cluster,
    const char *name,
    uint32_t first_cluster,
    uint32_t size,
    uint8_t attributes,
    fat32_dirent_t *created_entry
)
{
    uint8_t short_name[11];
    bool needs_long_name = false;
    size_t name_length = 0;
    while (name[name_length])
        name_length++;
    if (!name_length || name_length > FAT32_NAME_MAX)
        return false;
    for (size_t i = 0; i < sizeof(short_name); i++)
        short_name[i] = ' ';

    size_t dot = name_length;
    for (size_t i = 0; i < name_length; i++)
    {
        if (name[i] == '.')
            dot = i;
        if ((uint8_t)name[i] < 0x20 || name[i] == '/' ||
            name[i] == '\\' || name[i] == '"' || name[i] == '*' ||
            name[i] == ':' || name[i] == '<' || name[i] == '>' ||
            name[i] == '?' || name[i] == '|' || name[i] == '+')
            return false;
    }
    size_t base_length = dot;
    size_t extension_length = dot < name_length
        ? name_length - dot - 1
        : 0;
    bool short_valid = base_length > 0 && base_length <= 8 &&
        extension_length <= 3;
    for (size_t i = 0; i < base_length && short_valid; i++)
    {
        char character = name[i];
        if (character >= 'a' && character <= 'z')
        {
            needs_long_name = true;
            character = (char)(character - ('a' - 'A'));
        }
        if (!((character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') ||
              character == '_' || character == '-'))
            short_valid = false;
        if (i < 8)
            short_name[i] = (uint8_t)character;
    }
    for (size_t i = 0; i < extension_length && short_valid; i++)
    {
        char character = name[dot + 1 + i];
        if (character >= 'a' && character <= 'z')
        {
            needs_long_name = true;
            character = (char)(character - ('a' - 'A'));
        }
        if (!((character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') ||
              character == '_' || character == '-'))
            short_valid = false;
        if (i < 3)
            short_name[8 + i] = (uint8_t)character;
    }
    if (!short_valid)
        needs_long_name = true;
    if (needs_long_name)
    {
        for (size_t i = 0; i < sizeof(short_name); i++)
            short_name[i] = ' ';
        size_t alias_base = base_length < 6 ? base_length : 6;
        if (!alias_base)
            return false;
        for (size_t i = 0; i < alias_base; i++)
        {
            char character = name[i];
            if (character >= 'a' && character <= 'z')
                character = (char)(character - ('a' - 'A'));
            if (!((character >= 'A' && character <= 'Z') ||
                  (character >= '0' && character <= '9') ||
                  character == '_' || character == '-'))
                character = '_';
            short_name[i] = (uint8_t)character;
        }
        short_name[alias_base++] = '~';
        short_name[alias_base] = '1';
        size_t alias_extension =
            extension_length < 3 ? extension_length : 3;
        for (size_t i = 0; i < alias_extension; i++)
        {
            char character = name[dot + 1 + i];
            if (character >= 'a' && character <= 'z')
                character = (char)(character - ('a' - 'A'));
            short_name[8 + i] =
                (character >= 'A' && character <= 'Z') ||
                (character >= '0' && character <= '9')
                    ? (uint8_t)character
                    : '_';
        }
    }
    else if (needs_long_name)
    {
        return false;
    }
    else
    {
        for (size_t i = 0; i < name_length; i++)
        {
            if (name[i] >= 'a' && name[i] <= 'z')
                needs_long_name = true;
        }
    }

    uint32_t lfn_count = needs_long_name
        ? (uint32_t)((name_length + 13) / 13)
        : 0;
    if (lfn_count > 20)
        return false;
    uint32_t slot_count = lfn_count + 1;
    fat32_slot_t slots[21];
    uint32_t free_count = 0;
    bool past_end = false;
    uint8_t sector[BLOCK_SECTOR_SIZE];
    uint32_t cluster = directory_cluster;
    for (uint32_t hops = 0; hops <= volume->cluster_count; hops++)
    {
        for (uint32_t sector_index = 0;
             sector_index < volume->sectors_per_cluster;
             sector_index++)
        {
            uint64_t lba;
            if (!cluster_lba(volume, cluster, sector_index, &lba) ||
                !partition_read(volume, lba, sector))
                return false;
            for (uint32_t offset = 0;
                 offset < BLOCK_SECTOR_SIZE;
                 offset += FAT32_DIRECTORY_ENTRY_SIZE)
            {
                bool free_slot = past_end ||
                    sector[offset] == FAT32_DIRECTORY_END ||
                    sector[offset] == FAT32_DIRECTORY_DELETED;
                if (sector[offset] == FAT32_DIRECTORY_END)
                    past_end = true;
                if (!free_slot)
                {
                    free_count = 0;
                    continue;
                }
                slots[free_count++] = (fat32_slot_t) {
                    .lba = lba,
                    .offset = (uint16_t)offset
                };
                if (free_count == slot_count)
                {
                    uint8_t raw_entries[21][FAT32_DIRECTORY_ENTRY_SIZE];
                    for (uint32_t index = 0; index < lfn_count; index++)
                    {
                        uint32_t sequence = lfn_count - index;
                        uint8_t *raw = raw_entries[index];
                        for (size_t i = 0; i < FAT32_DIRECTORY_ENTRY_SIZE; i++)
                            raw[i] = 0;
                        raw[0] = (uint8_t)sequence;
                        if (sequence == lfn_count)
                            raw[0] |= 0x40;
                        raw[11] = FAT32_ATTRIBUTE_LONG_NAME;
                        raw[13] = short_name_checksum(short_name);
                        static const uint8_t offsets[] = {
                            1, 3, 5, 7, 9, 14, 16, 18, 20, 22,
                            24, 28, 30
                        };
                        for (uint32_t character = 0; character < 13; character++)
                        {
                            uint32_t name_index =
                                (sequence - 1) * 13 + character;
                            uint16_t value = name_index < name_length
                                ? (uint8_t)name[name_index]
                                : name_index == name_length
                                    ? 0
                                    : 0xFFFF;
                            write_le16(raw + offsets[character], value);
                        }
                    }
                    uint8_t *short_entry = raw_entries[lfn_count];
                    for (size_t i = 0; i < FAT32_DIRECTORY_ENTRY_SIZE; i++)
                        short_entry[i] = 0;
                    for (uint32_t i = 0; i < sizeof(short_name); i++)
                        short_entry[i] = short_name[i];
                    short_entry[11] = attributes;
                    write_le16(short_entry + 20, (uint16_t)(first_cluster >> 16));
                    write_le16(short_entry + 26, (uint16_t)first_cluster);
                    write_le32(short_entry + 28, size);

                    for (uint32_t index = 0; index < slot_count; index++)
                    {
                        if (!partition_read(
                                volume, slots[index].lba, sector))
                            return false;
                        for (uint32_t byte = 0;
                             byte < FAT32_DIRECTORY_ENTRY_SIZE;
                             byte++)
                            sector[slots[index].offset + byte] =
                                raw_entries[index][byte];
                        if (!partition_write(
                                volume, slots[index].lba, sector))
                            return false;
                    }
                    if (created_entry)
                    {
                        bytes_zero(created_entry, sizeof(*created_entry));
                        for (size_t i = 0; i < name_length; i++)
                            created_entry->name[i] = name[i];
                        created_entry->name[name_length] = '\0';
                        created_entry->size = size;
                        created_entry->first_cluster = first_cluster;
                        created_entry->entry_lba = slots[slot_count - 1].lba;
                        created_entry->entry_offset =
                            slots[slot_count - 1].offset;
                        created_entry->is_directory =
                            (attributes & FAT32_ATTRIBUTE_DIRECTORY) != 0;
                    }
                    return true;
                }
            }
        }
        uint32_t next;
        bool end;
        if (!fat_next_cluster(volume, cluster, &next, &end))
            return false;
        if (end)
        {
            uint32_t new_cluster;
            if (!fat32_allocate_cluster(volume, &new_cluster) ||
                !fat_write_entry(volume, cluster, new_cluster))
                return false;
            cluster = new_cluster;
        }
        else
            cluster = next;
    }
    return false;
}

bool fat32_format(
    uint32_t device_index,
    uint64_t partition_lba,
    uint64_t partition_sectors,
    const char *volume_label
)
{
    last_format_error = NULL;
    const block_device_t *device = block_device_get(device_index);
    if (!device || !device->writable ||
        partition_lba >= device->sector_count ||
        partition_sectors > device->sector_count - partition_lba ||
        partition_sectors > UINT32_MAX || partition_sectors < 65536)
    {
        last_format_error = "invalid FAT32 partition range";
        return false;
    }
    uint32_t total_sectors = (uint32_t)partition_sectors;
    uint32_t sectors_per_cluster =
        total_sectors < 1048576 ? 1 : 8;
    uint32_t reserved_sectors = 32;
    uint32_t fat_sectors = 1;
    uint32_t cluster_count = 0;
    for (uint32_t iteration = 0; iteration < 16; iteration++)
    {
        uint64_t data_sectors = total_sectors - reserved_sectors -
            2ULL * fat_sectors;
        cluster_count = (uint32_t)(data_sectors / sectors_per_cluster);
        uint32_t needed_fat_sectors = divide_round_up(
            (cluster_count + 2) * sizeof(uint32_t),
            BLOCK_SECTOR_SIZE);
        if (needed_fat_sectors == fat_sectors)
            break;
        fat_sectors = needed_fat_sectors;
    }
    if (cluster_count < 65525 ||
        cluster_count > FAT32_CLUSTER_LIMIT - 1 ||
        (uint64_t)reserved_sectors + 2ULL * fat_sectors >= total_sectors)
    {
        last_format_error = "unsupported FAT32 partition geometry";
        return false;
    }

    if (!format_zero_sectors(
            device_index,
            partition_lba,
            reserved_sectors,
            "Could not clear FAT32 reserved sectors"))
        return false;
    for (uint32_t fat = 0; fat < 2; fat++)
    {
        uint64_t fat_lba = partition_lba + reserved_sectors +
            (uint64_t)fat * fat_sectors;
        if (!format_zero_sectors(
                device_index,
                fat_lba,
                fat_sectors,
                "Could not clear FAT32 FAT sectors"))
            return false;
        uint8_t first_fat_sector[BLOCK_SECTOR_SIZE] = {0};
        write_le32(first_fat_sector, 0x0FFFFFF8U);
        write_le32(first_fat_sector + 4, 0xFFFFFFFFU);
        write_le32(first_fat_sector + 8, 0x0FFFFFFFU);
        if (!format_write(
                device_index,
                fat_lba,
                1,
                first_fat_sector,
                "Could not initialize FAT32 FAT"))
            return false;
    }

    uint8_t boot_sector[BLOCK_SECTOR_SIZE] = {0};
    boot_sector[0] = 0xEB;
    boot_sector[1] = 0x58;
    boot_sector[2] = 0x90;
    boot_sector[3] = 'V';
    boot_sector[4] = 'E';
    boot_sector[5] = 'R';
    boot_sector[6] = 'S';
    boot_sector[7] = 'O';
    boot_sector[8] = 'S';
    write_le16(boot_sector + 11, BLOCK_SECTOR_SIZE);
    boot_sector[13] = (uint8_t)sectors_per_cluster;
    write_le16(boot_sector + 14, (uint16_t)reserved_sectors);
    boot_sector[16] = 2;
    write_le16(boot_sector + 17, 0);
    write_le16(boot_sector + 19, 0);
    boot_sector[21] = 0xF8;
    write_le16(boot_sector + 22, 0);
    write_le32(boot_sector + 28, (uint32_t)partition_lba);
    write_le32(boot_sector + 32, total_sectors);
    write_le32(boot_sector + 36, fat_sectors);
    write_le16(boot_sector + 40, 0);
    write_le16(boot_sector + 42, 0);
    write_le32(boot_sector + 44, 2);
    write_le16(boot_sector + 48, 1);
    write_le16(boot_sector + 50, 6);
    boot_sector[64] = 0x80;
    boot_sector[66] = 0x29;
    write_le32(boot_sector + 67, 0x5652534FU);
    for (uint32_t i = 0; i < 11; i++)
        boot_sector[71 + i] = ' ';
    if (volume_label)
    {
        for (uint32_t i = 0; i < 11 && volume_label[i]; i++)
        {
            char character = volume_label[i];
            if (character >= 'a' && character <= 'z')
                character = (char)(character - ('a' - 'A'));
            boot_sector[71 + i] = (uint8_t)character;
        }
    }
    static const char fat_type[] = "FAT32   ";
    for (uint32_t i = 0; i < sizeof(fat_type) - 1; i++)
        boot_sector[82 + i] = (uint8_t)fat_type[i];
    boot_sector[510] = 0x55;
    boot_sector[511] = 0xAA;
    if (!format_write(
            device_index,
            partition_lba,
            1,
            boot_sector,
            "Could not write FAT32 primary boot sector") ||
        !format_write(
            device_index,
            partition_lba + 6,
            1,
            boot_sector,
            "Could not write FAT32 backup boot sector"))
        return false;

    uint8_t fs_info[BLOCK_SECTOR_SIZE] = {0};
    write_le32(fs_info, 0x41615252U);
    write_le32(fs_info + 484, 0x61417272U);
    write_le32(fs_info + 488, cluster_count - 1);
    write_le32(fs_info + 492, 3);
    write_le32(fs_info + 508, 0xAA550000U);
    if (!format_write(
            device_index,
            partition_lba + 1,
            1,
            fs_info,
            "Could not write primary FAT32 FSInfo sector") ||
        !format_write(
            device_index,
            partition_lba + 7,
            1,
            fs_info,
            "Could not write backup FAT32 FSInfo sector"))
        return false;

    uint64_t first_data_sector =
        reserved_sectors + 2ULL * fat_sectors;
    uint8_t root_sector[BLOCK_SECTOR_SIZE] = {0};
    for (uint32_t i = 0; i < 11; i++)
        root_sector[i] = boot_sector[71 + i];
    root_sector[11] = FAT32_ATTRIBUTE_VOLUME;
    if (!format_write(
            device_index,
            partition_lba + first_data_sector,
            1,
            root_sector,
            "Could not initialize FAT32 root directory cluster") ||
        !format_zero_sectors(
            device_index,
            partition_lba + first_data_sector + 1,
            sectors_per_cluster - 1,
            "Could not clear FAT32 root directory cluster"))
        return false;
    return true;
}

const char *fat32_format_error(void)
{
    return last_format_error;
}

static bool fat32_update_short_entry(
    fat32_file_t *file,
    uint32_t first_cluster,
    uint32_t size
)
{
    uint8_t sector[BLOCK_SECTOR_SIZE];
    if (!file || !file->volume ||
        !partition_read(file->volume, file->entry_lba, sector) ||
        file->entry_offset > BLOCK_SECTOR_SIZE -
            FAT32_DIRECTORY_ENTRY_SIZE)
        return false;
    uint8_t *entry = sector + file->entry_offset;
    write_le16(entry + 20, (uint16_t)(first_cluster >> 16));
    write_le16(entry + 26, (uint16_t)first_cluster);
    write_le32(entry + 28, size);
    return partition_write(file->volume, file->entry_lba, sector);
}

int64_t fat32_open_write(
    fat32_volume_t *volume,
    const char *path,
    bool create,
    bool truncate,
    fat32_file_t *file
)
{
    if (!volume || !volume->mounted || !volume->writable ||
        !path || !file)
        return -SYSCALL_EINVAL;
    fat32_dirent_t entry;
    if (lookup_path(volume, path, &entry))
    {
        if (entry.is_directory)
            return -SYSCALL_EISDIR;
        *file = (fat32_file_t) {
            .volume = volume,
            .first_cluster = entry.first_cluster,
            .size = entry.size,
            .entry_lba = entry.entry_lba,
            .entry_offset = entry.entry_offset
        };
        if (truncate)
        {
            if (!fat32_free_chain(volume, file->first_cluster) ||
                !fat32_update_short_entry(file, 0, 0))
                return -SYSCALL_EIO;
            file->first_cluster = 0;
            file->size = 0;
        }
        return 0;
    }
    if (!create)
        return -SYSCALL_ENOENT;

    const char *start = path;
    while (*start == '/')
        start++;
    const char *last = start;
    for (const char *cursor = start; *cursor; cursor++)
    {
        if (*cursor == '/')
            last = cursor + 1;
    }
    size_t name_length = 0;
    while (last[name_length])
        name_length++;
    if (!name_length || name_length > FAT32_NAME_MAX)
        return -SYSCALL_EINVAL;
    char name[FAT32_NAME_MAX + 1];
    for (size_t i = 0; i < name_length; i++)
        name[i] = last[i];
    name[name_length] = '\0';
    size_t parent_length = (size_t)(last - start);
    while (parent_length && start[parent_length - 1] == '/')
        parent_length--;
    char parent_path[FAT32_NAME_MAX + 1];
    if (parent_length > FAT32_NAME_MAX)
        return -SYSCALL_ENAMETOOLONG;
    for (size_t i = 0; i < parent_length; i++)
        parent_path[i] = start[i];
    parent_path[parent_length] = '\0';
    fat32_dirent_t parent;
    if (!lookup_path(volume, parent_path, &parent) ||
        !parent.is_directory)
        return -SYSCALL_ENOENT;
    if (!fat32_insert_directory_entry(
            volume,
            parent.first_cluster,
            name,
            0,
            0,
            0x20,
            &entry))
        return -SYSCALL_ENOSPC;
    *file = (fat32_file_t) {
        .volume = volume,
        .first_cluster = 0,
        .size = 0,
        .entry_lba = entry.entry_lba,
        .entry_offset = entry.entry_offset
    };
    return 0;
}

int64_t fat32_write(
    fat32_file_t *file,
    const void *buffer,
    uint64_t size
)
{
    if (!file || !file->volume || !file->volume->mounted ||
        !file->volume->writable || file->is_directory ||
        (!buffer && size))
        return -SYSCALL_EINVAL;
    if (!size)
        return 0;
    if (file->offset > UINT32_MAX ||
        size > UINT32_MAX - file->offset)
        return -SYSCALL_ENOSPC;
    fat32_volume_t *volume = file->volume;
    uint64_t cluster_size =
        (uint64_t)volume->sectors_per_cluster * BLOCK_SECTOR_SIZE;
    uint64_t written = 0;
    while (written < size)
    {
        uint64_t position = file->offset + written;
        uint64_t cluster_index = position / cluster_size;
        uint32_t cluster;
        if (!file->first_cluster)
        {
            if (!fat32_allocate_cluster(volume, &file->first_cluster))
                return written ? (int64_t)written : -SYSCALL_ENOSPC;
            if (!fat32_update_short_entry(
                    file, file->first_cluster, (uint32_t)file->size))
                return written ? (int64_t)written : -SYSCALL_EIO;
        }
        if (!fat32_cluster_for_index(
                volume,
                file->first_cluster,
                cluster_index,
                &cluster))
        {
            uint32_t last_cluster;
            if (!fat32_cluster_for_index(
                    volume,
                    file->first_cluster,
                    cluster_index - 1,
                    &last_cluster))
                return written ? (int64_t)written : -SYSCALL_EIO;
            uint32_t next_cluster;
            if (!fat32_allocate_cluster(volume, &next_cluster) ||
                !fat_write_entry(volume, last_cluster, next_cluster))
                return written ? (int64_t)written : -SYSCALL_ENOSPC;
            cluster = next_cluster;
        }
        uint32_t cluster_offset = (uint32_t)(position % cluster_size);
        uint32_t sector_index = cluster_offset / BLOCK_SECTOR_SIZE;
        uint32_t sector_offset = cluster_offset % BLOCK_SECTOR_SIZE;
        uint64_t amount = BLOCK_SECTOR_SIZE - sector_offset;
        if (amount > size - written)
            amount = size - written;
        uint64_t lba;
        uint8_t sector[BLOCK_SECTOR_SIZE];
        if (!cluster_lba(volume, cluster, sector_index, &lba) ||
            !partition_read(volume, lba, sector))
            return written ? (int64_t)written : -SYSCALL_EIO;
        for (uint64_t i = 0; i < amount; i++)
            sector[sector_offset + i] =
                ((const uint8_t *)buffer)[written + i];
        if (!partition_write(volume, lba, sector))
            return written ? (int64_t)written : -SYSCALL_EIO;
        written += amount;
    }
    file->offset += written;
    if (file->offset > file->size)
        file->size = file->offset;
    if (file->size > UINT32_MAX ||
        !fat32_update_short_entry(
            file,
            file->first_cluster,
            (uint32_t)file->size))
        return written ? (int64_t)written : -SYSCALL_EIO;
    return (int64_t)written;
}

int64_t fat32_mkdir(fat32_volume_t *volume, const char *path)
{
    if (!volume || !volume->mounted || !volume->writable || !path)
        return -SYSCALL_EINVAL;
    const char *start = path;
    while (*start == '/')
        start++;
    const char *last = start;
    for (const char *cursor = start; *cursor; cursor++)
    {
        if (*cursor == '/')
            last = cursor + 1;
    }
    size_t name_length = 0;
    while (last[name_length])
        name_length++;
    if (!name_length || name_length > FAT32_NAME_MAX)
        return -SYSCALL_EINVAL;
    char name[FAT32_NAME_MAX + 1];
    for (size_t i = 0; i < name_length; i++)
        name[i] = last[i];
    name[name_length] = '\0';
    size_t parent_length = (size_t)(last - start);
    while (parent_length && start[parent_length - 1] == '/')
        parent_length--;
    char parent_path[FAT32_NAME_MAX + 1];
    if (parent_length > FAT32_NAME_MAX)
        return -SYSCALL_ENAMETOOLONG;
    for (size_t i = 0; i < parent_length; i++)
        parent_path[i] = start[i];
    parent_path[parent_length] = '\0';
    fat32_dirent_t existing;
    if (lookup_path(volume, path, &existing))
        return -SYSCALL_EEXIST;
    fat32_dirent_t parent;
    if (!lookup_path(volume, parent_path, &parent) ||
        !parent.is_directory)
        return -SYSCALL_ENOENT;
    uint32_t cluster;
    if (!fat32_allocate_cluster(volume, &cluster))
        return -SYSCALL_ENOSPC;
    uint8_t sector[BLOCK_SECTOR_SIZE] = {0};
    sector[0] = '.';
    for (uint32_t i = 1; i < 11; i++)
        sector[i] = ' ';
    sector[11] = FAT32_ATTRIBUTE_DIRECTORY;
    write_le16(sector + 20, (uint16_t)(cluster >> 16));
    write_le16(sector + 26, (uint16_t)cluster);
    sector[32] = '.';
    sector[33] = '.';
    for (uint32_t i = 34; i < 43; i++)
        sector[i] = ' ';
    sector[43] = FAT32_ATTRIBUTE_DIRECTORY;
    uint32_t parent_cluster =
        parent_path[0] ? parent.first_cluster : 0;
    write_le16(
        sector + 52,
        (uint16_t)(parent_cluster >> 16));
    write_le16(sector + 58, (uint16_t)parent_cluster);
    if (!fat32_write_cluster(volume, cluster, 0, sector) ||
        !fat32_insert_directory_entry(
            volume,
            parent.first_cluster,
            name,
            cluster,
            0,
            FAT32_ATTRIBUTE_DIRECTORY,
            NULL))
    {
        (void)fat32_free_chain(volume, cluster);
        return -SYSCALL_EIO;
    }
    return 0;
}
