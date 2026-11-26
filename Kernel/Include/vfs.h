#ifndef VFS_H
#define VFS_H

#include <stdbool.h>
#include <stdint.h>

#include "ext2.h"
#include "fat32.h"
#include "iso9660.h"

typedef enum
{
    VFS_BACKEND_NONE,
    VFS_BACKEND_ISO9660,
    VFS_BACKEND_INITRAMFS,
    VFS_BACKEND_RAMFS,
    VFS_BACKEND_FAT32,
    VFS_BACKEND_EXT2,
    VFS_BACKEND_DEVFS,
    VFS_BACKEND_PROCFS,
    VFS_BACKEND_SYSFS,
    VFS_BACKEND_BOOT_LOG,
    VFS_BACKEND_SYSTEMD_LOG
} vfs_backend_t;

typedef struct
{
    vfs_backend_t backend;
    iso9660_file_t iso_file;
    const uint8_t *initramfs_data;
    uint32_t ram_file_index;
    fat32_file_t fat32_file;
    ext2_file_t ext2_file;
    char path[256];
    uint64_t offset;
    uint64_t size;
} vfs_file_t;

typedef struct
{
    char name[256];
    uint64_t size;
    bool is_directory;
} vfs_dirent_t;

bool vfs_init(void);
bool vfs_set_initramfs(const void *archive, uint64_t size);
uint32_t vfs_mount_fat32_partitions(void);
bool vfs_mount_fat32_partition(
    uint32_t device_index,
    uint64_t partition_lba,
    uint64_t partition_sectors,
    const char *mount_path
);
bool vfs_unmount_fat32(const char *mount_path);
uint32_t vfs_mount_ext2_partitions(void);
bool vfs_mount_installed_root(void);
bool vfs_root_uses_device(uint32_t device_index);
bool vfs_unmount_device_partitions(uint32_t device_index);
bool vfs_mount_ext2_partition(
    uint32_t device_index,
    uint64_t partition_lba,
    uint64_t partition_sectors,
    const char *mount_path
);
bool vfs_unmount_ext2(const char *mount_path);
bool vfs_root_is_mounted(void);
bool vfs_clone_file(vfs_file_t *destination, const vfs_file_t *source);
void vfs_close_file(vfs_file_t *file);
int64_t vfs_open(const char *path, vfs_file_t *file);
int64_t vfs_open_write(
    const char *path,
    bool create,
    bool truncate,
    vfs_file_t *file
);
int64_t vfs_read(vfs_file_t *file, void *buffer, uint64_t size);
int64_t vfs_write(vfs_file_t *file, const void *buffer, uint64_t size);
int64_t vfs_list(
    const char *path,
    uint64_t offset,
    vfs_dirent_t *entries,
    uint64_t capacity
);
int64_t vfs_mkdir(const char *path);
int64_t vfs_rmdir(const char *path);
int64_t vfs_unlink(const char *path);
bool vfs_is_directory(const char *path);

#endif
