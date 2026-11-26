#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "block.h"
#include "ext2.h"
#include "fat32.h"
#include "installer.h"
#include "mbr.h"
#include "serial.h"
#include "vfs.h"

extern const uint8_t binary_limine_hdd_bin_data[];

#define TEST_DISK_SECTORS 600000U
#define TEST_NODE_LIMIT 192
#define TEST_NODE_DATA 16384
#define TEST_STORED_SECTOR_LIMIT 512

typedef struct
{
    bool used;
    bool directory;
    char path[256];
    uint8_t data[TEST_NODE_DATA];
    uint64_t size;
} test_node_t;

typedef struct
{
    bool used;
    uint64_t lba;
    uint8_t data[BLOCK_SECTOR_SIZE];
} test_sector_t;

static test_node_t nodes[TEST_NODE_LIMIT];
static test_sector_t stored_sectors[TEST_STORED_SECTOR_LIMIT];
static uint32_t stored_sector_count;
static block_device_t test_device = {
    .name = "vd0",
    .sector_count = TEST_DISK_SECTORS,
    .sector_size = BLOCK_SECTOR_SIZE,
    .writable = true
};
static mbr_gpt_partition_t written_partitions[3];
static uint32_t written_partition_count;
static uint32_t gpt_write_count;
static uint32_t fat_format_count;
static uint32_t ext2_format_count;
static uint32_t progress_percent;
static bool progress_started;
static bool progress_failed;
static bool progress_completed;

static int fail(const char *message)
{
    fprintf(stderr, "installer test failed: %s\n", message);
    return 1;
}

static int node_find(const char *path)
{
    for (uint32_t i = 0; i < TEST_NODE_LIMIT; i++)
    {
        if (nodes[i].used && strcmp(nodes[i].path, path) == 0)
            return (int)i;
    }
    return -1;
}

static int node_create(const char *path, bool directory)
{
    int existing = node_find(path);
    if (existing >= 0)
        return nodes[existing].directory == directory ? existing : -1;
    for (uint32_t i = 0; i < TEST_NODE_LIMIT; i++)
    {
        if (nodes[i].used)
            continue;
        size_t length = strlen(path);
        if (!length || length >= sizeof(nodes[i].path))
            return -1;
        nodes[i].used = true;
        nodes[i].directory = directory;
        memcpy(nodes[i].path, path, length + 1);
        return (int)i;
    }
    return -1;
}

static bool path_parent(const char *path, char parent[256])
{
    size_t length = strlen(path);
    if (length <= 1)
        return false;
    size_t end = length;
    while (end > 1 && path[end - 1] != '/')
        end--;
    size_t parent_length = end == 1 ? 1 : end - 1;
    memcpy(parent, path, parent_length);
    parent[parent_length] = '\0';
    return true;
}

static bool add_directory(const char *path)
{
    if (strcmp(path, "/") == 0)
        return node_create("/", true) >= 0;
    char parent[256];
    if (!path_parent(path, parent))
        return false;
    int parent_index = node_find(parent);
    return parent_index >= 0 && nodes[parent_index].directory &&
        node_create(path, true) >= 0;
}

static bool add_file(const char *path, const char *contents)
{
    char parent[256];
    if (!path_parent(path, parent))
        return false;
    int parent_index = node_find(parent);
    int file_index = node_create(path, false);
    if (parent_index < 0 || !nodes[parent_index].directory ||
        file_index < 0)
        return false;
    size_t size = strlen(contents);
    if (size > sizeof(nodes[file_index].data))
        return false;
    memcpy(nodes[file_index].data, contents, size);
    nodes[file_index].size = size;
    return true;
}

static bool write_tar_octal(uint8_t *field, size_t length, uint64_t value)
{
    if (!field || length < 2)
        return false;
    field[length - 1] = '\0';
    for (size_t i = length - 1; i > 0; i--)
    {
        field[i - 1] = (uint8_t)('0' + value % 8);
        value /= 8;
    }
    return value == 0;
}

