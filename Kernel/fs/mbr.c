#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "block.h"
#include "heap.h"
#include "mbr.h"
#include "serial.h"
#include "timer.h"

#define MBR_SIGNATURE_OFFSET 510
#define MBR_PARTITION_OFFSET 446
#define MBR_ENTRY_SIZE 16
#define MBR_EXTENDED_CHS 0x05
#define MBR_EXTENDED_LBA 0x0F
#define MBR_EXTENDED_LINUX 0x85
#define MBR_PROTECTIVE 0xEE
#define MBR_EBR_LIMIT 128
#define GPT_HEADER_SIGNATURE "EFI PART"
#define GPT_HEADER_MIN_SIZE 92
#define GPT_ENTRY_MIN_SIZE 128
#define GPT_ENTRY_MAX_SIZE BLOCK_SECTOR_SIZE
#define GPT_ENTRY_LIMIT 4096
#define GPT_WRITE_ENTRY_COUNT 128
#define GPT_WRITE_ENTRY_SIZE 128
#define GPT_WRITE_ARRAY_SECTORS 32

static mbr_partition_t partitions[MBR_PARTITION_LIMIT];
static uint32_t partition_count;

static uint32_t read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
        ((uint32_t)bytes[1] << 8) |
        ((uint32_t)bytes[2] << 16) |
        ((uint32_t)bytes[3] << 24);
}

static void write_le16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void write_le32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static void write_le64(uint8_t *bytes, uint64_t value)
{
    write_le32(bytes, (uint32_t)value);
    write_le32(bytes + 4, (uint32_t)(value >> 32));
}

static uint64_t read_le64(const uint8_t *bytes)
{
    return (uint64_t)read_le32(bytes) |
        ((uint64_t)read_le32(bytes + 4) << 32);
}

