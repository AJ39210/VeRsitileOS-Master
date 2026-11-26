#include <stdbool.h>
#include <stdint.h>

#include "atapi.h"
#include "block.h"
#include "devfs.h"
#include "heap.h"
#include "idt.h"
#include "iso9660.h"
#include "mbr.h"
#include "procfs.h"
#include "serial.h"
#include "syscall.h"
#include "sysfs.h"
#include "vfs.h"

#define VFS_PATH_MAX 256
#define VFS_RAM_DIR_LIMIT 64
#define VFS_RAM_FILE_LIMIT 64
#define VFS_RAM_FILE_BYTES_LIMIT (32ULL * 1024 * 1024)
#define VFS_FAT32_VOLUME_LIMIT 8
#define VFS_EXT2_VOLUME_LIMIT 8
#define VFS_INITRAMFS_ENTRY_LIMIT 512

typedef struct
{
    bool used;
    char path[VFS_PATH_MAX];
} ram_directory_t;

typedef struct
{
    bool used;
    char path[VFS_PATH_MAX];
    uint8_t *data;
    uint64_t size;
    uint64_t capacity;
} ram_file_t;

typedef struct
{
    const char *path;
    const char *filesystem;
    bool mounted;
} mount_entry_t;

typedef struct
{
    uint16_t io_base;
    uint16_t control_base;
    bool slave;
} atapi_candidate_t;

typedef struct
{
    char path[VFS_PATH_MAX];
    const uint8_t *data;
    uint64_t size;
    bool is_directory;
} initramfs_entry_t;

static mount_entry_t mounts[] = {
    { "/", "iso9660", false },
    { "/dev", "devfs", true },
    { "/proc", "procfs", true },
    { "/sys", "sysfs", true }
};

static atapi_device_t boot_cdrom;
static bool root_mounted;
static ram_directory_t ram_directories[VFS_RAM_DIR_LIMIT];
static ram_file_t ram_files[VFS_RAM_FILE_LIMIT];
static uint64_t ram_file_capacity;
static fat32_volume_t fat32_volumes[VFS_FAT32_VOLUME_LIMIT];
static uint32_t fat32_volume_count;
static ext2_volume_t ext2_volumes[VFS_EXT2_VOLUME_LIMIT];
static initramfs_entry_t initramfs_entries[VFS_INITRAMFS_ENTRY_LIMIT];
static uint32_t initramfs_entry_count;
static bool initramfs_ready;

static bool parent_path(const char *path, char *parent);
static bool path_equals(const char *left, const char *right);
static bool path_has_mount_prefix(const char *path, const char *mount);

static bool find_fat32_volume(
    const char *path,
    fat32_volume_t **volume,
    const char **relative_path
)
{
    for (uint32_t i = 0; i < VFS_FAT32_VOLUME_LIMIT; i++)
    {
        if (fat32_path_matches(
                &fat32_volumes[i],
                path,
                relative_path))
        {
            if (volume)
                *volume = &fat32_volumes[i];
            return true;
        }
    }
    return false;
}

static bool find_ext2_volume(
    const char *path,
    ext2_volume_t **volume,
    const char **relative_path
)
{
    bool virtual_root_path =
        path_has_mount_prefix(path, "/dev") ||
        path_has_mount_prefix(path, "/proc") ||
        path_has_mount_prefix(path, "/sys");
    for (uint32_t i = 0; i < VFS_EXT2_VOLUME_LIMIT; i++)
    {
        if (virtual_root_path &&
            ext2_volumes[i].mounted &&
            path_equals(ext2_volumes[i].mount_path, "/"))
            continue;
        if (ext2_path_matches(
                &ext2_volumes[i],
                path,
                relative_path))
        {
            if (volume)
                *volume = &ext2_volumes[i];
            return true;
        }
    }
    return false;
}

static bool path_is_valid(const char *path)
{
    if (!path || path[0] != '/')
        return false;

    uint64_t length = 0;
    while (length < VFS_PATH_MAX && path[length])
        length++;
    if (length == 0 || length == VFS_PATH_MAX)
        return false;

    for (uint64_t i = 0; i + 1 < length; i++)
    {
        if ((path[i] == '/' && path[i + 1] == '/') ||
            (path[i] == '.' && path[i + 1] == '.' &&
             (i == 0 || path[i - 1] == '/') &&
             (i + 2 == length || path[i + 2] == '/')))
            return false;
    }
    return true;
}

static bool path_equals(const char *left, const char *right)
{
    while (*left && *right)
    {
        if (*left++ != *right++)
            return false;
    }
    return *left == '\0' && *right == '\0';
}

static uint64_t path_length(const char *path)
{
    uint64_t length = 0;
    while (path[length])
        length++;
    return length;
}

static uint64_t tar_octal_value(const uint8_t *field, uint64_t length)
{
    uint64_t value = 0;
    uint64_t i = 0;
    while (i < length && (field[i] == ' ' || field[i] == '\0'))
        i++;
    for (; i < length && field[i] >= '0' && field[i] <= '7'; i++)
    {
        if (value > (UINT64_MAX - (field[i] - '0')) / 8)
            return UINT64_MAX;
        value = value * 8 + (field[i] - '0');
    }
    return value;
}

static bool tar_field_length(
    const uint8_t *field,
    uint64_t capacity,
    uint64_t *length
)
{
    uint64_t i = 0;
    while (i < capacity && field[i])
        i++;
    if (i == capacity)
        return false;
    *length = i;
    return true;
}

static bool initramfs_build_path(
    const uint8_t *header,
    char *path,
    bool is_directory
)
{
    uint64_t name_length;
    uint64_t prefix_length;
    if (!tar_field_length(header, 100, &name_length) ||
        !tar_field_length(header + 345, 155, &prefix_length) ||
        !name_length)
        return false;
    if (!prefix_length &&
        ((name_length == 1 && header[0] == '.') ||
         (name_length == 2 && header[0] == '.' && header[1] == '/')))
    {
        path[0] = '/';
        path[1] = '\0';
        return is_directory;
    }

    uint64_t relative_length = prefix_length
        ? prefix_length + 1 + name_length
        : name_length;
    if (relative_length >= VFS_PATH_MAX - 1)
        return false;

    uint64_t out = 0;
    if (prefix_length)
    {
        for (uint64_t i = 0; i < prefix_length; i++)
            path[++out] = (char)header[345 + i];
        path[++out] = '/';
    }
    for (uint64_t i = 0; i < name_length; i++)
        path[++out] = (char)header[i];
    path[0] = '/';
    path[out + 1] = '\0';

    uint64_t start = 1;
    while (path[start] == '.' && path[start + 1] == '/')
    {
        start += 2;
        if (start > out)
            return false;
    }
    if (start > 1)
    {
        uint64_t source = start;
        uint64_t target = 1;
        while (source <= out)
            path[target++] = path[source++];
        out -= start - 1;
    }

    if (is_directory && out > 1 && path[out] == '/')
        path[out--] = '\0';
    if (out == 1 && path[1] == '.')
    {
        path[1] = '\0';
        return true;
    }
    if (out <= 1 || path[1] == '/')
        return false;
    path[out + 1] = '\0';
    return path_is_valid(path);
}

bool vfs_set_initramfs(const void *archive, uint64_t size)
{
    const uint8_t *bytes = archive;
    initramfs_ready = false;
    initramfs_entry_count = 0;
    if (!bytes || size < 512)
        return false;

    uint64_t offset = 0;
    while (offset <= size - 512)
    {
        const uint8_t *header = bytes + offset;
        bool empty = true;
        for (uint64_t i = 0; i < 512; i++)
        {
            if (header[i])
            {
                empty = false;
                break;
            }
        }
        if (empty)
        {
            initramfs_ready = initramfs_entry_count != 0;
            return initramfs_ready;
        }

        uint64_t file_size = tar_octal_value(header + 124, 12);
        if (file_size == UINT64_MAX)
            break;
        uint64_t data_offset = offset + 512;
        if (file_size > size - data_offset)
            break;

        uint8_t type = header[156];
        bool is_directory = type == '5';
        bool is_regular = type == '\0' || type == '0';
        if (is_directory || is_regular)
        {
            if (initramfs_entry_count >= VFS_INITRAMFS_ENTRY_LIMIT)
                break;

            initramfs_entry_t *entry =
                &initramfs_entries[initramfs_entry_count];
            if (!initramfs_build_path(header, entry->path, is_directory))
                break;
            entry->data = is_directory ? 0 : bytes + data_offset;
            entry->size = is_directory ? 0 : file_size;
            entry->is_directory = is_directory;
            initramfs_entry_count++;
        }

        if (file_size > UINT64_MAX - 511)
            break;
        uint64_t padded_size = (file_size + 511) & ~UINT64_C(511);
        if (padded_size > size - data_offset)
            break;
        offset = data_offset + padded_size;
    }

    initramfs_entry_count = 0;
    return false;
}