static bool append_tar_entry(
    uint8_t *archive,
    size_t capacity,
    size_t *offset,
    const char *path,
    const char *contents,
    bool directory
)
{
    uint8_t header[512] = {0};
    size_t path_length = strlen(path);
    size_t content_length = contents ? strlen(contents) : 0;
    if (!path_length || path_length >= 100 ||
        (!directory && content_length > capacity))
        return false;
    memcpy(header, path, path_length);
    memcpy(header + 100, directory ? "0000755" : "0000644", 7);
    memcpy(header + 108, "0000000", 7);
    memcpy(header + 116, "0000000", 7);
    if (!write_tar_octal(header + 124, 12, content_length))
        return false;
    memcpy(header + 136, "00000000000", 11);
    header[156] = directory ? '5' : '0';
    memcpy(header + 257, "ustar", 5);
    memcpy(header + 263, "00", 2);
    memset(header + 148, ' ', 8);
    unsigned checksum = 0;
    for (size_t i = 0; i < sizeof(header); i++)
        checksum += header[i];
    if (!write_tar_octal(header + 148, 7, checksum))
        return false;
    header[154] = '\0';
    header[155] = ' ';

    uint64_t padded_content =
        (content_length + 511U) & ~(uint64_t)511U;
    if (*offset > capacity ||
        sizeof(header) + padded_content > capacity - *offset)
        return false;
    memcpy(archive + *offset, header, sizeof(header));
    *offset += sizeof(header);
    if (content_length)
        memcpy(archive + *offset, contents, content_length);
    memset(archive + *offset + content_length, 0,
        (size_t)(padded_content - content_length));
    *offset += (size_t)padded_content;
    return true;
}

static bool add_rootfs_archive(void)
{
    uint8_t archive[TEST_NODE_DATA] = {0};
    size_t offset = 0;
    static const struct
    {
        const char *path;
        const char *contents;
        bool directory;
    } entries[] = {
        { "./", NULL, true },
        { "./bin/", NULL, true },
        { "./bin/terminal", "installed terminal", false },
        { "./etc/", NULL, true },
        { "./etc/system/", NULL, true },
        { "./etc/system/desktop", "Unknown / terminal\n", false },
        { "./etc/system/os_name", "VeRsitileOS\n", false },
        { "./etc/system/users/", NULL, true },
        { "./home/", NULL, true },
        { "./tmp/", NULL, true },
        { "./tmp/live-temporary-file", "from rootfs archive", false }
    };
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++)
    {
        if (!append_tar_entry(
                archive,
                sizeof(archive),
                &offset,
                entries[i].path,
                entries[i].contents,
                entries[i].directory))
            return false;
    }
    if (offset > sizeof(archive) - 1024)
        return false;
    memset(archive + offset, 0, 1024);
    offset += 1024;
    if (!add_directory("/system") ||
        !add_directory("/system/install"))
        return false;
    int archive_node = node_create("/system/install/rootfs.tar", false);
    if (archive_node < 0 || offset > sizeof(nodes[archive_node].data))
        return false;
    memcpy(nodes[archive_node].data, archive, offset);
    nodes[archive_node].size = offset;
    return true;
}

static bool direct_child(
    const char *parent,
    const char *path,
    char name[256]
)
{
    size_t parent_length = strlen(parent);
    const char *remainder;
    if (strcmp(parent, "/") == 0)
    {
        if (path[0] != '/')
            return false;
        remainder = path + 1;
    }
    else
    {
        if (strncmp(parent, path, parent_length) != 0 ||
            path[parent_length] != '/')
            return false;
        remainder = path + parent_length + 1;
    }
    if (!*remainder || strchr(remainder, '/'))
        return false;
    strcpy(name, remainder);
    return true;
}

const block_device_t *block_device_get(uint32_t index)
{
    return index == 0 ? &test_device : NULL;
}

uint32_t block_device_count(void)
{
    return 1;
}

bool block_read(uint32_t index, uint64_t lba, uint32_t count, void *buffer)
{
    if (index || !count || !buffer ||
        lba >= test_device.sector_count ||
        count > test_device.sector_count - lba)
        return false;
    uint8_t *output = buffer;
    for (uint32_t sector = 0; sector < count; sector++)
    {
        bool found = false;
        for (uint32_t i = 0; i < stored_sector_count; i++)
        {
            if (stored_sectors[i].used &&
                stored_sectors[i].lba == lba + sector)
            {
                memcpy(
                    output + (size_t)sector * BLOCK_SECTOR_SIZE,
                    stored_sectors[i].data,
                    BLOCK_SECTOR_SIZE);
                found = true;
                break;
            }
        }
        if (!found)
            memset(
                output + (size_t)sector * BLOCK_SECTOR_SIZE,
                0,
                BLOCK_SECTOR_SIZE);
    }
    return true;
}