static bool is_fat32_superfloppy(
    const block_device_t *device,
    const uint8_t *sector
)
{
    uint32_t bytes_per_sector = (uint32_t)sector[11] |
        ((uint32_t)sector[12] << 8);
    uint32_t sectors_per_cluster = sector[13];
    uint32_t reserved_sectors = (uint32_t)sector[14] |
        ((uint32_t)sector[15] << 8);
    uint32_t fat_count = sector[16];
    uint32_t root_entries = (uint32_t)sector[17] |
        ((uint32_t)sector[18] << 8);
    uint32_t total_sectors_16 = (uint32_t)sector[19] |
        ((uint32_t)sector[20] << 8);
    uint32_t fat_sectors_16 = (uint32_t)sector[22] |
        ((uint32_t)sector[23] << 8);
    uint32_t total_sectors = read_le32(sector + 32);
    uint32_t fat_sectors = read_le32(sector + 36);
    uint32_t root_cluster = read_le32(sector + 44) & 0x0FFFFFFFU;

    return device &&
        sector[MBR_SIGNATURE_OFFSET] == 0x55 &&
        sector[MBR_SIGNATURE_OFFSET + 1] == 0xAA &&
        bytes_per_sector == BLOCK_SECTOR_SIZE &&
        sectors_per_cluster &&
        !(sectors_per_cluster & (sectors_per_cluster - 1)) &&
        sectors_per_cluster <= 128 &&
        reserved_sectors &&
        fat_count && fat_count <= 2 &&
        !root_entries && !total_sectors_16 && !fat_sectors_16 &&
        total_sectors && total_sectors <= device->sector_count &&
        fat_sectors && root_cluster >= 2;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *bytes, size_t size)
{
    for (size_t i = 0; i < size; i++)
    {
        crc ^= bytes[i];
        for (uint32_t bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
    return crc;
}

static bool guid_is_zero(const uint8_t guid[16])
{
    for (uint32_t i = 0; i < 16; i++)
    {
        if (guid[i])
            return false;
    }
    return true;
}

static bool is_extended(uint8_t type)
{
    return type == MBR_EXTENDED_CHS ||
        type == MBR_EXTENDED_LBA ||
        type == MBR_EXTENDED_LINUX;
}

static bool range_valid(
    const block_device_t *device,
    uint64_t start,
    uint64_t count
)
{
    return count && start < device->sector_count &&
        count <= device->sector_count - start;
}

static bool add_partition(
    uint32_t device_index,
    uint32_t number,
    uint8_t type,
    uint8_t status,
    uint64_t start,
    uint64_t count,
    const uint8_t *type_guid
)
{
    const block_device_t *device = block_device_get(device_index);
    if (!device || (!type && !type_guid) ||
        (status != 0 && status != 0x80) ||
        !range_valid(device, start, count) ||
        partition_count >= MBR_PARTITION_LIMIT)
        return false;

    partitions[partition_count++] = (mbr_partition_t) {
        .device_index = device_index,
        .number = number,
        .type = type,
        .bootable = status == 0x80,
        .gpt = type_guid != NULL,
        .start_lba = start,
        .sector_count = count
    };
    if (type_guid)
    {
        for (uint32_t i = 0; i < sizeof(partitions[0].type_guid); i++)
            partitions[partition_count - 1].type_guid[i] = type_guid[i];
    }
    return true;
}

static bool scan_gpt(uint32_t device_index)
{
    const block_device_t *device = block_device_get(device_index);
    uint8_t header[BLOCK_SECTOR_SIZE];
    if (!device || device->sector_count < 2 ||
        !block_read(device_index, 1, 1, header))
        return false;

    static const uint8_t signature[8] = GPT_HEADER_SIGNATURE;
    for (uint32_t i = 0; i < sizeof(signature); i++)
    {
        if (header[i] != signature[i])
            return false;
    }

    uint32_t header_size = read_le32(header + 12);
    uint32_t stored_header_crc = read_le32(header + 16);
    if (header_size < GPT_HEADER_MIN_SIZE ||
        header_size > BLOCK_SECTOR_SIZE ||
        read_le64(header + 24) != 1)
        return false;

    uint8_t header_copy[BLOCK_SECTOR_SIZE];
    for (uint32_t i = 0; i < header_size; i++)
        header_copy[i] = header[i];
    for (uint32_t i = 16; i < 20; i++)
        header_copy[i] = 0;
    if ((crc32_update(0xFFFFFFFFU, header_copy, header_size) ^ 0xFFFFFFFFU)
        != stored_header_crc)
        return false;

    uint64_t backup_lba = read_le64(header + 32);
    uint64_t first_usable = read_le64(header + 40);
    uint64_t last_usable = read_le64(header + 48);
    uint64_t entries_lba = read_le64(header + 72);
    uint32_t entry_count = read_le32(header + 80);
    uint32_t entry_size = read_le32(header + 84);
    uint32_t stored_entries_crc = read_le32(header + 88);
    if (backup_lba >= device->sector_count || backup_lba == 1 ||
        first_usable > last_usable ||
        last_usable >= device->sector_count ||
        !entry_count || entry_count > GPT_ENTRY_LIMIT ||
        entry_size < GPT_ENTRY_MIN_SIZE ||
        entry_size > GPT_ENTRY_MAX_SIZE ||
        entry_size % 8 || BLOCK_SECTOR_SIZE % entry_size)
        return false;

    uint64_t entries_bytes = (uint64_t)entry_count * entry_size;
    uint64_t entries_sectors =
        (entries_bytes + BLOCK_SECTOR_SIZE - 1) / BLOCK_SECTOR_SIZE;
    if (entries_lba >= device->sector_count ||
        entries_sectors > device->sector_count - entries_lba)
        return false;

    uint8_t sector[BLOCK_SECTOR_SIZE];
    uint32_t crc = 0xFFFFFFFFU;
    uint64_t bytes_remaining = entries_bytes;
    for (uint64_t i = 0; i < entries_sectors; i++)
    {
        if (!block_read(device_index, entries_lba + i, 1, sector))
            return false;
        size_t amount = bytes_remaining > sizeof(sector)
            ? sizeof(sector)
            : (size_t)bytes_remaining;
        crc = crc32_update(crc, sector, amount);
        bytes_remaining -= amount;
    }
    if ((crc ^ 0xFFFFFFFFU) != stored_entries_crc)
        return false;

    uint32_t added = 0;
    uint32_t entries_per_sector = BLOCK_SECTOR_SIZE / entry_size;
    for (uint32_t index = 0; index < entry_count; index++)
    {
        if (index % entries_per_sector == 0 &&
            !block_read(
                device_index,
                entries_lba + index / entries_per_sector,
                1,
                sector))
            return false;

        const uint8_t *entry =
            sector + (index % entries_per_sector) * entry_size;
        if (guid_is_zero(entry))
            continue;

        uint64_t first_lba = read_le64(entry + 32);
        uint64_t last_lba = read_le64(entry + 40);
        if (first_lba < first_usable || last_lba > last_usable ||
            first_lba > last_lba)
            continue;
        if (add_partition(
                device_index,
                index + 1,
                0,
                0,
                first_lba,
                last_lba - first_lba + 1,
                entry))
            added++;
    }

    serial_log(SERIAL_LOG_INFO, "Found valid GPT partition table on IDE disk");
    if (!added)
        serial_log(SERIAL_LOG_INFO, "GPT contains no usable partitions");
    return true;
}

static void scan_extended(
    uint32_t device_index,
    uint64_t base,
    uint64_t extended_count,
    uint32_t *logical_number
)
{
    const block_device_t *device = block_device_get(device_index);
    if (!device || !range_valid(device, base, extended_count))
        return;

    uint64_t current = base;
    uint64_t visited[MBR_EBR_LIMIT];
    uint32_t visited_count = 0;
    uint8_t sector[BLOCK_SECTOR_SIZE];
    for (uint32_t depth = 0; depth < MBR_EBR_LIMIT; depth++)
    {
        bool seen = false;
        for (uint32_t i = 0; i < visited_count; i++)
        {
            if (visited[i] == current)
            {
                seen = true;
                break;
            }
        }
        if (seen || current < base ||
            current - base >= extended_count ||
            !block_read(device_index, current, 1, sector) ||
            sector[MBR_SIGNATURE_OFFSET] != 0x55 ||
            sector[MBR_SIGNATURE_OFFSET + 1] != 0xAA)
            return;
        visited[visited_count++] = current;

        const uint8_t *entry = sector + MBR_PARTITION_OFFSET;
        uint8_t type = entry[4];
        uint32_t relative_start = read_le32(entry + 8);
        uint32_t count = read_le32(entry + 12);
        if (type && !is_extended(type) && count &&
            current <= UINT64_MAX - relative_start)
        {
            uint64_t start = current + relative_start;
            if (start >= base && start - base < extended_count &&
                count <= extended_count - (start - base))
                (void)add_partition(
                    device_index,
                    (*logical_number)++,
                    type,
                    entry[0],
                    start,
                    count,
                    NULL);
        }

        entry = sector + MBR_PARTITION_OFFSET + MBR_ENTRY_SIZE;
        if (!is_extended(entry[4]))
            return;
        relative_start = read_le32(entry + 8);
        count = read_le32(entry + 12);
        if (!count || base > UINT64_MAX - relative_start)
            return;
        uint64_t next = base + relative_start;
        if (next < base || next - base >= extended_count ||
            count > extended_count - (next - base))
            return;
        current = next;
    }
}

uint32_t mbr_scan(void)
{
    partition_count = 0;
    uint8_t sector[BLOCK_SECTOR_SIZE];
    for (uint32_t device_index = 0;
         device_index < block_device_count();
         device_index++)
    {
        const block_device_t *device = block_device_get(device_index);
        if (!device)
            continue;
        if (!block_read(device_index, 0, 1, sector))
        {
            serial_log(SERIAL_LOG_WARN, "Could not read block-device sector zero");
            continue;
        }
        if (sector[MBR_SIGNATURE_OFFSET] != 0x55 ||
            sector[MBR_SIGNATURE_OFFSET + 1] != 0xAA)
        {
            serial_log(SERIAL_LOG_WARN, "Invalid block-device partition-table signature");
            continue;
        }

        bool protective_gpt = false;
        for (uint32_t i = 0; i < 4; i++)
        {
            const uint8_t *entry =
                sector + MBR_PARTITION_OFFSET + i * MBR_ENTRY_SIZE;
            if (entry[4] == MBR_PROTECTIVE)
            {
                protective_gpt = true;
                break;
            }
        }
        if (protective_gpt)
        {
            if (!scan_gpt(device_index))
                serial_log(SERIAL_LOG_WARN, "Invalid or unsupported GPT table");
            continue;
        }

        bool has_mbr_entries = false;
        uint32_t logical_number = 5;
        for (uint32_t i = 0; i < 4; i++)
        {
            const uint8_t *entry =
                sector + MBR_PARTITION_OFFSET + i * MBR_ENTRY_SIZE;
            uint8_t type = entry[4];
            if (type)
                has_mbr_entries = true;
            uint32_t start = read_le32(entry + 8);
            uint32_t count = read_le32(entry + 12);
            if (!type || !count ||
                !add_partition(
                    device_index,
                    i + 1,
                    type,
                    entry[0],
                    start,
                    count,
                    NULL))
                continue;
            if (is_extended(type))
                scan_extended(
                    device_index,
                    start,
                    count,
                    &logical_number);
        }

        if (has_mbr_entries)
        {
            serial_log(
                SERIAL_LOG_INFO,
                "Found MBR partition table on block device");
        }
        else if (is_fat32_superfloppy(device, sector) &&
            add_partition(
                device_index,
                0,
                0x0C,
                0,
                0,
                read_le32(sector + 32),
                NULL))
        {
            serial_log(
                SERIAL_LOG_INFO,
                "Found FAT32 superfloppy on block device");
        }
    }
    return partition_count;
}

static void make_guid(uint8_t guid[16], uint64_t seed)
{
    for (uint32_t i = 0; i < 16; i++)
    {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        guid[i] = (uint8_t)(seed >> 32);
    }
    guid[7] = (uint8_t)((guid[7] & 0x0F) | 0x40);
    guid[8] = (uint8_t)((guid[8] & 0x3F) | 0x80);
}

static void finish_gpt_header_crc(uint8_t header[BLOCK_SECTOR_SIZE])
{
    write_le32(header + 16, 0);
    write_le32(
        header + 16,
        crc32_update(0xFFFFFFFFU, header, GPT_HEADER_MIN_SIZE) ^
            0xFFFFFFFFU);
}

bool mbr_create_gpt(
    uint32_t device_index,
    const mbr_gpt_partition_t *requested,
    uint32_t requested_count
)
{
    const block_device_t *device = block_device_get(device_index);
    if (!device || !device->writable || !requested ||
        !requested_count || requested_count > MBR_PARTITION_LIMIT ||
        device->sector_count < 4096 ||
        device->sector_count <= 2 * GPT_WRITE_ARRAY_SECTORS + 2)
        return false;

    uint64_t first_usable = 2 + GPT_WRITE_ARRAY_SECTORS;
    uint64_t last_usable =
        device->sector_count - GPT_WRITE_ARRAY_SECTORS - 2;
    for (uint32_t i = 0; i < requested_count; i++)
    {
        const mbr_gpt_partition_t *partition = &requested[i];
        if (guid_is_zero(partition->type_guid) ||
            !partition->sector_count ||
            partition->start_lba < first_usable ||
            partition->start_lba > last_usable ||
            partition->sector_count - 1 >
                last_usable - partition->start_lba)
            return false;
        uint64_t partition_end =
            partition->start_lba + partition->sector_count - 1;
        for (uint32_t j = 0; j < i; j++)
        {
            uint64_t previous_end =
                requested[j].start_lba +
                requested[j].sector_count - 1;
            if (partition->start_lba <= previous_end &&
                requested[j].start_lba <= partition_end)
                return false;
        }
    }

    uint8_t *entries = kmalloc(
        GPT_WRITE_ENTRY_COUNT * GPT_WRITE_ENTRY_SIZE);
    if (!entries)
        return false;
    for (uint32_t i = 0;
         i < GPT_WRITE_ENTRY_COUNT * GPT_WRITE_ENTRY_SIZE;
         i++)
        entries[i] = 0;

    uint64_t seed = timer_get_milliseconds() ^
        ((uint64_t)device_index << 32) ^ device->sector_count;
    uint8_t disk_guid[16];
    make_guid(disk_guid, seed);
    for (uint32_t i = 0; i < requested_count; i++)
    {
        uint8_t *entry = entries + i * GPT_WRITE_ENTRY_SIZE;
        const mbr_gpt_partition_t *partition = &requested[i];
        for (uint32_t byte = 0; byte < 16; byte++)
            entry[byte] = partition->type_guid[byte];
        make_guid(
            entry + 16,
            seed ^ partition->start_lba ^
                (partition->sector_count << 1) ^ i);
        write_le64(entry + 32, partition->start_lba);
        write_le64(
            entry + 40,
            partition->start_lba + partition->sector_count - 1);
        write_le64(entry + 48, partition->attributes);
        for (uint32_t character = 0;
             character < MBR_GPT_PARTITION_NAME_MAX &&
             partition->name[character];
             character++)
            write_le16(
                entry + 56 + character * 2,
                (uint8_t)partition->name[character]);
    }

    uint32_t entries_crc =
        crc32_update(
            0xFFFFFFFFU,
            entries,
            GPT_WRITE_ENTRY_COUNT * GPT_WRITE_ENTRY_SIZE) ^
        0xFFFFFFFFU;
    uint8_t header[BLOCK_SECTOR_SIZE] = {0};
    static const uint8_t signature[8] = GPT_HEADER_SIGNATURE;
    for (uint32_t i = 0; i < sizeof(signature); i++)
        header[i] = signature[i];
    write_le32(header + 8, 0x00010000U);
    write_le32(header + 12, GPT_HEADER_MIN_SIZE);
    write_le64(header + 24, 1);
    write_le64(header + 32, device->sector_count - 1);
    write_le64(header + 40, first_usable);
    write_le64(header + 48, last_usable);
    for (uint32_t i = 0; i < sizeof(disk_guid); i++)
        header[56 + i] = disk_guid[i];
    write_le64(header + 72, 2);
    write_le32(header + 80, GPT_WRITE_ENTRY_COUNT);
    write_le32(header + 84, GPT_WRITE_ENTRY_SIZE);
    write_le32(header + 88, entries_crc);
    finish_gpt_header_crc(header);

    uint8_t backup_header[BLOCK_SECTOR_SIZE];
    for (uint32_t i = 0; i < sizeof(backup_header); i++)
        backup_header[i] = header[i];
    write_le64(backup_header + 24, device->sector_count - 1);
    write_le64(backup_header + 32, 1);
    write_le64(
        backup_header + 72,
        device->sector_count - GPT_WRITE_ARRAY_SECTORS - 1);
    finish_gpt_header_crc(backup_header);

    uint8_t mbr[BLOCK_SECTOR_SIZE] = {0};
    uint8_t *protective = mbr + MBR_PARTITION_OFFSET;
    protective[1] = 0xFF;
    protective[2] = 0xFF;
    protective[3] = 0xFF;
    protective[4] = MBR_PROTECTIVE;
    protective[5] = 0xFF;
    protective[6] = 0xFF;
    protective[7] = 0xFF;
    write_le32(protective + 8, 1);
    uint64_t protective_count = device->sector_count - 1;
    if (protective_count > UINT32_MAX)
        protective_count = UINT32_MAX;
    write_le32(protective + 12, (uint32_t)protective_count);
    mbr[MBR_SIGNATURE_OFFSET] = 0x55;
    mbr[MBR_SIGNATURE_OFFSET + 1] = 0xAA;

    uint64_t backup_entries_lba =
        device->sector_count - GPT_WRITE_ARRAY_SECTORS - 1;
    bool success =
        block_write(device_index, 0, 1, mbr) &&
        block_write(
            device_index,
            2,
            GPT_WRITE_ARRAY_SECTORS,
            entries) &&
        block_write(
            device_index,
            backup_entries_lba,
            GPT_WRITE_ARRAY_SECTORS,
            entries) &&
        block_write(device_index, 1, 1, header) &&
        block_write(
            device_index,
            device->sector_count - 1,
            1,
            backup_header);
    kfree(entries);
    if (!success)
        return false;
    return mbr_scan() >= requested_count;
}

bool mbr_delete_partition(uint32_t device_index, uint32_t partition_number)
{
    const block_device_t *device = block_device_get(device_index);
    if (!device || !device->writable || !partition_number)
        return false;
    mbr_scan();
    const mbr_partition_t *partition = NULL;
    for (uint32_t i = 0; i < partition_count; i++)
    {
        if (partitions[i].device_index == device_index &&
            partitions[i].number == partition_number)
        {
            partition = &partitions[i];
            break;
        }
    }
    if (!partition)
        return false;

    if (!partition->gpt)
    {
        if (partition_number > 4)
            return false;
        uint8_t sector[BLOCK_SECTOR_SIZE];
        if (!block_read(device_index, 0, 1, sector))
            return false;
        uint8_t *entry = sector + MBR_PARTITION_OFFSET +
            (partition_number - 1) * MBR_ENTRY_SIZE;
        for (uint32_t i = 0; i < MBR_ENTRY_SIZE; i++)
            entry[i] = 0;
        if (!block_write(device_index, 0, 1, sector))
            return false;
        (void)mbr_scan();
        return true;
    }

    uint8_t header[BLOCK_SECTOR_SIZE];
    if (!block_read(device_index, 1, 1, header) ||
        header[0] != 'E' || header[1] != 'F' ||
        header[2] != 'I' || header[3] != ' ' ||
        header[4] != 'P' || header[5] != 'A' ||
        header[6] != 'R' || header[7] != 'T')
        return false;
    uint32_t count = read_le32(header + 80);
    uint32_t entry_size = read_le32(header + 84);
    uint64_t entries_lba = read_le64(header + 72);
    uint64_t total_bytes = (uint64_t)count * entry_size;
    if (count < partition_number ||
        count > GPT_WRITE_ENTRY_COUNT ||
        entry_size != GPT_WRITE_ENTRY_SIZE ||
        total_bytes != GPT_WRITE_ENTRY_COUNT * GPT_WRITE_ENTRY_SIZE ||
        entries_lba >= device->sector_count ||
        GPT_WRITE_ARRAY_SECTORS > device->sector_count - entries_lba)
        return false;

    uint8_t *entries = kmalloc(
        GPT_WRITE_ENTRY_COUNT * GPT_WRITE_ENTRY_SIZE);
    if (!entries)
        return false;
    if (!block_read(
            device_index,
            entries_lba,
            GPT_WRITE_ARRAY_SECTORS,
            entries))
    {
        kfree(entries);
        return false;
    }
    uint8_t *entry =
        entries + (partition_number - 1) * GPT_WRITE_ENTRY_SIZE;
    if (guid_is_zero(entry))
    {
        kfree(entries);
        return false;
    }
    for (uint32_t i = 0; i < GPT_WRITE_ENTRY_SIZE; i++)
        entry[i] = 0;
    uint32_t entries_crc =
        crc32_update(
            0xFFFFFFFFU,
            entries,
            GPT_WRITE_ENTRY_COUNT * GPT_WRITE_ENTRY_SIZE) ^
        0xFFFFFFFFU;
    uint64_t backup_entries_lba =
        device->sector_count - GPT_WRITE_ARRAY_SECTORS - 1;
    uint64_t alternate_lba = read_le64(header + 32);
    write_le32(header + 88, entries_crc);
    finish_gpt_header_crc(header);
    bool success =
        block_write(
            device_index,
            entries_lba,
            GPT_WRITE_ARRAY_SECTORS,
            entries) &&
        block_write(
            device_index,
            backup_entries_lba,
            GPT_WRITE_ARRAY_SECTORS,
            entries) &&
        block_write(device_index, 1, 1, header);

    uint8_t backup_header[BLOCK_SECTOR_SIZE];
    if (success && alternate_lba < device->sector_count &&
        block_read(device_index, alternate_lba, 1, backup_header))
    {
        write_le32(backup_header + 88, entries_crc);
        write_le64(backup_header + 72, backup_entries_lba);
        finish_gpt_header_crc(backup_header);
        success = block_write(
            device_index,
            alternate_lba,
            1,
            backup_header);
    }
    else
    {
        success = false;
    }
    kfree(entries);
    if (!success)
        return false;
    (void)mbr_scan();
    return true;
}

uint32_t mbr_partition_count(void)
{
    return partition_count;
}

const mbr_partition_t *mbr_partition_get(uint32_t index)
{
    return index < partition_count ? &partitions[index] : 0;
}
