#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "block.h"
#include "ext2.h"

#define TEST_SECTORS 524288U
#define TEST_BYTES ((size_t)TEST_SECTORS * BLOCK_SECTOR_SIZE)

static uint8_t disk[TEST_BYTES];
static block_device_t test_device = {
    .name = "test0",
    .sector_count = TEST_SECTORS,
    .sector_size = BLOCK_SECTOR_SIZE,
    .writable = true
};

const block_device_t *block_device_get(uint32_t index)
{
    return index == 0 ? &test_device : NULL;
}

bool block_read(uint32_t index, uint64_t lba, uint32_t count, void *buffer)
{
    if (index || !buffer || !count || lba >= TEST_SECTORS ||
        count > TEST_SECTORS - lba)
        return false;
    memcpy(
        buffer,
        disk + (size_t)lba * BLOCK_SECTOR_SIZE,
        (size_t)count * BLOCK_SECTOR_SIZE);
    return true;
}

bool block_write(
    uint32_t index,
    uint64_t lba,
    uint32_t count,
    const void *buffer
)
{
    if (index || !buffer || !count || lba >= TEST_SECTORS ||
        count > TEST_SECTORS - lba)
        return false;
    memcpy(
        disk + (size_t)lba * BLOCK_SECTOR_SIZE,
        buffer,
        (size_t)count * BLOCK_SECTOR_SIZE);
    return true;
}