bool block_write(
    uint32_t index,
    uint64_t lba,
    uint32_t count,
    const void *buffer
)
{
    if (index || !count || !buffer ||
        lba >= test_device.sector_count ||
        count > test_device.sector_count - lba)
        return false;
    const uint8_t *input = buffer;
    for (uint32_t sector = 0; sector < count; sector++)
    {
        uint64_t target = lba + sector;
        uint32_t slot = TEST_STORED_SECTOR_LIMIT;
        for (uint32_t i = 0; i < stored_sector_count; i++)
        {
            if (stored_sectors[i].used &&
                stored_sectors[i].lba == target)
            {
                slot = i;
                break;
            }
        }
        if (slot == TEST_STORED_SECTOR_LIMIT)
        {
            if (stored_sector_count == TEST_STORED_SECTOR_LIMIT)
                return false;
            slot = stored_sector_count++;
            stored_sectors[slot].used = true;
            stored_sectors[slot].lba = target;
        }
        memcpy(
            stored_sectors[slot].data,
            input + (size_t)sector * BLOCK_SECTOR_SIZE,
            BLOCK_SECTOR_SIZE);
    }
    return true;
}

bool mbr_create_gpt(
    uint32_t device_index,
    const mbr_gpt_partition_t *partitions,
    uint32_t partition_count
)
{
    if (device_index || !partitions || partition_count != 3)
        return false;
    memcpy(
        written_partitions,
        partitions,
        sizeof(written_partitions));
    written_partition_count = partition_count;
    gpt_write_count++;
    uint8_t mbr[BLOCK_SECTOR_SIZE] = {0};
    mbr[446 + 4] = 0xEE;
    mbr[446 + 8] = 1;
    mbr[510] = 0x55;
    mbr[511] = 0xAA;
    return block_write(0, 0, 1, mbr);
}

bool fat32_format(
    uint32_t device_index,
    uint64_t partition_lba,
    uint64_t partition_sectors,
    const char *label
)
{
    if (device_index || !partition_lba || !partition_sectors ||
        strcmp(label, "VERSITILEOS") != 0)
        return false;
    fat_format_count++;
    return true;
}

const char *fat32_format_error(void)
{
    return NULL;
}

bool ext2_format(
    uint32_t device_index,
    uint64_t partition_lba,
    uint64_t partition_sectors,
    const char *label
)
{
    if (device_index || !partition_lba || !partition_sectors ||
        strcmp(label, "VeRsitileOS") != 0)
        return false;
    ext2_format_count++;
    return true;
}

void fat32_unmount(fat32_volume_t *volume)
{
    (void)volume;
}

void ext2_unmount(ext2_volume_t *volume)
{
    (void)volume;
}

bool vfs_root_uses_device(uint32_t device_index)
{
    (void)device_index;
    return false;
}

bool vfs_unmount_device_partitions(uint32_t device_index)
{
    return device_index == 0;
}

bool vfs_mount_ext2_partition(
    uint32_t device_index,
    uint64_t partition_lba,
    uint64_t partition_sectors,
    const char *mount_path
)
{
    (void)device_index;
    (void)partition_lba;
    (void)partition_sectors;
    return add_directory(mount_path);
}

bool vfs_mount_fat32_partition(
    uint32_t device_index,
    uint64_t partition_lba,
    uint64_t partition_sectors,
    const char *mount_path
)
{
    (void)device_index;
    (void)partition_lba;
    (void)partition_sectors;
    return add_directory(mount_path);
}

bool vfs_unmount_ext2(const char *mount_path)
{
    (void)mount_path;
    return true;
}

bool vfs_unmount_fat32(const char *mount_path)
{
    (void)mount_path;
    return true;
}

bool vfs_is_directory(const char *path)
{
    int index = node_find(path);
    return index >= 0 && nodes[index].directory;
}

int64_t vfs_mkdir(const char *path)
{
    return add_directory(path) ? 0 : -1;
}