static const initramfs_entry_t *initramfs_find(const char *path)
{
    for (uint32_t i = 0; i < initramfs_entry_count; i++)
    {
        if (path_equals(path, initramfs_entries[i].path))
            return &initramfs_entries[i];
    }
    return 0;
}

static bool initramfs_has_child(const char *parent, const char *name)
{
    char path[VFS_PATH_MAX];
    uint64_t parent_length = 0;
    while (parent[parent_length])
        parent_length++;
    uint64_t name_length = 0;
    while (name[name_length])
        name_length++;
    uint64_t separator = path_equals(parent, "/") ? 0 : 1;
    if (parent_length + separator + name_length >= sizeof(path))
        return false;
    uint64_t out = 0;
    for (uint64_t i = 0; i < parent_length; i++)
        path[out++] = parent[i];
    if (separator)
        path[out++] = '/';
    for (uint64_t i = 0; i < name_length; i++)
        path[out++] = name[i];
    path[out] = '\0';
    return initramfs_find(path) != 0;
}

static bool ram_directory_exists(const char *path)
{
    for (uint32_t i = 0; i < VFS_RAM_DIR_LIMIT; i++)
    {
        if (ram_directories[i].used &&
            path_equals(ram_directories[i].path, path))
            return true;
    }
    return false;
}

static int32_t ram_file_find(const char *path)
{
    for (uint32_t i = 0; i < VFS_RAM_FILE_LIMIT; i++)
    {
        if (ram_files[i].used && path_equals(ram_files[i].path, path))
            return (int32_t)i;
    }
    return -1;
}

