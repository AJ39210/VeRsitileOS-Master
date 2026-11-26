
#ifndef ISO9660_H
#define ISO9660_H

#include <stdint.h>
#include <stdbool.h>

#define ISO9660_SECTOR_SIZE 2048
#define ISO9660_MAX_NAME 256

typedef bool (*iso9660_sector_reader_t)(
    void *context,
    uint32_t lba,
    uint8_t *buffer
);

typedef struct
{
    char name[ISO9660_MAX_NAME];

    uint32_t lba;
    uint32_t size;

    bool is_directory;
} iso9660_file_t;

typedef struct
{
    char name[ISO9660_MAX_NAME];
    uint32_t size;
    bool is_directory;
} iso9660_dirent_t;

/* Initialize using a low-level sector reader. */
bool iso9660_init(
    iso9660_sector_reader_t reader,
    void *context
);

/* Check filesystem initialization. */
bool iso9660_is_ready(void);
uint32_t iso9660_get_volume_sectors(void);

/* Get the root directory. */
bool iso9660_get_root(iso9660_file_t *root);

/* Find a file or directory using an absolute path. */
bool iso9660_find(
    const char *path,
    iso9660_file_t *result
);

/* Read bytes from a file. */
bool iso9660_read(
    const iso9660_file_t *file,
    uint32_t offset,
    uint32_t size,
    void *buffer
);
bool iso9660_list(
    const char *path,
    uint64_t offset,
    iso9660_dirent_t *entries,
    uint64_t capacity,
    uint64_t *total
);

#endif