int64_t vfs_open(const char *path, vfs_file_t *file)
{
    int index = node_find(path);
    if (index < 0)
        return -1;
    if (nodes[index].directory)
        return -1;
    memset(file, 0, sizeof(*file));
    strcpy(file->path, path);
    file->size = nodes[index].size;
    return 0;
}

int64_t vfs_open_write(
    const char *path,
    bool create,
    bool truncate,
    vfs_file_t *file
)
{
    int index = node_find(path);
    if (index < 0)
    {
        char parent[256];
        if (!create || !path_parent(path, parent) ||
            !vfs_is_directory(parent))
            return -1;
        index = node_create(path, false);
        if (index < 0)
            return -1;
    }
    if (nodes[index].directory)
        return -1;
    if (truncate)
        nodes[index].size = 0;
    memset(file, 0, sizeof(*file));
    strcpy(file->path, path);
    file->size = nodes[index].size;
    return 0;
}

int64_t vfs_read(vfs_file_t *file, void *buffer, uint64_t size)
{
    int index = node_find(file->path);
    if (index < 0 || nodes[index].directory)
        return -1;
    if (file->offset >= nodes[index].size)
        return 0;
    if (size > nodes[index].size - file->offset)
        size = nodes[index].size - file->offset;
    memcpy(buffer, nodes[index].data + file->offset, (size_t)size);
    file->offset += size;
    return (int64_t)size;
}

int64_t vfs_write(vfs_file_t *file, const void *buffer, uint64_t size)
{
    int index = node_find(file->path);
    if (index < 0 || nodes[index].directory ||
        size > sizeof(nodes[index].data) - file->offset)
        return -1;
    memcpy(nodes[index].data + file->offset, buffer, (size_t)size);
    file->offset += size;
    if (file->offset > nodes[index].size)
        nodes[index].size = file->offset;
    file->size = nodes[index].size;
    return (int64_t)size;
}

void vfs_close_file(vfs_file_t *file)
{
    (void)file;
}

int64_t vfs_list(
    const char *path,
    uint64_t offset,
    vfs_dirent_t *entries,
    uint64_t capacity
)
{
    if (!vfs_is_directory(path))
        return -1;
    uint64_t child_index = 0;
    uint64_t returned = 0;
    for (uint32_t i = 0; i < TEST_NODE_LIMIT; i++)
    {
        if (!nodes[i].used)
            continue;
        char name[256];
        if (!direct_child(path, nodes[i].path, name))
            continue;
        if (child_index++ < offset)
            continue;
        if (returned == capacity)
            break;
        memset(&entries[returned], 0, sizeof(entries[returned]));
        strcpy(entries[returned].name, name);
        entries[returned].size = nodes[i].size;
        entries[returned].is_directory = nodes[i].directory;
        returned++;
    }
    return (int64_t)returned;
}

void serial_log(serial_log_level_t level, const char *message)
{
    (void)level;
    (void)message;
}

void serial_log_begin(serial_log_level_t level)
{
    (void)level;
}

void serial_write(const char *text)
{
    (void)text;
}

void serial_write_dec(uint64_t value)
{
    (void)value;
}

void serial_log_end(void)
{
}

void serial_install_progress_begin(void)
{
    progress_started = true;
    progress_percent = 0;
}

void serial_install_progress_update(uint32_t percent)
{
    progress_percent = percent;
}

void serial_install_progress_error(const char *message)
{
    progress_failed = message != NULL;
}

void serial_install_progress_complete(void)
{
    progress_percent = 100;
    progress_completed = true;
}

static bool initialize_source_tree(void)
{
    static const char *const directories[] = {
        "/", "/boot", "/boot/limine", "/EFI", "/EFI/BOOT",
        "/etc", "/etc/system", "/bin", "/usr", "/usr/include",
        "/tmp", "/dev", "/proc", "/sys", "/mnt"
    };
    for (size_t i = 0; i < sizeof(directories) / sizeof(directories[0]); i++)
    {
        if (!add_directory(directories[i]))
            return false;
    }
    return add_file("/boot/kernel.elf", "test kernel image") &&
        add_file("/boot/limine/limine-bios.sys", "bios stage file") &&
        add_file("/EFI/BOOT/BOOTX64.EFI", "uefi boot file") &&
        add_file(
            "/etc/system/desktop",
            "Terminal based ( WM = n DE = n )\n") &&
        add_file("/etc/system/os_name", "VeRsitileOS\n") &&
        add_file("/bin/install", "installer executable") &&
        add_file("/bin/terminal", "terminal executable") &&
        add_file("/usr/include/sample.h", "sample header") &&
        add_file("/tmp/live-temporary-file", "copy requested temp data") &&
        add_rootfs_archive();
}

