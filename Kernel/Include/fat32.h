#ifndef FAT32_H
#define FAT32_H

#include <stdbool.h>
#include <stdint.h>

#define FAT32_NAME_MAX 255
#define FAT32_VOLUME_NAME_MAX 32

typedef struct
{
    uint32_t device_index;
    uint64_t partition_lba;
    uint64_t partition_sectors;
    uint32_t sectors_per_cluster;
    uint32_t reserved_sectors;
    uint32_t fat_sectors;
    uint32_t first_data_sector;
    uint32_t cluster_count;
    uint32_t root_cluster;
    uint32_t active_fat;
    uint32_t fat_count;
    uint32_t next_free_cluster;
    uint32_t free_clusters;
    uint32_t fs_info_sector;
    uint32_t backup_boot_sector;
    bool fat_mirroring;
    bool mounted;
    bool writable;
    char mount_path[FAT32_VOLUME_NAME_MAX];
} fat32_volume_t;

typedef struct
{
    char name[FAT32_NAME_MAX + 1];
    uint64_t size;
    uint32_t first_cluster;
    uint64_t entry_lba;
    uint16_t entry_offset;
    bool is_directory;
} fat32_dirent_t;

typedef struct
{
    fat32_volume_t *volume;
    uint32_t first_cluster;
    uint64_t size;
    uint64_t offset;
    uint64_t entry_lba;
    uint16_t entry_offset;
    bool is_directory;
} fat32_file_t;

bool fat32_mount(
    fat32_volume_t *volume,
    uint32_t device_index,
    uint64_t partition_lba,
    uint64_t partition_sectors,
    const char *mount_path
);
bool fat32_format(
    uint32_t device_index,
    uint64_t partition_lba,
    uint64_t partition_sectors,
    const char *volume_label
);
const char *fat32_format_error(void);
void fat32_unmount(fat32_volume_t *volume);
bool fat32_path_matches(
    const fat32_volume_t *volume,
    const char *path,
    const char **relative_path
);
bool fat32_is_directory(fat32_volume_t *volume, const char *path);
int64_t fat32_open(
    fat32_volume_t *volume,
    const char *path,
    fat32_file_t *file
);
int64_t fat32_open_write(
    fat32_volume_t *volume,
    const char *path,
    bool create,
    bool truncate,
    fat32_file_t *file
);
int64_t fat32_read(fat32_file_t *file, void *buffer, uint64_t size);
int64_t fat32_write(fat32_file_t *file, const void *buffer, uint64_t size);
int64_t fat32_mkdir(fat32_volume_t *volume, const char *path);
int64_t fat32_list(
    fat32_volume_t *volume,
    const char *path,
    uint64_t offset,
    fat32_dirent_t *entries,
    uint64_t capacity
);

#endif
