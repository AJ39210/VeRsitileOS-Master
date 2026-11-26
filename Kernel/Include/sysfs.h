#ifndef SYSFS_H
#define SYSFS_H

#include <stdbool.h>
#include <stdint.h>

#include "vfs.h"

bool sysfs_is_directory(const char *path);
bool sysfs_file_size(const char *path, uint64_t *size);
int64_t sysfs_list(
    const char *path,
    uint64_t offset,
    vfs_dirent_t *entries,
    uint64_t capacity
);
int64_t sysfs_read(
    const char *path,
    uint64_t offset,
    void *buffer,
    uint64_t size
);

#endif