static int fail(const char *message)
{
    fprintf(stderr, "ext2 test failed: %s\n", message);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 2)
        return fail("expected disk image output path");

    if (!ext2_format(0, 0, TEST_SECTORS, "VeRsitileOS"))
        return fail("format");

    ext2_volume_t volume;
    if (!ext2_mount(&volume, 0, 0, TEST_SECTORS, "/mnt/root"))
        return fail("mount freshly formatted filesystem");
    ext2_volume_t root_volume;
    const char *root_relative_path = NULL;
    if (!ext2_mount(&root_volume, 0, 0, TEST_SECTORS, "/") ||
        strcmp(root_volume.volume_name, "VeRsitileOS") != 0 ||
        !ext2_path_matches(
            &root_volume,
            "/home/ada",
            &root_relative_path) ||
        strcmp(root_relative_path, "/home/ada") != 0)
        return fail("mount installed root and match absolute paths");
    ext2_unmount(&root_volume);
    if (!ext2_is_directory(&volume, "") ||
        ext2_mkdir(&volume, "home") < 0 ||
        ext2_mkdir(&volume, "home/ada") < 0 ||
        ext2_mkdir(&volume, "etc") < 0 ||
        ext2_mkdir(&volume, "etc/system") < 0 ||
        ext2_mkdir(&volume, "etc/system/systemd") < 0)
        return fail("create nested directories");
    if (ext2_mkdir(&volume, "home/empty") < 0 ||
        ext2_rmdir(&volume, "home/empty") < 0 ||
        ext2_rmdir(&volume, "home") >= 0)
        return fail("remove empty directory and reject nonempty directory");

    const size_t payload_size = 28 * 1024 * 1024;
    const size_t write_chunk_size = 64 * 1024;
    uint8_t *payload = malloc(write_chunk_size);
    uint8_t *readback = malloc(write_chunk_size);
    if (!payload || !readback)
        return fail("allocate test buffers");

    ext2_file_t file;
    if (ext2_open(
            &volume, "home/ada/large.bin", true, false, &file) < 0)
        return fail("open file for chunked write");
    for (size_t offset = 0; offset < payload_size;
         offset += write_chunk_size)
    {
        size_t chunk_size = payload_size - offset;
        if (chunk_size > write_chunk_size)
            chunk_size = write_chunk_size;
        for (size_t i = 0; i < chunk_size; i++)
        {
            size_t index = offset + i;
            payload[i] = (uint8_t)((index * 37U + index / 13U) & 0xFF);
        }
        if (ext2_write(&file, payload, chunk_size) !=
            (int64_t)chunk_size)
            return fail("write full-size file in installer-sized chunks");
    }
    ext2_close(&file);
    if (ext2_open(
            &volume, "home/ada/large.bin", false, false, &file) < 0)
        return fail("reopen full-size file");
    for (size_t offset = 0; offset < payload_size;
         offset += write_chunk_size)
    {
        size_t chunk_size = payload_size - offset;
        if (chunk_size > write_chunk_size)
            chunk_size = write_chunk_size;
        for (size_t i = 0; i < chunk_size; i++)
        {
            size_t index = offset + i;
            payload[i] = (uint8_t)((index * 37U + index / 13U) & 0xFF);
        }
        if (ext2_read(&file, readback, chunk_size) !=
                (int64_t)chunk_size ||
            memcmp(payload, readback, chunk_size) != 0)
            return fail("read and compare full-size file chunks");
    }
    ext2_close(&file);
    if (ext2_open(
            &volume, "home/ada/large2.bin", true, false, &file) < 0)
        return fail("open second full-size file");
    for (size_t offset = 0; offset < payload_size;
         offset += write_chunk_size)
    {
        size_t chunk_size = payload_size - offset;
        if (chunk_size > write_chunk_size)
            chunk_size = write_chunk_size;
        for (size_t i = 0; i < chunk_size; i++)
        {
            size_t index = offset + i;
            payload[i] = (uint8_t)((index * 37U + index / 13U) & 0xFF);
        }
        if (ext2_write(&file, payload, chunk_size) !=
            (int64_t)chunk_size)
            return fail("write second full-size file");
    }
    ext2_close(&file);
    if (ext2_open(
            &volume, "etc/system/systemd/version", true, false, &file) < 0 ||
        ext2_write(&file, "5.0.7\n", 6) != 6)
        return fail("create file after writing two full-size files");
    ext2_close(&file);

    ext2_dirent_t entries[4];
    if (ext2_list(&volume, "home/ada", 0, entries, 4) != 2)
        return fail("list directory entry");
    bool found_large_file = false;
    for (size_t i = 0; i < 2; i++)
        if (strcmp(entries[i].name, "large.bin") == 0 &&
            entries[i].size == payload_size &&
            !entries[i].is_directory)
            found_large_file = true;
    if (!found_large_file)
        return fail("validate large file directory entry");

    if (ext2_open(
            &volume, "home/ada/large.bin", false, true, &file) < 0 ||
        file.size != 0 ||
        ext2_write(&file, "done", 4) != 4)
        return fail("truncate and rewrite file");
    ext2_close(&file);
    ext2_unmount(&volume);
    if (!ext2_mount(&volume, 0, 0, TEST_SECTORS, "/mnt/root") ||
        ext2_open(
            &volume, "home/ada/large.bin", false, false, &file) < 0 ||
        file.size != 4)
        return fail("remount and inspect file");
    char result[4];
    if (ext2_read(&file, result, sizeof(result)) != sizeof(result) ||
        memcmp(result, "done", sizeof(result)) != 0)
        return fail("read file after remount");
    ext2_close(&file);
    int64_t unlink_result = ext2_unlink(&volume, "home/ada/large.bin");
    if (unlink_result < 0)
        return fail("unlink file and reclaim its data blocks");
    if (ext2_list(&volume, "home/ada", 0, entries, 4) != 1 ||
        strcmp(entries[0].name, "large2.bin") != 0)
        return fail("remove unlinked file from directory");
    if (ext2_open(
            &volume, "home/ada/large.bin", false, false, &file) >= 0)
        return fail("open unlinked file");

    FILE *image = fopen(argv[1], "wb");
    if (!image)
        return fail("open output image");
    if (fwrite(disk, 1, TEST_BYTES, image) != TEST_BYTES ||
        fclose(image) != 0)
        return fail("write output image");

    free(readback);
    free(payload);
    ext2_unmount(&volume);
    puts("ext2 format, mount, directory, rmdir, file I/O, indirect blocks, unlink, and remount passed");
    return 0;
}