static bool vfs_is_directory_internal(const char *path)
{
    if (path_equals(path, "/mnt"))
        return true;
    fat32_volume_t *volume;
    const char *relative_path;
    if (find_fat32_volume(path, &volume, &relative_path))
        return fat32_is_directory(volume, relative_path);
    ext2_volume_t *ext2_volume;
    if (find_ext2_volume(path, &ext2_volume, &relative_path))
        return ext2_is_directory(ext2_volume, relative_path);

    iso9660_file_t file;
    if (root_mounted &&
        iso9660_find(path, &file) &&
        file.is_directory)
        return true;
    const initramfs_entry_t *initramfs_entry = initramfs_find(path);
    if (initramfs_entry && initramfs_entry->is_directory)
        return true;
    return ram_directory_exists(path) || path_equals(path, "/") ||
        devfs_is_directory(path) ||
        procfs_is_directory(path) ||
        sysfs_is_directory(path);
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static bool path_has_mount_prefix(const char *path, const char *mount)
{
    uint64_t length = 0;
    while (mount[length])
        length++;

    for (uint64_t i = 0; i < length; i++)
    {
        if (path[i] != mount[i])
            return false;
    }

    return path[length] == '\0' || path[length] == '/';
}

static bool mount_iso_root(void)
{
    static const atapi_candidate_t candidates[] = {
        { 0x1F0, 0x3F6, false },
        { 0x1F0, 0x3F6, true },
        { 0x170, 0x376, false },
        { 0x170, 0x376, true }
    };

    for (uint32_t i = 0;
         i < sizeof(candidates) / sizeof(candidates[0]);
         i++)
    {
        uint8_t status = inb(candidates[i].io_base + 7);
        if (status == 0 || status == 0xFF)
            continue;

        if (!atapi_init(
                &boot_cdrom,
                candidates[i].io_base,
                candidates[i].control_base,
                candidates[i].slave))
            continue;

        if (iso9660_init(atapi_read_sector, &boot_cdrom))
        {
            devfs_register_cdrom(
                atapi_read_sector,
                &boot_cdrom,
                iso9660_get_volume_sectors()
            );
            return true;
        }
    }

    return false;
}

bool vfs_init(void)
{
    for (uint32_t i = 0; i < VFS_RAM_FILE_LIMIT; i++)
    {
        if (ram_files[i].data)
            kfree(ram_files[i].data);
        ram_files[i] = (ram_file_t){0};
    }
    ram_file_capacity = 0;
    for (uint32_t i = 0; i < VFS_RAM_DIR_LIMIT; i++)
        ram_directories[i].used = false;
    ram_directories[0].used = true;
    ram_directories[0].path[0] = '/';
    ram_directories[0].path[1] = 'v';
    ram_directories[0].path[2] = 'a';
    ram_directories[0].path[3] = 'r';
    ram_directories[0].path[4] = '\0';
    ram_directories[1].used = true;
    const char boot_log_directory[] = "/var/boot_logs";
    for (uint32_t i = 0; i < sizeof(boot_log_directory); i++)
        ram_directories[1].path[i] = boot_log_directory[i];
    ram_directories[2].used = true;
    const char temporary_directory[] = "/tmp";
    for (uint32_t i = 0; i < sizeof(temporary_directory); i++)
        ram_directories[2].path[i] = temporary_directory[i];
    ram_directories[3].used = true;
    const char systemd_log_directory[] = "/var/systemd";
    for (uint32_t i = 0; i < sizeof(systemd_log_directory); i++)
        ram_directories[3].path[i] = systemd_log_directory[i];
    for (uint32_t i = 0; i < VFS_FAT32_VOLUME_LIMIT; i++)
        fat32_unmount(&fat32_volumes[i]);
    fat32_volume_count = 0;
    for (uint32_t i = 0; i < VFS_EXT2_VOLUME_LIMIT; i++)
        ext2_unmount(&ext2_volumes[i]);

    root_mounted = initramfs_ready || mount_iso_root();
    mounts[0].mounted = root_mounted;

    if (initramfs_ready)
    {
        serial_log(
            SERIAL_LOG_INFO,
            "Mounted Limine initramfs root filesystem at / (read-only)"
        );
    }
    else if (root_mounted)
    {
        serial_log(
            SERIAL_LOG_INFO,
            "Mounted ISO9660 root filesystem at / (read-only)"
        );
    }
    else
    {
        serial_log(
            SERIAL_LOG_WARN,
            "Root ISO9660 filesystem unavailable; / paths return ENODEV"
        );
    }

    for (uint32_t i = 1; i < sizeof(mounts) / sizeof(mounts[0]); i++)
    {
        serial_log_begin(SERIAL_LOG_INFO);
        serial_write("Mounted ");
        serial_write(mounts[i].filesystem);
        serial_write(" at ");
        serial_write(mounts[i].path);
        serial_log_end();
    }

    return true;
}

bool vfs_root_is_mounted(void)
{
    return root_mounted;
}

static size_t append_decimal(char *output, size_t position, uint32_t value)
{
    char digits[10];
    size_t count = 0;
    do
    {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value && count < sizeof(digits));
    while (count)
        output[position++] = digits[--count];
    return position;
}

static bool partition_may_contain_fat32(const mbr_partition_t *partition)
{
    static const uint8_t microsoft_basic_data_guid[16] = {
        0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
        0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7
    };
    static const uint8_t efi_system_guid[16] = {
        0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
        0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
    };
    if (!partition)
        return false;
    if (!partition->gpt)
        return partition->type == 0x0B || partition->type == 0x0C;
    for (size_t i = 0; i < sizeof(partition->type_guid); i++)
    {
        if (partition->type_guid[i] != microsoft_basic_data_guid[i])
            break;
        if (i == sizeof(partition->type_guid) - 1)
            return true;
    }
    for (size_t i = 0; i < sizeof(partition->type_guid); i++)
    {
        if (partition->type_guid[i] != efi_system_guid[i])
            break;
        if (i == sizeof(partition->type_guid) - 1)
            return true;
    }
    return false;
}

static bool partition_may_contain_ext2(const mbr_partition_t *partition)
{
    static const uint8_t linux_filesystem_guid[16] = {
        0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
        0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4
    };
    if (!partition)
        return false;
    if (!partition->gpt)
        return partition->type == 0x83;
    for (size_t i = 0; i < sizeof(partition->type_guid); i++)
    {
        if (partition->type_guid[i] != linux_filesystem_guid[i])
            return false;
    }
    return true;
}

bool vfs_mount_ext2_partition(
    uint32_t device_index,
    uint64_t partition_lba,
    uint64_t partition_sectors,
    const char *mount_path
)
{
    if (!mount_path || !path_is_valid(mount_path) ||
        path_equals(mount_path, "/"))
        return false;
    for (uint32_t i = 0; i < VFS_EXT2_VOLUME_LIMIT; i++)
    {
        if (ext2_volumes[i].mounted &&
            path_equals(ext2_volumes[i].mount_path, mount_path))
            return false;
    }
    for (uint32_t i = 0; i < VFS_EXT2_VOLUME_LIMIT; i++)
    {
        if (!ext2_volumes[i].mounted &&
            ext2_mount(
                &ext2_volumes[i],
                device_index,
                partition_lba,
                partition_sectors,
                mount_path))
            return true;
    }
    return false;
}

bool vfs_mount_fat32_partition(
    uint32_t device_index,
    uint64_t partition_lba,
    uint64_t partition_sectors,
    const char *mount_path
)
{
    if (!mount_path || !path_is_valid(mount_path) ||
        path_equals(mount_path, "/"))
        return false;
    for (uint32_t i = 0; i < VFS_FAT32_VOLUME_LIMIT; i++)
    {
        if (fat32_volumes[i].mounted &&
            (path_equals(fat32_volumes[i].mount_path, mount_path) ||
             (fat32_volumes[i].device_index == device_index &&
              fat32_volumes[i].partition_lba == partition_lba)))
            return false;
    }
    for (uint32_t i = 0; i < VFS_FAT32_VOLUME_LIMIT; i++)
    {
        if (!fat32_volumes[i].mounted &&
            fat32_mount(
                &fat32_volumes[i],
                device_index,
                partition_lba,
                partition_sectors,
                mount_path))
        {
            fat32_volume_count++;
            return true;
        }
    }
    return false;
}

bool vfs_unmount_fat32(const char *mount_path)
{
    if (!mount_path)
        return false;
    for (uint32_t i = 0; i < VFS_FAT32_VOLUME_LIMIT; i++)
    {
        if (fat32_volumes[i].mounted &&
            path_equals(fat32_volumes[i].mount_path, mount_path))
        {
            fat32_unmount(&fat32_volumes[i]);
            if (fat32_volume_count)
                fat32_volume_count--;
            return true;
        }
    }
    return false;
}

bool vfs_mount_installed_root(void)
{
    static const char installed_volume_name[] = "VeRsitileOS";
    for (uint32_t slot = 0; slot < VFS_EXT2_VOLUME_LIMIT; slot++)
    {
        if (ext2_volumes[slot].mounted &&
            path_equals(ext2_volumes[slot].mount_path, "/"))
            return false;
    }
    bool previous_root_mounted = root_mounted;
    root_mounted = false;
    mounts[0].mounted = false;
    for (uint32_t i = 0; i < mbr_partition_count(); i++)
    {
        const mbr_partition_t *partition = mbr_partition_get(i);
        if (!partition_may_contain_ext2(partition))
            continue;
        for (uint32_t slot = 0; slot < VFS_EXT2_VOLUME_LIMIT; slot++)
        {
            if (ext2_volumes[slot].mounted)
                continue;
            if (!ext2_mount(
                    &ext2_volumes[slot],
                    partition->device_index,
                    partition->start_lba,
                    partition->sector_count,
                    "/"))
                continue;
            bool matches = path_equals(
                ext2_volumes[slot].volume_name,
                installed_volume_name);
            if (!matches)
            {
                ext2_unmount(&ext2_volumes[slot]);
                continue;
            }
            root_mounted = true;
            mounts[0].mounted = true;
            serial_log(
                SERIAL_LOG_INFO,
                "Mounted installed ext2 root filesystem at /");
            return true;
        }
    }
    root_mounted = previous_root_mounted;
    mounts[0].mounted = previous_root_mounted;
    return false;
}

bool vfs_root_uses_device(uint32_t device_index)
{
    for (uint32_t i = 0; i < VFS_EXT2_VOLUME_LIMIT; i++)
    {
        if (ext2_volumes[i].mounted &&
            path_equals(ext2_volumes[i].mount_path, "/"))
            return ext2_volumes[i].device_index == device_index;
    }
    return false;
}

bool vfs_unmount_device_partitions(uint32_t device_index)
{
    if (vfs_root_uses_device(device_index))
        return false;
    for (uint32_t i = 0; i < VFS_EXT2_VOLUME_LIMIT; i++)
    {
        if (ext2_volumes[i].mounted &&
            ext2_volumes[i].device_index == device_index &&
            ext2_volumes[i].open_files)
            return false;
    }
    for (uint32_t i = 0; i < VFS_FAT32_VOLUME_LIMIT; i++)
    {
        if (fat32_volumes[i].mounted &&
            fat32_volumes[i].device_index == device_index)
        {
            fat32_unmount(&fat32_volumes[i]);
            if (fat32_volume_count)
                fat32_volume_count--;
        }
    }
    for (uint32_t i = 0; i < VFS_EXT2_VOLUME_LIMIT; i++)
    {
        if (ext2_volumes[i].mounted &&
            ext2_volumes[i].device_index == device_index)
        {
            if (!ext2_sync(&ext2_volumes[i]))
                return false;
            ext2_unmount(&ext2_volumes[i]);
        }
    }
    return true;
}

bool vfs_unmount_ext2(const char *mount_path)
{
    if (!mount_path)
        return false;
    for (uint32_t i = 0; i < VFS_EXT2_VOLUME_LIMIT; i++)
    {
        if (ext2_volumes[i].mounted &&
            path_equals(ext2_volumes[i].mount_path, mount_path))
        {
            if (ext2_volumes[i].open_files)
                return false;
            if (!ext2_sync(&ext2_volumes[i]))
                return false;
            ext2_unmount(&ext2_volumes[i]);
            return true;
        }
    }
    return false;
}

bool vfs_clone_file(vfs_file_t *destination, const vfs_file_t *source)
{
    if (!destination || !source)
        return false;
    *destination = *source;
    if (source->backend == VFS_BACKEND_EXT2)
    {
        return ext2_clone_file(
            &destination->ext2_file,
            &source->ext2_file);
    }
    return true;
}

void vfs_close_file(vfs_file_t *file)
{
    if (file && file->backend == VFS_BACKEND_EXT2)
        ext2_close(&file->ext2_file);
}

uint32_t vfs_mount_ext2_partitions(void)
{
    uint32_t mounted = 0;
    for (uint32_t i = 0;
         i < mbr_partition_count() &&
         mounted < VFS_EXT2_VOLUME_LIMIT;
         i++)
    {
        const mbr_partition_t *partition = mbr_partition_get(i);
        if (!partition_may_contain_ext2(partition))
            continue;
        bool already_mounted = false;
        for (uint32_t slot = 0; slot < VFS_EXT2_VOLUME_LIMIT; slot++)
        {
            if (ext2_volumes[slot].mounted &&
                ext2_volumes[slot].device_index ==
                    partition->device_index &&
                ext2_volumes[slot].partition_lba ==
                    partition->start_lba)
            {
                already_mounted = true;
                break;
            }
        }
        if (already_mounted)
            continue;
        const block_device_t *device =
            block_device_get(partition->device_index);
        if (!device)
            continue;
        char mount_path[EXT2_MOUNT_PATH_MAX] = "/mnt/";
        size_t position = sizeof("/mnt/") - 1;
        for (size_t j = 0; device->name[j] &&
             position < sizeof(mount_path) - 1; j++)
            mount_path[position++] = device->name[j];
        if (partition->number)
        {
            if (position >= sizeof(mount_path) - 2)
                continue;
            mount_path[position++] = 'p';
            position = append_decimal(
                mount_path,
                position,
                partition->number);
        }
        if (position >= sizeof(mount_path))
            continue;
        mount_path[position] = '\0';
        if (vfs_mount_ext2_partition(
                partition->device_index,
                partition->start_lba,
                partition->sector_count,
                mount_path))
            mounted++;
    }
    return mounted;
}

uint32_t vfs_mount_fat32_partitions(void)
{
    uint32_t mounted = 0;
    for (uint32_t i = 0;
         i < mbr_partition_count() &&
         fat32_volume_count < VFS_FAT32_VOLUME_LIMIT;
         i++)
    {
        const mbr_partition_t *partition = mbr_partition_get(i);
        if (!partition_may_contain_fat32(partition))
            continue;

        const block_device_t *device =
            block_device_get(partition->device_index);
        if (!device)
            continue;
        char mount_path[FAT32_VOLUME_NAME_MAX] = "/mnt/";
        size_t position = sizeof("/mnt/") - 1;
        for (size_t j = 0; device->name[j] &&
             position < sizeof(mount_path) - 1; j++)
            mount_path[position++] = device->name[j];
        if (position >= sizeof(mount_path) - 1)
            continue;
        if (partition->number)
        {
            mount_path[position++] = 'p';
            position = append_decimal(
                mount_path,
                position,
                partition->number);
        }
        mount_path[position] = '\0';

        if (!vfs_mount_fat32_partition(
                partition->device_index,
                partition->start_lba,
                partition->sector_count,
                mount_path))
            continue;
        mounted++;
        serial_log_begin(SERIAL_LOG_INFO);
        serial_write("Mounted FAT32 volume at ");
        serial_write(mount_path);
        serial_log_end();
    }
    return mounted;
}

int64_t vfs_open(const char *path, vfs_file_t *file)
{
    if (!path_is_valid(path) || !file)
        return -SYSCALL_EINVAL;

    uint64_t length = 0;
    while (length < VFS_PATH_MAX && path[length])
        length++;

    *file = (vfs_file_t){0};
    int32_t ram_index = ram_file_find(path);
    if (ram_index >= 0)
    {
        file->backend = VFS_BACKEND_RAMFS;
        file->ram_file_index = (uint32_t)ram_index;
        file->size = ram_files[ram_index].size;
        for (uint64_t i = 0; i <= length; i++)
            file->path[i] = path[i];
        return 0;
    }

    if (path_equals(path, "/var/boot_logs/boot.txt"))
    {
        file->backend = VFS_BACKEND_BOOT_LOG;
        file->size = serial_boot_log_size();
    }
    else if (path_equals(path, "/var/systemd/systemd.log"))
    {
        file->backend = VFS_BACKEND_SYSTEMD_LOG;
        file->size = serial_systemd_log_size();
    }
    else
    {
        fat32_volume_t *fat_volume;
        const char *fat_relative_path;
        if (find_fat32_volume(path, &fat_volume, &fat_relative_path))
        {
            int64_t result = fat32_open(
                fat_volume,
                fat_relative_path,
                &file->fat32_file);
            if (result < 0)
                return fat32_is_directory(fat_volume, fat_relative_path)
                    ? -SYSCALL_EISDIR
                    : -SYSCALL_ENOENT;
            file->backend = VFS_BACKEND_FAT32;
            file->size = file->fat32_file.size;
        }
        else
        {
            ext2_volume_t *ext2_volume;
            const char *ext2_relative_path;
            if (find_ext2_volume(
                    path, &ext2_volume, &ext2_relative_path))
            {
                int64_t result = ext2_open(
                    ext2_volume,
                    ext2_relative_path,
                    false,
                    false,
                    &file->ext2_file);
                if (result < 0)
                    return result;
                file->backend = VFS_BACKEND_EXT2;
                file->size = file->ext2_file.size;
            }
            else if (path_has_mount_prefix(path, "/dev"))
            {
                if (devfs_is_directory(path))
                    return -SYSCALL_EISDIR;
                if (!devfs_file_size(path, &file->size))
                    return -SYSCALL_ENOENT;
                file->backend = VFS_BACKEND_DEVFS;
            }

            else if (path_has_mount_prefix(path, "/proc"))
            {
                if (procfs_is_directory(path))
                    return -SYSCALL_EISDIR;
                if (!procfs_file_size(path, &file->size))
                    return -SYSCALL_ENOENT;
                file->backend = VFS_BACKEND_PROCFS;
            }
            else if (path_has_mount_prefix(path, "/sys"))
            {
                if (sysfs_is_directory(path))
                    return -SYSCALL_EISDIR;
                if (!sysfs_file_size(path, &file->size))
                    return -SYSCALL_ENOENT;
                file->backend = VFS_BACKEND_SYSFS;
            }
            else
            {
                if (!root_mounted)
                    return -SYSCALL_ENODEV;

                if (initramfs_ready)
                {
                    const initramfs_entry_t *entry = initramfs_find(path);
                    if (!entry)
                        return -SYSCALL_ENOENT;
                    if (entry->is_directory)
                        return -SYSCALL_EISDIR;
                    file->backend = VFS_BACKEND_INITRAMFS;
                    file->initramfs_data = entry->data;
                    file->size = entry->size;
                }
                else
                {
                    if (!iso9660_find(path, &file->iso_file))
                        return -SYSCALL_ENOENT;
                    if (file->iso_file.is_directory)
                        return -SYSCALL_EISDIR;

                    file->backend = VFS_BACKEND_ISO9660;
                    file->size = file->iso_file.size;
                }
            }
        }
    }

    for (uint64_t i = 0; i <= length; i++)
        file->path[i] = path[i];

    return 0;
}

int64_t vfs_open_write(
    const char *path,
    bool create,
    bool truncate,
    vfs_file_t *file
)
{
    if (!path_is_valid(path) || !file || path_equals(path, "/"))
        return -SYSCALL_EINVAL;
    if (path_has_mount_prefix(path, "/dev") ||
        path_has_mount_prefix(path, "/proc") ||
        path_has_mount_prefix(path, "/sys") ||
        path_has_mount_prefix(path, "/var"))
        return -SYSCALL_EROFS;
    *file = (vfs_file_t){0};

    fat32_volume_t *fat_volume;
    const char *fat_relative_path;
    if (find_fat32_volume(path, &fat_volume, &fat_relative_path))
    {
        int64_t result = fat32_open_write(
            fat_volume,
            fat_relative_path,
            create,
            truncate,
            &file->fat32_file);
        if (result < 0)
            return result;
        file->backend = VFS_BACKEND_FAT32;
        file->size = file->fat32_file.size;
        uint64_t length = path_length(path);
        for (uint64_t i = 0; i <= length; i++)
            file->path[i] = path[i];
        return 0;
    }

    ext2_volume_t *ext2_volume;
    const char *ext2_relative_path;
    if (find_ext2_volume(path, &ext2_volume, &ext2_relative_path))
    {
        int64_t result = ext2_open(
            ext2_volume,
            ext2_relative_path,
            create,
            truncate,
            &file->ext2_file);
        if (result < 0)
            return result;
        file->backend = VFS_BACKEND_EXT2;
        file->size = file->ext2_file.size;
        uint64_t length = path_length(path);
        for (uint64_t i = 0; i <= length; i++)
            file->path[i] = path[i];
        return 0;
    }

    int32_t index = ram_file_find(path);
    if (index < 0)
    {
        if (!create)
        {
            vfs_file_t existing;
            int64_t result = vfs_open(path, &existing);
            return result < 0 ? result : -SYSCALL_EROFS;
        }
        if (vfs_is_directory_internal(path))
            return -SYSCALL_EISDIR;

        char parent[VFS_PATH_MAX];
        if (!parent_path(path, parent) ||
            !vfs_is_directory_internal(parent))
            return -SYSCALL_ENOENT;

        iso9660_file_t existing_iso;
        if (root_mounted && iso9660_find(path, &existing_iso) &&
            existing_iso.is_directory)
            return -SYSCALL_EISDIR;
        const initramfs_entry_t *existing_initramfs = initramfs_find(path);
        if (existing_initramfs && existing_initramfs->is_directory)
            return -SYSCALL_EISDIR;

        for (uint32_t i = 0; i < VFS_RAM_FILE_LIMIT; i++)
        {
            if (!ram_files[i].used)
            {
                index = (int32_t)i;
                break;
            }
        }
        if (index < 0)
            return -SYSCALL_ENOMEM;
        ram_file_t *new_file = &ram_files[index];
        new_file->used = true;
        uint64_t length = path_length(path);
        for (uint64_t i = 0; i <= length; i++)
            new_file->path[i] = path[i];
    }

    ram_file_t *ram_file = &ram_files[index];
    if (truncate)
        ram_file->size = 0;
    *file = (vfs_file_t) {
        .backend = VFS_BACKEND_RAMFS,
        .ram_file_index = (uint32_t)index,
        .size = ram_file->size
    };
    uint64_t length = path_length(path);
    for (uint64_t i = 0; i <= length; i++)
        file->path[i] = path[i];
    return 0;
}

int64_t vfs_read(vfs_file_t *file, void *buffer, uint64_t size)
{
    if (!file || (!buffer && size))
        return -SYSCALL_EINVAL;

    if (size == 0)
        return 0;

    if ((file->backend != VFS_BACKEND_DEVFS ||
         file->size != UINT64_MAX) &&
        file->offset >= file->size)
        return 0;

    if ((file->backend != VFS_BACKEND_DEVFS ||
         file->size != UINT64_MAX) &&
        size > file->size - file->offset)
        size = file->size - file->offset;

    int64_t result;
    switch (file->backend)
    {
        case VFS_BACKEND_ISO9660:
        {
            uint64_t total = 0;
            while (total < size)
            {
                uint32_t amount = (uint32_t)(size - total);
                if (amount > ISO9660_SECTOR_SIZE)
                    amount = ISO9660_SECTOR_SIZE;
                if (!iso9660_read(
                        &file->iso_file,
                        (uint32_t)(file->offset + total),
                        amount,
                        (uint8_t *)buffer + total))
                    return total ? (int64_t)total : -SYSCALL_EIO;
                total += amount;
            }
            result = (int64_t)total;
            break;
        }
        case VFS_BACKEND_INITRAMFS:
            for (uint64_t i = 0; i < size; i++)
                ((uint8_t *)buffer)[i] = file->initramfs_data[file->offset + i];
            result = (int64_t)size;
            break;
        case VFS_BACKEND_RAMFS:
            if (file->ram_file_index >= VFS_RAM_FILE_LIMIT ||
                !ram_files[file->ram_file_index].used)
                return -SYSCALL_EBADF;
            for (uint64_t i = 0; i < size; i++)
                ((uint8_t *)buffer)[i] =
                    ram_files[file->ram_file_index].data[file->offset + i];
            result = (int64_t)size;
            break;
        case VFS_BACKEND_FAT32:
            file->fat32_file.offset = file->offset;
            result = fat32_read(&file->fat32_file, buffer, size);
            break;
        case VFS_BACKEND_EXT2:
            file->ext2_file.offset = file->offset;
            result = ext2_read(&file->ext2_file, buffer, size);
            break;
        case VFS_BACKEND_DEVFS:
            result = devfs_read(
                file->path,
                file->offset,
                buffer,
                size
            );
            break;
        case VFS_BACKEND_PROCFS:
            result = procfs_read(
                file->path,
                file->offset,
                buffer,
                size
            );
            break;
        case VFS_BACKEND_SYSFS:
            result = sysfs_read(
                file->path,
                file->offset,
                buffer,
                size
            );
            break;
        case VFS_BACKEND_BOOT_LOG:
            result = (int64_t)serial_boot_log_read(
                file->offset,
                buffer,
                size
            );
            break;
        case VFS_BACKEND_SYSTEMD_LOG:
            result = (int64_t)serial_systemd_log_read(
                file->offset,
                buffer,
                size
            );
            break;
        default:
            return -SYSCALL_EBADF;
    }

    if (result > 0)
        file->offset += (uint64_t)result;
    return result;
}

int64_t vfs_write(vfs_file_t *file, const void *buffer, uint64_t size)
{
    if (!file || (!buffer && size))
        return -SYSCALL_EINVAL;

    if (file->backend == VFS_BACKEND_RAMFS)
    {
        if (file->ram_file_index >= VFS_RAM_FILE_LIMIT ||
            !ram_files[file->ram_file_index].used)
            return -SYSCALL_EBADF;
        ram_file_t *ram_file = &ram_files[file->ram_file_index];
        if (size > VFS_RAM_FILE_BYTES_LIMIT ||
            file->offset > VFS_RAM_FILE_BYTES_LIMIT - size)
            return -SYSCALL_ENOSPC;
        uint64_t required = file->offset + size;
        if (required > ram_file->capacity)
        {
            uint64_t capacity = ram_file->capacity ? ram_file->capacity : 4096;
            while (capacity < required)
            {
                if (capacity > VFS_RAM_FILE_BYTES_LIMIT / 2)
                {
                    capacity = VFS_RAM_FILE_BYTES_LIMIT;
                    break;
                }
                capacity *= 2;
            }
            if (capacity - ram_file->capacity >
                VFS_RAM_FILE_BYTES_LIMIT - ram_file_capacity)
                return -SYSCALL_ENOSPC;
            uint8_t *data = kmalloc(capacity);
            if (!data)
                return -SYSCALL_ENOMEM;
            for (uint64_t i = 0; i < ram_file->size; i++)
                data[i] = ram_file->data[i];
            if (ram_file->data)
                kfree(ram_file->data);
            ram_file_capacity += capacity - ram_file->capacity;
            ram_file->data = data;
            ram_file->capacity = capacity;
        }
        for (uint64_t i = ram_file->size; i < file->offset; i++)
            ram_file->data[i] = 0;
        for (uint64_t i = 0; i < size; i++)
            ram_file->data[file->offset + i] = ((const uint8_t *)buffer)[i];
        file->offset += size;
        if (file->offset > ram_file->size)
            ram_file->size = file->offset;
        file->size = ram_file->size;
        return (int64_t)size;
    }
    if (file->backend == VFS_BACKEND_EXT2)
    {
        file->ext2_file.offset = file->offset;
        int64_t result = ext2_write(&file->ext2_file, buffer, size);
        if (result > 0)
        {
            file->offset = file->ext2_file.offset;
            file->size = file->ext2_file.size;
        }
        return result;
    }
    if (file->backend == VFS_BACKEND_FAT32)
    {
        file->fat32_file.offset = file->offset;
        int64_t result = fat32_write(&file->fat32_file, buffer, size);
        if (result > 0)
        {
            file->offset = file->fat32_file.offset;
            file->size = file->fat32_file.size;
        }
        return result;
    }
    if (file->backend != VFS_BACKEND_DEVFS)
        return -SYSCALL_EROFS;

    int64_t result = devfs_write(
        file->path, file->offset, buffer, size);
    if (result > 0)
        file->offset += (uint64_t)result;
    return result;
}

static bool direct_child_name(
    const char *parent,
    const char *child,
    char *name
)
{
    uint64_t parent_length = 0;
    while (parent[parent_length])
        parent_length++;

    uint64_t child_length = 0;
    while (child[child_length])
        child_length++;

    uint64_t start = parent_length;
    if (path_equals(parent, "/"))
        start = 1;
    else if (child_length <= parent_length ||
             child[parent_length] != '/')
        return false;
    else
    {
        for (uint64_t i = 0; i < parent_length; i++)
        {
            if (parent[i] != child[i])
                return false;
        }
        start++;
    }

    uint64_t end = start;
    while (end < child_length && child[end] != '/')
        end++;
    if (end != child_length || end == start)
        return false;

    uint64_t length = end - start;
    if (length >= ISO9660_MAX_NAME)
        return false;
    for (uint64_t i = 0; i < length; i++)
        name[i] = child[start + i];
    name[length] = '\0';
    return true;
}

static bool initramfs_list(
    const char *path,
    uint64_t offset,
    vfs_dirent_t *entries,
    uint64_t capacity,
    uint64_t *total
)
{
    uint64_t count = 0;
    uint64_t returned = 0;
    for (uint32_t i = 0; i < initramfs_entry_count; i++)
    {
        char name[ISO9660_MAX_NAME];
        const initramfs_entry_t *entry = &initramfs_entries[i];
        if (!direct_child_name(path, entry->path, name))
            continue;
        if (count >= offset && returned < capacity)
        {
            uint64_t j = 0;
            while (name[j] && j < sizeof(entries[returned].name) - 1)
            {
                entries[returned].name[j] = name[j];
                j++;
            }
            entries[returned].name[j] = '\0';
            entries[returned].size = entry->size;
            entries[returned].is_directory = entry->is_directory;
            returned++;
        }
        count++;
    }
    *total = count;
    return true;
}

static int64_t list_virtual_directory(
    const char *path,
    uint64_t offset,
    vfs_dirent_t *entries,
    uint64_t capacity
)
{
    typedef struct
    {
        const char *name;
        bool is_directory;
    } virtual_entry_t;

    static const virtual_entry_t proc_root[] = {
        { "cpuinfo", false }, { "meminfo", false },
        { "devices", false }, { "partitions", false }
    };
    static const virtual_entry_t proc_process[] = {
        { "status", false }
    };
    static const virtual_entry_t sys_root[] = {
        { "kernel", true }, { "devices", true },
        { "class", true }, { "block", true }, { "bus", true }
    };
    static const virtual_entry_t sys_kernel[] = {
        { "osrelease", false }
    };
    static const virtual_entry_t sys_devices[] = {
        { "system", true }
    };
    static const virtual_entry_t sys_system[] = {
        { "cpu", true }
    };
    static const virtual_entry_t sys_cpu[] = {
        { "online", false }
    };
    static const virtual_entry_t sys_class[] = {
        { "tty", true }
    };
    static const virtual_entry_t sys_tty[] = {
        { "console", false }
    };
    static const virtual_entry_t sys_block[] = {
        { "sr0", true }
    };
    static const virtual_entry_t sys_sr0[] = {
        { "device", true }
    };
    static const virtual_entry_t sys_device[] = {
        { "type", false }
    };
    static const virtual_entry_t dev_root[] = {
        { "console", false }, { "keyboard", false },
        { "mouse", false }, { "null", false },
        { "zero", false }, { "fb0", false },
        { "sr0", false }, { "input", true },
        { "disk", true }, { "pts", true }, { "pci", true },
        { "dri", true }, { "snd", true }
    };
    static const virtual_entry_t dev_input[] = {
        { "mouse0", false }
    };
    static const virtual_entry_t dev_empty[] = {
        { 0, false }
    };
    static const virtual_entry_t var_root[] = {
        { "boot_logs", true }, { "systemd", true }
    };
    static const virtual_entry_t var_boot_logs[] = {
        { "boot.txt", false }
    };
    static const virtual_entry_t var_systemd[] = {
        { "systemd.log", false }
    };
    const virtual_entry_t *nodes = 0;
    uint64_t total = 0;

    if (path_equals(path, "/dev/disk"))
    {
        uint64_t disk_count = block_device_count();
        uint64_t all_partitions = mbr_partition_count();
        uint64_t partition_total = 0;
        for (uint64_t i = 0; i < all_partitions; i++)
        {
            const mbr_partition_t *partition =
                mbr_partition_get((uint32_t)i);
            if (partition && partition->number)
                partition_total++;
        }
        total = disk_count + partition_total;
        if (offset >= total)
            return 0;
        uint64_t returned = total - offset;
        if (returned > capacity)
            returned = capacity;
        for (uint64_t i = 0; i < returned; i++)
        {
            uint64_t item = offset + i;
            char name[BLOCK_DEVICE_NAME_MAX + 12];
            uint64_t size = 0;
            if (item < disk_count)
            {
                const block_device_t *device =
                    block_device_get((uint32_t)item);
                if (!device ||
                    device->sector_count >
                        UINT64_MAX / device->sector_size)
                    return -SYSCALL_EIO;
                uint64_t name_index = 0;
                while (device->name[name_index] &&
                       name_index < sizeof(name) - 1)
                {
                    name[name_index] = device->name[name_index];
                    name_index++;
                }
                name[name_index] = '\0';
                size = device->sector_count * device->sector_size;
            }
            else
            {
                uint64_t partition_index = item - disk_count;
                const mbr_partition_t *partition = 0;
                for (uint64_t j = 0; j < all_partitions; j++)
                {
                    const mbr_partition_t *candidate =
                        mbr_partition_get((uint32_t)j);
                    if (!candidate || !candidate->number)
                        continue;
                    if (!partition_index)
                    {
                        partition = candidate;
                        break;
                    }
                    partition_index--;
                }
                if (!partition)
                    return -SYSCALL_EIO;
                const block_device_t *device =
                    block_device_get(partition->device_index);
                if (!device ||
                    partition->sector_count >
                        UINT64_MAX / BLOCK_SECTOR_SIZE)
                    return -SYSCALL_EIO;
                uint64_t name_index = 0;
                while (device->name[name_index] &&
                       name_index < sizeof(name) - 1)
                {
                    name[name_index] = device->name[name_index];
                    name_index++;
                }
                if (partition->number)
                {
                    name[name_index++] = 'p';
                    uint32_t number = partition->number;
                    char digits[10];
                    uint32_t digit_count = 0;
                    do
                    {
                        digits[digit_count++] =
                            (char)('0' + number % 10);
                        number /= 10;
                    } while (number && digit_count < sizeof(digits));
                    while (digit_count)
                        name[name_index++] = digits[--digit_count];
                }
                name[name_index] = '\0';
                size = partition->sector_count * BLOCK_SECTOR_SIZE;
            }
            uint64_t name_index = 0;
            while (name[name_index] &&
                   name_index < sizeof(entries[i].name) - 1)
            {
                entries[i].name[name_index] = name[name_index];
                name_index++;
            }
            entries[i].name[name_index] = '\0';
            entries[i].size = size;
            entries[i].is_directory = false;
        }
        return (int64_t)returned;
    }

    if (path_equals(path, "/var/boot_logs"))
    {
        if (offset > 0 || !capacity)
            return 0;
        entries[0].name[0] = 'b';
        entries[0].name[1] = 'o';
        entries[0].name[2] = 'o';
        entries[0].name[3] = 't';
        entries[0].name[4] = '.';
        entries[0].name[5] = 't';
        entries[0].name[6] = 'x';
        entries[0].name[7] = 't';
        entries[0].name[8] = '\0';
        entries[0].size = serial_boot_log_size();
        entries[0].is_directory = false;
        return 1;
    }

    if (path_equals(path, "/mnt"))
    {
        total = fat32_volume_count;
        for (uint32_t i = 0; i < VFS_EXT2_VOLUME_LIMIT; i++)
        {
            if (ext2_volumes[i].mounted)
                total++;
        }
        if (offset >= total)
            return 0;
        uint64_t returned = total - offset;
        if (returned > capacity)
            returned = capacity;
        for (uint64_t i = 0; i < returned; i++)
        {
            uint64_t entry_index = offset + i;
            const char *mount_path;
            if (entry_index < fat32_volume_count)
                mount_path = fat32_volumes[entry_index].mount_path;
            else
            {
                entry_index -= fat32_volume_count;
                mount_path = NULL;
                for (uint32_t j = 0; j < VFS_EXT2_VOLUME_LIMIT; j++)
                {
                    if (!ext2_volumes[j].mounted)
                        continue;
                    if (entry_index == 0)
                    {
                        mount_path = ext2_volumes[j].mount_path;
                        break;
                    }
                    entry_index--;
                }
                if (!mount_path)
                    return -SYSCALL_EIO;
            }
            const char *name = mount_path + sizeof("/mnt/") - 1;
            uint64_t j = 0;
            while (name[j] && j < sizeof(entries[i].name) - 1)
            {
                entries[i].name[j] = name[j];
                j++;
            }
            entries[i].name[j] = '\0';
            entries[i].size = 0;
            entries[i].is_directory = true;
        }
        return (int64_t)returned;
    }

    if (path_equals(path, "/dev/pci") ||
        path_equals(path, "/dev/dri") ||
        path_equals(path, "/dev/snd"))
        return devfs_list(path, offset, entries, capacity);

    if (path_equals(path, "/sys/bus/pci/devices") ||
        path_has_mount_prefix(path, "/sys/bus/pci/devices"))
        return sysfs_list(path, offset, entries, capacity);

    if (path_equals(path, "/var"))
    {
        nodes = var_root;
        total = sizeof(var_root) / sizeof(var_root[0]);
    }
    else if (path_equals(path, "/var/boot_logs"))
    {
        nodes = var_boot_logs;
        total = sizeof(var_boot_logs) / sizeof(var_boot_logs[0]);
    }
    else if (path_equals(path, "/var/systemd"))
    {
        nodes = var_systemd;
        total = sizeof(var_systemd) / sizeof(var_systemd[0]);
    }
    else if (path_equals(path, "/proc"))
    {
        nodes = proc_root;
        total = sizeof(proc_root) / sizeof(proc_root[0]);
    }
    else if (procfs_is_directory(path))
    {
        nodes = proc_process;
        total = sizeof(proc_process) / sizeof(proc_process[0]);
    }
    else if (path_equals(path, "/sys"))
    {
        nodes = sys_root;
        total = sizeof(sys_root) / sizeof(sys_root[0]);
    }
    else if (path_equals(path, "/sys/kernel"))
    {
        nodes = sys_kernel;
        total = sizeof(sys_kernel) / sizeof(sys_kernel[0]);
    }
    else if (path_equals(path, "/sys/devices"))
    {
        nodes = sys_devices;
        total = sizeof(sys_devices) / sizeof(sys_devices[0]);
    }
    else if (path_equals(path, "/sys/devices/system"))
    {
        nodes = sys_system;
        total = sizeof(sys_system) / sizeof(sys_system[0]);
    }
    else if (path_equals(path, "/sys/devices/system/cpu"))
    {
        nodes = sys_cpu;
        total = sizeof(sys_cpu) / sizeof(sys_cpu[0]);
    }
    else if (path_equals(path, "/sys/class"))
    {
        nodes = sys_class;
        total = sizeof(sys_class) / sizeof(sys_class[0]);
    }
    else if (path_equals(path, "/sys/class/tty"))
    {
        nodes = sys_tty;
        total = sizeof(sys_tty) / sizeof(sys_tty[0]);
    }
    else if (path_equals(path, "/sys/block"))
    {
        nodes = sys_block;
        total = sizeof(sys_block) / sizeof(sys_block[0]);
    }
    else if (path_equals(path, "/sys/block/sr0"))
    {
        nodes = sys_sr0;
        total = sizeof(sys_sr0) / sizeof(sys_sr0[0]);
    }
    else if (path_equals(path, "/sys/block/sr0/device"))
    {
        nodes = sys_device;
        total = sizeof(sys_device) / sizeof(sys_device[0]);
    }
    else if (path_equals(path, "/sys/bus"))
    {
        static const virtual_entry_t sys_bus[] = {
            { "pci", true }
        };
        nodes = sys_bus;
        total = sizeof(sys_bus) / sizeof(sys_bus[0]);
    }
    else if (path_equals(path, "/sys/bus/pci"))
    {
        static const virtual_entry_t sys_pci[] = {
            { "devices", true }
        };
        nodes = sys_pci;
        total = sizeof(sys_pci) / sizeof(sys_pci[0]);
    }
    else if (path_equals(path, "/dev"))
    {
        nodes = dev_root;
        total = sizeof(dev_root) / sizeof(dev_root[0]);
    }
    else if (path_equals(path, "/dev/input"))
    {
        nodes = dev_input;
        total = sizeof(dev_input) / sizeof(dev_input[0]);
    }
    else if (path_equals(path, "/dev/disk") ||
             path_equals(path, "/dev/pts"))
    {
        nodes = dev_empty;
    }
    else
    {
        return -SYSCALL_ENOENT;
    }

    if (offset >= total)
        return 0;
    uint64_t returned = total - offset;
    if (returned > capacity)
        returned = capacity;
    for (uint64_t i = 0; i < returned; i++)
    {
        uint64_t j = 0;
        const char *name = nodes[offset + i].name;
        while (name[j] && j < sizeof(entries[i].name) - 1)
        {
            entries[i].name[j] = name[j];
            j++;
        }
        entries[i].name[j] = '\0';
        entries[i].size = 0;
        entries[i].is_directory = nodes[offset + i].is_directory;
    }
    return (int64_t)returned;
}

bool vfs_is_directory(const char *path)
{
    return path_is_valid(path) && vfs_is_directory_internal(path);
}

int64_t vfs_list(
    const char *path,
    uint64_t offset,
    vfs_dirent_t *entries,
    uint64_t capacity
)
{
    if (!path_is_valid(path) || (capacity && !entries))
        return -SYSCALL_EINVAL;
    if (!vfs_is_directory_internal(path))
        return -SYSCALL_ENOENT;

    if (path_has_mount_prefix(path, "/proc") ||
        path_has_mount_prefix(path, "/sys") ||
        path_has_mount_prefix(path, "/dev") ||
        path_has_mount_prefix(path, "/var") ||
        path_equals(path, "/mnt"))
        return list_virtual_directory(path, offset, entries, capacity);

    fat32_volume_t *fat_volume;
    const char *fat_relative_path;
    if (find_fat32_volume(path, &fat_volume, &fat_relative_path))
    {
        fat32_dirent_t fat_entries[16];
        uint64_t fat_capacity = capacity > 16 ? 16 : capacity;
        int64_t count = fat32_list(
            fat_volume,
            fat_relative_path,
            offset,
            fat_entries,
            fat_capacity);
        if (count < 0)
            return -SYSCALL_EIO;
        for (int64_t i = 0; i < count; i++)
        {
            uint64_t j = 0;
            while (fat_entries[i].name[j] &&
                   j < sizeof(entries[i].name) - 1)
            {
                entries[i].name[j] = fat_entries[i].name[j];
                j++;
            }
            entries[i].name[j] = '\0';
            entries[i].size = fat_entries[i].size;
            entries[i].is_directory = fat_entries[i].is_directory;
        }
        return count;
    }
    ext2_volume_t *ext2_volume;
    const char *ext2_relative_path;
    if (find_ext2_volume(path, &ext2_volume, &ext2_relative_path))
    {
        ext2_dirent_t ext2_entries[16];
        uint64_t ext2_capacity = capacity > 16 ? 16 : capacity;
        int64_t count = ext2_list(
            ext2_volume,
            ext2_relative_path,
            offset,
            ext2_entries,
            ext2_capacity);
        if (count < 0)
            return count;
        for (int64_t i = 0; i < count; i++)
        {
            uint64_t j = 0;
            while (ext2_entries[i].name[j] &&
                   j < sizeof(entries[i].name) - 1)
            {
                entries[i].name[j] = ext2_entries[i].name[j];
                j++;
            }
            entries[i].name[j] = '\0';
            entries[i].size = ext2_entries[i].size;
            entries[i].is_directory = ext2_entries[i].is_directory;
        }
        return count;
    }

    uint64_t root_total = 0;
    uint64_t root_returned = 0;
    if (initramfs_ready)
    {
        const initramfs_entry_t *directory = initramfs_find(path);
        if (path_equals(path, "/") ||
            (directory && directory->is_directory))
        {
            if (!initramfs_list(
                    path,
                    offset,
                    entries,
                    capacity,
                    &root_total))
                return -SYSCALL_EIO;
            root_returned = offset < root_total ? root_total - offset : 0;
            if (root_returned > capacity)
                root_returned = capacity;
        }
        else if (!ram_directory_exists(path))
        {
            return -SYSCALL_ENOENT;
        }
    }
    else
    {
        iso9660_file_t directory;
        if (root_mounted && iso9660_find(path, &directory) &&
            directory.is_directory)
        {
            iso9660_dirent_t iso_entries[16];
            uint64_t iso_total = 0;
            uint64_t iso_capacity = capacity > 16 ? 16 : capacity;
            if (!iso9660_list(
                    path,
                    offset,
                    iso_entries,
                    iso_capacity,
                    &iso_total))
                return -SYSCALL_EIO;

            root_total = iso_total;
            root_returned = offset < root_total ? root_total - offset : 0;
            if (root_returned > iso_capacity)
                root_returned = iso_capacity;
            for (uint64_t i = 0; i < root_returned; i++)
            {
                uint64_t j = 0;
                while (iso_entries[i].name[j] &&
                       j < sizeof(entries[i].name) - 1)
                {
                    entries[i].name[j] = iso_entries[i].name[j];
                    j++;
                }
                entries[i].name[j] = '\0';
                entries[i].size = iso_entries[i].size;
                entries[i].is_directory = iso_entries[i].is_directory;
            }
        }
    }

    uint64_t count = 0;
    uint64_t position = 0;
    for (uint32_t i = 0; i < VFS_RAM_DIR_LIMIT; i++)
    {
        if (!ram_directories[i].used)
            continue;

        char name[ISO9660_MAX_NAME];
        if (!direct_child_name(path, ram_directories[i].path, name))
            continue;

        if (initramfs_ready && initramfs_has_child(path, name))
            continue;
        uint64_t index = root_total + position++;
        if (index < offset || root_returned + count >= capacity)
            continue;

        uint64_t j = 0;
        while (name[j] && j < sizeof(entries[0].name) - 1)
        {
            entries[root_returned + count].name[j] = name[j];
            j++;
        }
        entries[root_returned + count].name[j] = '\0';
        entries[root_returned + count].size = 0;
        entries[root_returned + count].is_directory = true;
        count++;
    }
    position = 0;
    for (uint32_t i = 0; i < VFS_RAM_FILE_LIMIT; i++)
    {
        if (!ram_files[i].used)
            continue;
        char name[ISO9660_MAX_NAME];
        if (!direct_child_name(path, ram_files[i].path, name))
            continue;
        if (initramfs_ready && initramfs_has_child(path, name))
            continue;
        bool duplicate_directory = false;
        for (uint32_t j = 0; j < VFS_RAM_DIR_LIMIT; j++)
        {
            char directory_name[ISO9660_MAX_NAME];
            if (ram_directories[j].used &&
                direct_child_name(path, ram_directories[j].path,
                    directory_name) &&
                path_equals(name, directory_name))
            {
                duplicate_directory = true;
                break;
            }
        }
        if (duplicate_directory)
            continue;
        uint64_t index = root_total + count + position++;
        if (index < offset || root_returned + count >= capacity)
            continue;
        uint64_t j = 0;
        while (name[j] && j < sizeof(entries[0].name) - 1)
        {
            entries[root_returned + count].name[j] = name[j];
            j++;
        }
        entries[root_returned + count].name[j] = '\0';
        entries[root_returned + count].size = ram_files[i].size;
        entries[root_returned + count].is_directory = false;
        count++;
    }
    return (int64_t)(root_returned + count);
}

static bool parent_path(const char *path, char *parent)
{
    uint64_t length = 0;
    while (path[length])
        length++;
    if (length <= 1)
        return false;

    uint64_t end = length;
    while (end > 1 && path[end - 1] != '/')
        end--;
    uint64_t parent_length = end == 1 ? 1 : end - 1;
    for (uint64_t i = 0; i < parent_length; i++)
        parent[i] = path[i];
    parent[parent_length] = '\0';
    return true;
}

int64_t vfs_mkdir(const char *path)
{
    if (!path_is_valid(path) || path_equals(path, "/"))
        return -SYSCALL_EINVAL;
    fat32_volume_t *fat_volume;
    const char *fat_relative_path;
    if (find_fat32_volume(path, &fat_volume, &fat_relative_path))
        return fat32_mkdir(fat_volume, fat_relative_path);
    ext2_volume_t *ext2_volume;
    const char *ext2_relative_path;
    if (find_ext2_volume(path, &ext2_volume, &ext2_relative_path))
        return ext2_mkdir(ext2_volume, ext2_relative_path);
    if (path_has_mount_prefix(path, "/dev") ||
        path_has_mount_prefix(path, "/proc") ||
        path_has_mount_prefix(path, "/sys") ||
        path_has_mount_prefix(path, "/var"))
        return -SYSCALL_EROFS;
    if (vfs_is_directory_internal(path))
        return -SYSCALL_EEXIST;
    if (ram_file_find(path) >= 0)
        return -SYSCALL_EEXIST;

    iso9660_file_t existing;
    if (root_mounted && iso9660_find(path, &existing))
        return -SYSCALL_EEXIST;
    if (initramfs_find(path))
        return -SYSCALL_EEXIST;

    char parent[VFS_PATH_MAX];
    if (!parent_path(path, parent) ||
        !vfs_is_directory_internal(parent))
        return -SYSCALL_ENOENT;

    uint32_t slot = VFS_RAM_DIR_LIMIT;
    for (uint32_t i = 0; i < VFS_RAM_DIR_LIMIT; i++)
    {
        if (!ram_directories[i].used)
        {
            slot = i;
            break;
        }
    }
    if (slot == VFS_RAM_DIR_LIMIT)
        return -SYSCALL_ENOMEM;

    uint64_t i = 0;
    while (path[i])
    {
        ram_directories[slot].path[i] = path[i];
        i++;
    }
    ram_directories[slot].path[i] = '\0';
    ram_directories[slot].used = true;
    return 0;
}

int64_t vfs_rmdir(const char *path)
{
    if (!path_is_valid(path) || path_equals(path, "/"))
        return -SYSCALL_EINVAL;
    if (path_equals(path, "/tmp") ||
        path_equals(path, "/var") ||
        path_equals(path, "/var/boot_logs"))
        return -SYSCALL_EBUSY;
    if (path_has_mount_prefix(path, "/dev") ||
        path_has_mount_prefix(path, "/proc") ||
        path_has_mount_prefix(path, "/sys") ||
        path_has_mount_prefix(path, "/var"))
        return -SYSCALL_EROFS;

    ext2_volume_t *ext2_volume;
    const char *ext2_relative_path;
    if (find_ext2_volume(path, &ext2_volume, &ext2_relative_path))
        return ext2_rmdir(ext2_volume, ext2_relative_path);

    uint32_t slot = VFS_RAM_DIR_LIMIT;
    for (uint32_t i = 0; i < VFS_RAM_DIR_LIMIT; i++)
    {
        if (ram_directories[i].used &&
            path_equals(ram_directories[i].path, path))
        {
            slot = i;
            break;
        }
    }
    if (slot == VFS_RAM_DIR_LIMIT)
    {
        const initramfs_entry_t *entry = initramfs_find(path);
        if (entry)
            return entry->is_directory
                ? -SYSCALL_EROFS
                : -SYSCALL_ENOTDIR;
        iso9660_file_t existing;
        if (root_mounted && iso9660_find(path, &existing))
            return existing.is_directory
                ? -SYSCALL_EROFS
                : -SYSCALL_ENOTDIR;
        return -SYSCALL_ENOENT;
    }

    for (uint32_t i = 0; i < VFS_RAM_DIR_LIMIT; i++)
    {
        char name[ISO9660_MAX_NAME];
        if (i != slot && ram_directories[i].used &&
            direct_child_name(ram_directories[slot].path,
                ram_directories[i].path, name))
            return -SYSCALL_ENOTEMPTY;
    }
    for (uint32_t i = 0; i < VFS_RAM_FILE_LIMIT; i++)
    {
        char name[ISO9660_MAX_NAME];
        if (ram_files[i].used &&
            direct_child_name(ram_directories[slot].path,
                ram_files[i].path, name))
            return -SYSCALL_ENOTEMPTY;
    }

    if (root_mounted)
    {
        iso9660_dirent_t child;
        uint64_t total = 0;
        if (iso9660_list(path, 0, &child, 1, &total) && total)
            return -SYSCALL_ENOTEMPTY;
    }
    for (uint32_t i = 0; i < initramfs_entry_count; i++)
    {
        char name[ISO9660_MAX_NAME];
        if (direct_child_name(
                ram_directories[slot].path,
                initramfs_entries[i].path,
                name))
            return -SYSCALL_ENOTEMPTY;
    }

    ram_directories[slot].used = false;
    ram_directories[slot].path[0] = '\0';
    return 0;
}

int64_t vfs_unlink(const char *path)
{
    if (!path_is_valid(path) || path_equals(path, "/"))
        return -SYSCALL_EINVAL;
    if (path_equals(path, "/tmp") ||
        path_equals(path, "/var") ||
        path_has_mount_prefix(path, "/dev") ||
        path_has_mount_prefix(path, "/proc") ||
        path_has_mount_prefix(path, "/sys") ||
        path_has_mount_prefix(path, "/var"))
        return -SYSCALL_EROFS;

    fat32_volume_t *fat_volume;
    const char *fat_relative_path;
    if (find_fat32_volume(path, &fat_volume, &fat_relative_path))
        return -SYSCALL_ENOTSUP;

    ext2_volume_t *ext2_volume;
    const char *ext2_relative_path;
    if (find_ext2_volume(path, &ext2_volume, &ext2_relative_path))
        return ext2_unlink(ext2_volume, ext2_relative_path);

    if (vfs_is_directory_internal(path))
        return -SYSCALL_EISDIR;
    if (ram_file_find(path) >= 0)
        return -SYSCALL_ENOTSUP;
    if (initramfs_find(path))
        return -SYSCALL_EROFS;
    iso9660_file_t existing;
    if (root_mounted && iso9660_find(path, &existing))
        return existing.is_directory
            ? -SYSCALL_EISDIR
            : -SYSCALL_EROFS;
    return -SYSCALL_ENOENT;
}
