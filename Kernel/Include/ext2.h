#ifndef EXT2_H
#define EXT2_H

#include <stdbool.h>
#include <stdint.h>

#define EXT2_NAME_MAX 255
#define EXT2_MOUNT_PATH_MAX 256

typedef struct
{
    uint32_t device_index;
    uint64_t partition_lba;
    uint64_t partition_sectors;
    uint32_t block_size;
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint32_t inode_size;
    uint32_t first_data_block;
    uint32_t group_count;
    uint32_t gdt_start;
    uint32_t blocks_count;
    uint32_t inodes_count;
    uint32_t feature_compat;
    uint32_t feature_incompat;
    uint32_t feature_ro_compat;
    uint32_t open_files;
    uint32_t allocation_group;
    uint32_t allocation_next_bit;
    uint32_t allocation_pending;
    uint8_t allocation_descriptor[32];
    uint8_t allocation_bitmap[4096];
    bool allocation_cache_valid;
    bool metadata_dirty;
    char volume_name[17];
    bool mounted;
    bool writable;
    char mount_path[EXT2_MOUNT_PATH_MAX];
} ext2_volume_t;

typedef struct
{
    char name[EXT2_NAME_MAX + 1];
    uint64_t size;
    uint32_t inode;
    bool is_directory;
} ext2_dirent_t;

typedef struct
{
    ext2_volume_t *volume;
    uint32_t inode;
    uint64_t size;
    uint64_t offset;
    bool is_directory;
    bool open;
} ext2_file_t;

bool ext2_format(
    uint32_t device_index,
    uint64_t partition_lba,
    uint64_t partition_sectors,
    const char *volume_name
);
bool ext2_mount(
    ext2_volume_t *volume,
    uint32_t device_index,
    uint64_t partition_lba,
    uint64_t partition_sectors,
    const char *mount_path
);
bool ext2_sync(ext2_volume_t *volume);
void ext2_unmount(ext2_volume_t *volume);
bool ext2_path_matches(
    const ext2_volume_t *volume,
    const char *path,
    const char **relative_path
);
bool ext2_is_directory(ext2_volume_t *volume, const char *path);
int64_t ext2_open(
    ext2_volume_t *volume,
    const char *path,
    bool create,
    bool truncate,
    ext2_file_t *file
);
int64_t ext2_read(ext2_file_t *file, void *buffer, uint64_t size);
int64_t ext2_write(ext2_file_t *file, const void *buffer, uint64_t size);
bool ext2_clone_file(ext2_file_t *destination, const ext2_file_t *source);
void ext2_close(ext2_file_t *file);
int64_t ext2_list(
    ext2_volume_t *volume,
    const char *path,
    uint64_t offset,
    ext2_dirent_t *entries,
    uint64_t capacity
);
int64_t ext2_mkdir(ext2_volume_t *volume, const char *path);
int64_t ext2_rmdir(ext2_volume_t *volume, const char *path);
int64_t ext2_unlink(ext2_volume_t *volume, const char *path);

#endif
