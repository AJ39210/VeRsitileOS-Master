#ifndef DEVFS_H
#define DEVFS_H

#include <stdbool.h>
#include <stdint.h>

#include "iso9660.h"
#include "vfs.h"

void devfs_register_cdrom(
    iso9660_sector_reader_t reader,
    void *context,
    uint32_t sector_count
);
bool devfs_is_directory(const char *path);
bool devfs_file_size(const char *path, uint64_t *size);
int64_t devfs_list(
    const char *path,
    uint64_t offset,
    vfs_dirent_t *entries,
    uint64_t capacity
);
int64_t devfs_read(
    const char *path,
    uint64_t offset,
    void *buffer,
    uint64_t size
);
int64_t devfs_write(
    const char *path,
    uint64_t offset,
    const void *buffer,
    uint64_t size
);

#endif
