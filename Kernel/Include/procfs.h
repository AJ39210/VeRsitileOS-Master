#ifndef PROCFS_H
#define PROCFS_H

#include <stdbool.h>
#include <stdint.h>

bool procfs_is_directory(const char *path);
bool procfs_file_size(const char *path, uint64_t *size);
int64_t procfs_read(
    const char *path,
    uint64_t offset,
    void *buffer,
    uint64_t size
);

#endif
