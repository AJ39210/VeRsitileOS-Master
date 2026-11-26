#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "block.h"
#include "heap.h"
#include "mbr.h"

#define TEST_SECTORS 131072U
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

uint32_t block_device_count(void)
{
    return 1;
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

void *kmalloc(uint64_t size)
{
    return malloc((size_t)size);
}

void kfree(void *pointer)
{
    free(pointer);
}

uint64_t timer_get_milliseconds(void)
{
    return 123456;
}

void serial_log(int level, const char *message)
{
    (void)level;
    (void)message;
}

void serial_log_begin(int level)
{
    (void)level;
}

void serial_log_end(void)
{
}

void serial_write(const char *text)
{
    (void)text;
}

void serial_write_hex(uint64_t value)
{
    (void)value;
}

void serial_write_dec(uint64_t value)
{
    (void)value;
}

static int fail(const char *message)
{
    fprintf(stderr, "GPT test failed: %s\n", message);
    return 1;
}

int main(void)
{
    static const uint8_t bios_boot_guid[16] = {
        0x48, 0x61, 0x68, 0x21, 0x49, 0x6E, 0x6F, 0x74,
        0x4E, 0x65, 0x65, 0x64, 0x45, 0x46, 0x49, 0x20
    };
    static const uint8_t efi_system_guid[16] = {
        0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
        0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
    };
    static const uint8_t linux_filesystem_guid[16] = {
        0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
        0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4
    };
    const uint64_t last_usable = TEST_SECTORS - 34;
    mbr_gpt_partition_t partitions[3] = {
        {
            .start_lba = 2048,
            .sector_count = 2048,
            .name = "BIOS boot"
        },
        {
            .start_lba = 4096,
            .sector_count = 8192,
            .name = "EFI System"
        },
        {
            .start_lba = 12288,
            .sector_count = last_usable - 12288 + 1,
            .name = "VeRsitileOS"
        }
    };
    memcpy(partitions[0].type_guid, bios_boot_guid, 16);
    memcpy(partitions[1].type_guid, efi_system_guid, 16);
    memcpy(partitions[2].type_guid, linux_filesystem_guid, 16);

    if (!mbr_create_gpt(0, partitions, 3) ||
        mbr_partition_count() != 3)
        return fail("create and scan GPT");
    const mbr_partition_t *partition = mbr_partition_get(1);
    if (!partition || !partition->gpt || partition->number != 2 ||
        partition->start_lba != 4096 ||
        partition->sector_count != 8192)
        return fail("inspect partition metadata");
    mbr_gpt_partition_t invalid[3];
    memcpy(invalid, partitions, sizeof(invalid));
    invalid[2].start_lba = 8192;
    if (mbr_create_gpt(0, invalid, 3))
        return fail("reject overlapping partitions");
    if (!mbr_delete_partition(0, 2) ||
        mbr_partition_count() != 2)
        return fail("delete GPT partition");
    partition = mbr_partition_get(1);
    if (!partition || partition->number != 3 ||
        partition->start_lba != 12288)
        return fail("preserve remaining GPT entry number");
    puts("GPT create, scan, range checks, and delete passed");
    return 0;
}