static bool file_equals(const char *path, const char *contents)
{
    int index = node_find(path);
    size_t length = strlen(contents);
    return index >= 0 && !nodes[index].directory &&
        nodes[index].size == length &&
        memcmp(nodes[index].data, contents, length) == 0;
}

int main(void)
{
    if (!initialize_source_tree())
        return fail("initialize installer source tree");

    os_install_request_t request = {
        .device = "vd0",
        .confirmation = "WRONG"
    };
    if (installer_run(&request) >= 0 ||
        gpt_write_count || fat_format_count || ext2_format_count ||
        stored_sector_count)
        return fail("reject unconfirmed disk destruction");

    strcpy(request.confirmation, "WIPE vd0");
    if (installer_run(&request) != 0)
        return fail("complete installation");
    if (!progress_started || progress_failed || !progress_completed ||
        progress_percent != 100)
        return fail("report successful install progress");
    if (written_partition_count != 3 ||
        written_partitions[0].start_lba != 2048 ||
        written_partitions[0].sector_count != 32768 ||
        written_partitions[1].start_lba != 34816 ||
        written_partitions[1].sector_count != 262144 ||
        written_partitions[2].start_lba != 296960 ||
        fat_format_count != 1 || ext2_format_count != 1)
        return fail("create expected GPT and filesystem layout");

    if (!vfs_is_directory("/mnt/installer-root/home"))
        return fail("extract rootfs home directory");
    if (!file_equals(
            "/mnt/installer-root/etc/system/desktop",
            "Unknown / terminal\n"))
        return fail("extract rootfs desktop config");
    if (!file_equals(
            "/mnt/installer-root/etc/system/os_name",
            "VeRsitileOS\n"))
        return fail("extract rootfs OS name");
    if (!vfs_is_directory("/mnt/installer-root/etc/system/users"))
        return fail("extract rootfs users directory");
    if (!file_equals(
            "/mnt/installer-root/tmp/live-temporary-file",
            "from rootfs archive"))
        return fail("extract rootfs file");
    if (!file_equals(
            "/mnt/installer-root/bin/terminal",
            "installed terminal"))
        return fail("extract installed terminal");
    if (node_find("/mnt/installer-root/bin/install") >= 0)
        return fail("do not install live installer");

    if (!file_equals(
            "/mnt/installer-efi/EFI/BOOT/BOOTX64.EFI",
            "uefi boot file") ||
        !file_equals(
            "/mnt/installer-efi/boot/kernel.elf",
            "test kernel image"))
        return fail("install EFI executable and kernel");
    int config_index = node_find(
        "/mnt/installer-efi/boot/limine/limine.conf");
    if (config_index < 0 ||
        !strstr((const char *)nodes[config_index].data,
                "root=VeRsitileOS"))
        return fail("write installed-root Limine config");

    uint8_t sector[BLOCK_SECTOR_SIZE];
    if (!block_read(0, 0, 1, sector) ||
        sector[510] != 0x55 || sector[511] != 0xAA ||
        sector[446 + 4] != 0xEE)
        return fail("preserve protective GPT MBR signature");
    uint64_t stage_byte_offset = 0;
    for (uint32_t i = 0; i < sizeof(stage_byte_offset); i++)
        stage_byte_offset |= (uint64_t)sector[0x1A4 + i] << (8 * i);
    if (stage_byte_offset != 2048ULL * BLOCK_SECTOR_SIZE ||
        !block_read(0, 2048, 1, sector) ||
        memcmp(sector, binary_limine_hdd_bin_data + BLOCK_SECTOR_SIZE,
               BLOCK_SECTOR_SIZE) != 0)
        return fail("install Limine BIOS boot stages");

    puts("Installer confirmation, GPT layout, rootfs extraction, UEFI, and BIOS passed");
    return 0;
}
