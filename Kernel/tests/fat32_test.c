#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "block.h"
#include "fat32.h"
#include "serial.h"

#define TEST_SECTORS 264192U
#define TEST_PARTITION_LBA 2048U
#define TEST_PARTITION_SECTORS 262144U
#define TEST_BYTES ((size_t)TEST_SECTORS * BLOCK_SECTOR_SIZE)

static uint8_t disk[TEST_BYTES];
static uint64_t failed_write_lba = UINT64_MAX;
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
    if (index || !buffer || !count || count > 8 ||
        lba >= TEST_SECTORS ||
        count > TEST_SECTORS - lba)
        return false;
    if (failed_write_lba >= lba && failed_write_lba - lba < count)
        return false;
    memcpy(
        disk + (size_t)lba * BLOCK_SECTOR_SIZE,
        buffer,
        (size_t)count * BLOCK_SECTOR_SIZE);
    return true;
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

static int fail(const char *message)
{
    fprintf(stderr, "FAT32 test failed: %s\n", message);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 2)
        return fail("expected FAT32 image output path");
    if (!fat32_format(
            0,
            TEST_PARTITION_LBA,
            TEST_PARTITION_SECTORS,
            "TEST"))
        return fail("format filesystem");

    fat32_volume_t volume;
    if (!fat32_mount(
            &volume,
            0,
            TEST_PARTITION_LBA,
            TEST_PARTITION_SECTORS,
            "/mnt/test"))
        return fail("mount freshly formatted filesystem");
    if (!volume.writable ||
        fat32_mkdir(&volume, "/Long directory name") < 0 ||
        fat32_mkdir(&volume, "/Long directory name/nested") < 0)
        return fail("create long-name directories");
    if (fat32_mkdir(&volume, "/EFI") < 0 ||
        fat32_mkdir(&volume, "/EFI/BOOT") < 0 ||
        fat32_mkdir(&volume, "/boot") < 0 ||
        fat32_mkdir(&volume, "/boot/limine") < 0)
        return fail("create EFI boot directory tree on installer-sized volume");

    static const struct
    {
        const char *path;
        const char *contents;
    } boot_files[] = {
        { "/EFI/BOOT/BOOTX64.EFI", "UEFI boot executable" },
        { "/boot/kernel.elf", "kernel image" },
        { "/boot/limine/limine-bios.sys", "Limine BIOS stage" },
        { "/boot/limine/limine.conf", "boot configuration" }
    };
    fat32_file_t boot_file;
    uint8_t boot_readback[128];
    for (size_t i = 0; i < sizeof(boot_files) / sizeof(boot_files[0]); i++)
    {
        if (fat32_open_write(
                &volume, boot_files[i].path, true, true, &boot_file) < 0)
            return fail("create installer boot file on FAT32");
        size_t content_size = strlen(boot_files[i].contents);
        if (fat32_write(&boot_file, boot_files[i].contents, content_size) !=
            (int64_t)content_size)
            return fail("write installer boot file on FAT32");
        if (fat32_open(&volume, boot_files[i].path, &boot_file) < 0 ||
            boot_file.size != content_size ||
            fat32_read(&boot_file, boot_readback, content_size) !=
                (int64_t)content_size ||
            memcmp(boot_readback, boot_files[i].contents, content_size) != 0)
            return fail("verify installer boot file on FAT32");
    }

    const size_t payload_size = 512 * 1024;
    uint8_t *payload = malloc(payload_size);
    uint8_t *readback = malloc(payload_size);
    if (!payload || !readback)
        return fail("allocate test buffers");
    for (size_t i = 0; i < payload_size; i++)
        payload[i] = (uint8_t)((i * 31U + i / 17U) & 0xFF);

    fat32_file_t file;
    if (fat32_open_write(
            &volume,
            "/Long directory name/nested/configuration file.txt",
            true,
            false,
            &file) < 0 ||
        fat32_write(&file, payload, payload_size) !=
            (int64_t)payload_size)
        return fail("write across multiple clusters");
    if (fat32_open(
            &volume,
            "/Long directory name/nested/configuration file.txt",
            &file) < 0 ||
        file.size != payload_size ||
        fat32_read(&file, readback, payload_size) !=
            (int64_t)payload_size ||
        memcmp(payload, readback, payload_size) != 0)
        return fail("read and compare long-name file");
    if (fat32_open_write(
            &volume,
            "/boot/kernel.elf",
            true,
            true,
            &file) < 0 ||
        fat32_write(&file, payload, payload_size) !=
            (int64_t)payload_size ||
        fat32_open(&volume, "/boot/kernel.elf", &file) < 0 ||
        file.size != payload_size ||
        fat32_read(&file, readback, payload_size) !=
            (int64_t)payload_size ||
        memcmp(payload, readback, payload_size) != 0)
        return fail("write and verify installer-sized kernel on FAT32");

    fat32_dirent_t entries[4];
    if (fat32_list(
            &volume,
            "/Long directory name/nested",
            0,
            entries,
            4) != 1 ||
        strcmp(entries[0].name, "configuration file.txt") != 0 ||
        entries[0].size != payload_size)
        return fail("list directory entries");

    if (fat32_open_write(
            &volume,
            "/Long directory name/nested/configuration file.txt",
            false,
            true,
            &file) < 0 ||
        file.size != 0)
        return fail("truncate existing file");
    static const char replacement[] = "FAT32 write test";
    if (fat32_write(&file, replacement, sizeof(replacement)) !=
            (int64_t)sizeof(replacement) ||
        fat32_open(
            &volume,
            "/Long directory name/nested/configuration file.txt",
            &file) < 0 ||
        file.size != sizeof(replacement) ||
        fat32_read(&file, readback, sizeof(replacement)) !=
            (int64_t)sizeof(replacement) ||
        memcmp(readback, replacement, sizeof(replacement)) != 0)
        return fail("truncate and rewrite file");

    FILE *image = fopen(argv[1], "wb");
    if (!image)
        return fail("open output image");
    size_t image_size =
        (size_t)TEST_PARTITION_SECTORS * BLOCK_SECTOR_SIZE;
    bool image_ok = fwrite(
        disk + (size_t)TEST_PARTITION_LBA * BLOCK_SECTOR_SIZE,
        1,
        image_size,
        image) == image_size;
    if (fclose(image) != 0 || !image_ok)
        return fail("write output image");

    failed_write_lba = TEST_PARTITION_LBA + 32;
    if (fat32_format(
            0,
            TEST_PARTITION_LBA,
            TEST_PARTITION_SECTORS,
            "TEST") ||
        !fat32_format_error() ||
        strcmp(
            fat32_format_error(),
            "Could not clear FAT32 FAT sectors") != 0)
        return fail("report a failed batched FAT write");

    free(readback);
    free(payload);
    puts("FAT32 format, mount, long names, directory, file I/O, and truncate passed");
    return 0;
}
