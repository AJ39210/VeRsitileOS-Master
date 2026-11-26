#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "block.h"
#include "ext2.h"
#include "syscall.h"

#define EXT2_SUPER_MAGIC 0xEF53U
#define EXT2_SUPER_BYTES 1024U
#define EXT2_SUPER_OFFSET 1024U
#define EXT2_GROUP_DESC_SIZE 32U
#define EXT2_INODE_MIN_SIZE 128U
#define EXT2_INODE_MAX_SIZE 256U
#define EXT2_ROOT_INODE 2U
#define EXT2_FIRST_NON_RESERVED_INODE 11U
#define EXT2_BLOCK_POINTERS 15U
#define EXT2_DIRECT_BLOCKS 12U
#define EXT2_FEATURE_INCOMPAT_FILETYPE 0x0002U
#define EXT2_FEATURE_COMPAT_HAS_JOURNAL 0x0004U
#define EXT2_FEATURE_RO_COMPAT_SUPPORTED 0x0007U
#define EXT2_MODE_TYPE_MASK 0xF000U
#define EXT2_MODE_DIRECTORY 0x4000U
#define EXT2_MODE_REGULAR 0x8000U
#define EXT2_DIR_ENTRY_HEADER 8U
#define EXT2_DIR_TYPE_UNKNOWN 0U
#define EXT2_DIR_TYPE_REGULAR 1U
#define EXT2_DIR_TYPE_DIRECTORY 2U

static void bytes_zero(void *destination, size_t size)
{
    uint8_t *bytes = destination;
    for (size_t i = 0; i < size; i++)
        bytes[i] = 0;
}

static void bytes_copy(void *destination, const void *source, size_t size)
{
    uint8_t *to = destination;
    const uint8_t *from = source;
    for (size_t i = 0; i < size; i++)
        to[i] = from[i];
}

static uint16_t read_le16(const uint8_t *bytes)
{
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

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

static uint32_t divide_round_up(uint32_t value, uint32_t divisor)
{
    return value / divisor + (value % divisor != 0);
}

static bool ext2_group_has_backup(uint32_t group);
static bool ext2_write_superblock_copy(
    const ext2_volume_t *volume,
    const uint8_t superblock[EXT2_SUPER_BYTES],
    uint32_t group
);

static bool string_copy(char *destination, size_t capacity, const char *source)
{
    if (!destination || !capacity || !source)
        return false;
    size_t i = 0;
    while (i < capacity - 1 && source[i])
    {
        destination[i] = source[i];
        i++;
    }
    if (source[i])
        return false;
    destination[i] = '\0';
    return true;
}

static bool raw_read(
    uint32_t device_index,
    uint64_t partition_lba,
    uint64_t partition_sectors,
    uint64_t relative_lba,
    uint32_t count,
    void *buffer
)
{
    if (!count || relative_lba >= partition_sectors ||
        count > partition_sectors - relative_lba ||
        partition_lba > UINT64_MAX - relative_lba)
        return false;
    return block_read(
        device_index,
        partition_lba + relative_lba,
        count,
        buffer);
}

static bool raw_write(
    uint32_t device_index,
    uint64_t partition_lba,
    uint64_t partition_sectors,
    uint64_t relative_lba,
    uint32_t count,
    const void *buffer
)
{
    if (!count || relative_lba >= partition_sectors ||
        count > partition_sectors - relative_lba ||
        partition_lba > UINT64_MAX - relative_lba)
        return false;
    return block_write(
        device_index,
        partition_lba + relative_lba,
        count,
        buffer);
}

static bool ext2_read_block(
    const ext2_volume_t *volume,
    uint32_t block_number,
    void *buffer
)
{
    if (!volume || !buffer || block_number >= volume->blocks_count ||
        !volume->block_size || volume->block_size % BLOCK_SECTOR_SIZE)
        return false;
    uint32_t sectors_per_block =
        volume->block_size / BLOCK_SECTOR_SIZE;
    uint64_t relative_lba = (uint64_t)block_number * sectors_per_block;
    return raw_read(
        volume->device_index,
        volume->partition_lba,
        volume->partition_sectors,
        relative_lba,
        sectors_per_block,
        buffer);
}

static bool ext2_write_block(
    const ext2_volume_t *volume,
    uint32_t block_number,
    const void *buffer
)
{
    if (!volume || !volume->writable || !buffer ||
        block_number >= volume->blocks_count ||
        !volume->block_size || volume->block_size % BLOCK_SECTOR_SIZE)
        return false;
    uint32_t sectors_per_block =
        volume->block_size / BLOCK_SECTOR_SIZE;
    uint64_t relative_lba = (uint64_t)block_number * sectors_per_block;
    return raw_write(
        volume->device_index,
        volume->partition_lba,
        volume->partition_sectors,
        relative_lba,
        sectors_per_block,
        buffer);
}

static bool ext2_read_super(
    uint32_t device_index,
    uint64_t partition_lba,
    uint64_t partition_sectors,
    uint8_t superblock[EXT2_SUPER_BYTES]
)
{
    return raw_read(
        device_index,
        partition_lba,
        partition_sectors,
        EXT2_SUPER_OFFSET / BLOCK_SECTOR_SIZE,
        EXT2_SUPER_BYTES / BLOCK_SECTOR_SIZE,
        superblock);
}

static bool ext2_write_super(
    const ext2_volume_t *volume,
    const uint8_t superblock[EXT2_SUPER_BYTES]
)
{
    return raw_write(
        volume->device_index,
        volume->partition_lba,
        volume->partition_sectors,
        EXT2_SUPER_OFFSET / BLOCK_SECTOR_SIZE,
        EXT2_SUPER_BYTES / BLOCK_SECTOR_SIZE,
        superblock);
}

static bool ext2_read_group_desc(
    const ext2_volume_t *volume,
    uint32_t group,
    uint8_t descriptor[EXT2_GROUP_DESC_SIZE]
)
{
    if (!volume || group >= volume->group_count)
        return false;
    uint8_t block[4096];
    uint32_t byte_offset = group * EXT2_GROUP_DESC_SIZE;
    uint32_t table_block =
        volume->gdt_start + byte_offset / volume->block_size;
    uint32_t offset = byte_offset % volume->block_size;
    if (offset > volume->block_size - EXT2_GROUP_DESC_SIZE ||
        !ext2_read_block(volume, table_block, block))
        return false;
    bytes_copy(descriptor, block + offset, EXT2_GROUP_DESC_SIZE);
    return true;
}

static bool ext2_write_group_desc(
    ext2_volume_t *volume,
    uint32_t group,
    const uint8_t descriptor[EXT2_GROUP_DESC_SIZE]
)
{
    if (!volume || !volume->writable || group >= volume->group_count)
        return false;
    uint8_t block[4096];
    uint32_t byte_offset = group * EXT2_GROUP_DESC_SIZE;
    uint32_t table_block =
        volume->gdt_start + byte_offset / volume->block_size;
    uint32_t offset = byte_offset % volume->block_size;
    if (offset > volume->block_size - EXT2_GROUP_DESC_SIZE ||
        !ext2_read_block(volume, table_block, block))
        return false;
    bytes_copy(block + offset, descriptor, EXT2_GROUP_DESC_SIZE);
    if (!ext2_write_block(volume, table_block, block))
        return false;
    volume->metadata_dirty = true;
    return true;
}

static bool ext2_update_super_counts(
    ext2_volume_t *volume,
    int32_t free_blocks_delta,
    int32_t free_inodes_delta
)
{
    uint8_t superblock[EXT2_SUPER_BYTES];
    if (!ext2_read_super(
            volume->device_index,
            volume->partition_lba,
            volume->partition_sectors,
            superblock))
        return false;
    uint32_t free_blocks = read_le32(superblock + 12);
    uint32_t free_inodes = read_le32(superblock + 16);
    if ((free_blocks_delta < 0 &&
         free_blocks < (uint32_t)-free_blocks_delta) ||
        (free_inodes_delta < 0 &&
         free_inodes < (uint32_t)-free_inodes_delta) ||
        (free_blocks_delta > 0 &&
         free_blocks > UINT32_MAX - (uint32_t)free_blocks_delta) ||
        (free_inodes_delta > 0 &&
         free_inodes > UINT32_MAX - (uint32_t)free_inodes_delta))
        return false;
    free_blocks = (uint32_t)(free_blocks + free_blocks_delta);
    free_inodes = (uint32_t)(free_inodes + free_inodes_delta);
    write_le32(superblock + 12, free_blocks);
    write_le32(superblock + 16, free_inodes);
    if (!ext2_write_super(volume, superblock))
        return false;
    volume->metadata_dirty = true;
    for (uint32_t group = 1; group < volume->group_count; group++)
    {
        if (ext2_group_has_backup(group) &&
            !ext2_write_superblock_copy(volume, superblock, group))
            return false;
    }
    return true;
}

static bool ext2_inode_location(
    const ext2_volume_t *volume,
    uint32_t inode_number,
    uint32_t *table_block,
    uint32_t *offset
)
{
    if (!volume || !inode_number || inode_number > volume->inodes_count)
        return false;
    uint32_t group = (inode_number - 1) / volume->inodes_per_group;
    uint32_t index = (inode_number - 1) % volume->inodes_per_group;
    uint8_t descriptor[EXT2_GROUP_DESC_SIZE];
    if (!ext2_read_group_desc(volume, group, descriptor))
        return false;
    uint32_t inode_table = read_le32(descriptor + 8);
    uint64_t byte_offset = (uint64_t)index * volume->inode_size;
    uint64_t block_offset = byte_offset / volume->block_size;
    if (block_offset > UINT32_MAX - inode_table)
        return false;
    *table_block = inode_table + (uint32_t)block_offset;
    *offset = (uint32_t)(byte_offset % volume->block_size);
    return *offset <= volume->block_size - volume->inode_size;
}

static bool ext2_read_inode(
    const ext2_volume_t *volume,
    uint32_t inode_number,
    uint8_t inode[EXT2_INODE_MAX_SIZE]
)
{
    uint32_t block_number;
    uint32_t offset;
    uint8_t block[4096];
    if (!inode || !ext2_inode_location(
            volume, inode_number, &block_number, &offset) ||
        !ext2_read_block(volume, block_number, block))
        return false;
    bytes_copy(inode, block + offset, volume->inode_size);
    return true;
}

static bool ext2_write_inode(
    const ext2_volume_t *volume,
    uint32_t inode_number,
    const uint8_t inode[EXT2_INODE_MAX_SIZE]
)
{
    uint32_t block_number;
    uint32_t offset;
    uint8_t block[4096];
    if (!inode || !ext2_inode_location(
            volume, inode_number, &block_number, &offset) ||
        !ext2_read_block(volume, block_number, block))
        return false;
    bytes_copy(block + offset, inode, volume->inode_size);
    return ext2_write_block(volume, block_number, block);
}

static bool ext2_read_block_bitmap(
    const ext2_volume_t *volume,
    uint32_t group,
    uint8_t bitmap[4096],
    uint8_t descriptor[EXT2_GROUP_DESC_SIZE]
)
{
    return ext2_read_group_desc(volume, group, descriptor) &&
        ext2_read_block(volume, read_le32(descriptor), bitmap);
}

static bool ext2_adjust_free_blocks(
    ext2_volume_t *volume,
    uint32_t group,
    int32_t delta,
    uint8_t descriptor[EXT2_GROUP_DESC_SIZE]
)
{
    uint32_t free_blocks = read_le16(descriptor + 12);
    if ((delta < 0 && free_blocks < (uint32_t)-delta) ||
        (delta > 0 && free_blocks > UINT16_MAX - (uint32_t)delta))
        return false;
    write_le16(
        descriptor + 12,
        (uint16_t)(free_blocks + delta));
    return ext2_write_group_desc(volume, group, descriptor) &&
        ext2_update_super_counts(volume, delta, 0);
}

#define EXT2_ALLOCATION_BATCH_SIZE 32U

static bool ext2_flush_allocation_cache(ext2_volume_t *volume)
{
    if (!volume || !volume->allocation_cache_valid ||
        !volume->allocation_pending)
        return true;
    if (!ext2_write_block(
            volume,
            read_le32(volume->allocation_descriptor),
            volume->allocation_bitmap) ||
        !ext2_write_group_desc(
            volume,
            volume->allocation_group,
            volume->allocation_descriptor) ||
        !ext2_update_super_counts(
            volume,
            -(int32_t)volume->allocation_pending,
            0))
        return false;
    volume->allocation_pending = 0;
    return true;
}

static bool ext2_allocate_block(
    ext2_volume_t *volume,
    uint32_t *allocated_block,
    bool zero_contents
)
{
    if (!volume || !volume->mounted || !volume->writable ||
        !allocated_block)
        return false;
    uint8_t zero_block[4096];
    if (zero_contents)
        bytes_zero(zero_block, volume->block_size);
    uint32_t first_group = volume->allocation_cache_valid
        ? volume->allocation_group
        : 0;
    for (uint32_t attempt = 0; attempt < volume->group_count; attempt++)
    {
        uint32_t group = (first_group + attempt) % volume->group_count;
        if (!volume->allocation_cache_valid ||
            volume->allocation_group != group)
        {
            if (!ext2_flush_allocation_cache(volume))
                return false;
            volume->allocation_cache_valid = false;
            if (!ext2_read_block_bitmap(
                    volume,
                    group,
                    volume->allocation_bitmap,
                    volume->allocation_descriptor))
                return false;
            volume->allocation_group = group;
            volume->allocation_next_bit = 0;
            volume->allocation_pending = 0;
            volume->allocation_cache_valid = true;
        }
        if (!read_le16(volume->allocation_descriptor + 12))
        {
            if (!ext2_flush_allocation_cache(volume))
                return false;
            volume->allocation_cache_valid = false;
            continue;
        }

        uint32_t group_first =
            volume->first_data_block + group * volume->blocks_per_group;
        uint32_t valid_blocks = volume->blocks_count - group_first;
        if (valid_blocks > volume->blocks_per_group)
            valid_blocks = volume->blocks_per_group;
        for (uint32_t bit = volume->allocation_next_bit;
             bit < valid_blocks;
             bit++)
        {
            uint8_t mask = (uint8_t)(1U << (bit & 7));
            if (volume->allocation_bitmap[bit >> 3] & mask)
                continue;
            uint32_t block_number = group_first + bit;
            if (block_number < volume->first_data_block ||
                block_number >= volume->blocks_count)
                continue;
            if (zero_contents &&
                !ext2_write_block(volume, block_number, zero_block))
                return false;
            volume->allocation_bitmap[bit >> 3] |= mask;
            uint32_t free_blocks =
                read_le16(volume->allocation_descriptor + 12);
            if (!free_blocks)
                return false;
            write_le16(
                volume->allocation_descriptor + 12,
                (uint16_t)(free_blocks - 1));
            volume->allocation_next_bit = bit + 1;
            volume->allocation_pending++;
            if (volume->allocation_pending >=
                    EXT2_ALLOCATION_BATCH_SIZE &&
                !ext2_flush_allocation_cache(volume))
                return false;
            *allocated_block = block_number;
            return true;
        }
        if (!ext2_flush_allocation_cache(volume))
            return false;
        volume->allocation_cache_valid = false;
    }
    return false;
}

static bool ext2_free_block(ext2_volume_t *volume, uint32_t block_number)
{
    if (!volume || !volume->mounted || !volume->writable ||
        block_number < volume->first_data_block ||
        block_number >= volume->blocks_count)
        return false;
    if (!ext2_flush_allocation_cache(volume))
        return false;
    volume->allocation_cache_valid = false;
    uint32_t relative = block_number - volume->first_data_block;
    uint32_t group = relative / volume->blocks_per_group;
    uint32_t bit = relative % volume->blocks_per_group;
    uint8_t bitmap[4096];
    uint8_t descriptor[EXT2_GROUP_DESC_SIZE];
    if (!ext2_read_block_bitmap(volume, group, bitmap, descriptor))
        return false;
    uint8_t mask = (uint8_t)(1U << (bit & 7));
    if (!(bitmap[bit >> 3] & mask))
        return false;
    bitmap[bit >> 3] &= (uint8_t)~mask;
    return ext2_write_block(volume, read_le32(descriptor), bitmap) &&
        ext2_adjust_free_blocks(volume, group, 1, descriptor);
}

static bool ext2_allocate_inode(
    ext2_volume_t *volume,
    bool directory,
    uint32_t *allocated_inode
)
{
    if (!volume || !volume->mounted || !volume->writable ||
        !allocated_inode)
        return false;
    if (!ext2_flush_allocation_cache(volume))
        return false;
    volume->allocation_cache_valid = false;
    uint8_t bitmap[4096];
    for (uint32_t group = 0; group < volume->group_count; group++)
    {
        uint8_t descriptor[EXT2_GROUP_DESC_SIZE];
        if (!ext2_read_group_desc(volume, group, descriptor))
            return false;
        if (!read_le16(descriptor + 14))
            continue;
        uint32_t first_inode = group * volume->inodes_per_group + 1;
        uint32_t valid_inodes = volume->inodes_count - first_inode + 1;
        if (valid_inodes > volume->inodes_per_group)
            valid_inodes = volume->inodes_per_group;
        if (!ext2_read_block(volume, read_le32(descriptor + 4), bitmap))
            return false;
        for (uint32_t bit = 0; bit < valid_inodes; bit++)
        {
            uint32_t inode_number = first_inode + bit;
            if (inode_number < EXT2_FIRST_NON_RESERVED_INODE)
                continue;
            uint8_t mask = (uint8_t)(1U << (bit & 7));
            if (bitmap[bit >> 3] & mask)
                continue;
            bitmap[bit >> 3] |= mask;
            if (!ext2_write_block(
                    volume,
                    read_le32(descriptor + 4),
                    bitmap))
                return false;
            uint32_t free_inodes = read_le16(descriptor + 14);
            if (!free_inodes)
                return false;
            write_le16(descriptor + 14, (uint16_t)(free_inodes - 1));
            if (directory)
            {
                uint32_t directories = read_le16(descriptor + 16);
                if (directories == UINT16_MAX)
                    return false;
                write_le16(
                    descriptor + 16,
                    (uint16_t)(directories + 1));
            }
            if (!ext2_write_group_desc(volume, group, descriptor) ||
                !ext2_update_super_counts(volume, 0, -1))
                return false;
            *allocated_inode = inode_number;
            return true;
        }
    }
    return false;
}

static bool ext2_free_inode(
    ext2_volume_t *volume,
    uint32_t inode_number,
    bool directory
)
{
    if (!volume || inode_number < EXT2_FIRST_NON_RESERVED_INODE ||
        inode_number > volume->inodes_count)
        return false;
    if (!ext2_flush_allocation_cache(volume))
        return false;
    volume->allocation_cache_valid = false;
    uint32_t group = (inode_number - 1) / volume->inodes_per_group;
    uint32_t bit = (inode_number - 1) % volume->inodes_per_group;
    uint8_t bitmap[4096];
    uint8_t descriptor[EXT2_GROUP_DESC_SIZE];
    if (!ext2_read_group_desc(volume, group, descriptor) ||
        !ext2_read_block(volume, read_le32(descriptor + 4), bitmap))
        return false;
    uint8_t mask = (uint8_t)(1U << (bit & 7));
    if (!(bitmap[bit >> 3] & mask))
        return false;
    bitmap[bit >> 3] &= (uint8_t)~mask;
    if (!ext2_write_block(volume, read_le32(descriptor + 4), bitmap))
        return false;
    uint32_t free_inodes = read_le16(descriptor + 14);
    if (free_inodes == UINT16_MAX)
        return false;
    write_le16(descriptor + 14, (uint16_t)(free_inodes + 1));
    if (directory)
    {
        uint32_t directories = read_le16(descriptor + 16);
        if (!directories)
            return false;
        write_le16(descriptor + 16, (uint16_t)(directories - 1));
    }
    return ext2_write_group_desc(volume, group, descriptor) &&
        ext2_update_super_counts(volume, 0, 1);
}

static uint32_t inode_size_bytes(
    const ext2_volume_t *volume,
    const uint8_t inode[EXT2_INODE_MAX_SIZE]
)
{
    (void)volume;
    return read_le32(inode + 4);
}

static bool ext2_map_file_block(
    ext2_volume_t *volume,
    uint8_t inode[EXT2_INODE_MAX_SIZE],
    uint64_t file_block_index,
    bool allocate,
    bool zero_new_data_block,
    uint32_t *block_number
)
{
    uint32_t pointers_per_block = volume->block_size / sizeof(uint32_t);
    if (file_block_index < EXT2_DIRECT_BLOCKS)
    {
        uint32_t *unused = 0;
        (void)unused;
        uint32_t value = read_le32(
            inode + 40 + (uint32_t)file_block_index * 4);
        if (!value && allocate)
        {
            if (!ext2_allocate_block(
                    volume, &value, zero_new_data_block))
                return false;
            write_le32(
                inode + 40 + (uint32_t)file_block_index * 4,
                value);
            write_le32(inode + 28, read_le32(inode + 28) +
                volume->block_size / 512);
        }
        *block_number = value;
        return true;
    }

    uint64_t index = file_block_index - EXT2_DIRECT_BLOCKS;
    uint32_t level;
    if (index < pointers_per_block)
        level = 1;
    else
    {
        index -= pointers_per_block;
        uint64_t double_capacity =
            (uint64_t)pointers_per_block * pointers_per_block;
        if (index < double_capacity)
            level = 2;
        else
        {
            index -= double_capacity;
            uint64_t triple_capacity =
                double_capacity * pointers_per_block;
            if (index >= triple_capacity)
                return false;
            level = 3;
        }
    }

    uint32_t root_index = EXT2_DIRECT_BLOCKS + level - 1;
    uint32_t pointer_block = read_le32(inode + 40 + root_index * 4);
    if (!pointer_block)
    {
        if (!allocate ||
            !ext2_allocate_block(volume, &pointer_block, true))
            return allocate ? false : (*block_number = 0, true);
        write_le32(inode + 40 + root_index * 4, pointer_block);
        write_le32(inode + 28, read_le32(inode + 28) +
            volume->block_size / 512);
    }

    uint8_t pointer_data[4096];
    for (uint32_t depth = level; depth > 0; depth--)
    {
        if (!ext2_read_block(volume, pointer_block, pointer_data))
            return false;
        uint32_t divisor = 1;
        for (uint32_t i = 1; i < depth; i++)
            divisor *= pointers_per_block;
        uint32_t pointer_index = (uint32_t)(index / divisor);
        index %= divisor;
        if (pointer_index >= pointers_per_block)
            return false;
        uint32_t value = read_le32(pointer_data + pointer_index * 4);
        if (depth == 1)
        {
            if (!value && allocate)
            {
                if (!ext2_allocate_block(
                        volume, &value, zero_new_data_block))
                    return false;
                write_le32(pointer_data + pointer_index * 4, value);
                if (!ext2_write_block(volume, pointer_block, pointer_data))
                    return false;
                write_le32(inode + 28, read_le32(inode + 28) +
                    volume->block_size / 512);
            }
            *block_number = value;
            return true;
        }
        if (!value)
        {
            if (!allocate ||
                !ext2_allocate_block(volume, &value, true))
                return allocate ? false : (*block_number = 0, true);
            write_le32(pointer_data + pointer_index * 4, value);
            if (!ext2_write_block(volume, pointer_block, pointer_data))
                return false;
            write_le32(inode + 28, read_le32(inode + 28) +
                volume->block_size / 512);
        }
        pointer_block = value;
    }
    return false;
}

static bool ext2_free_indirect(
    ext2_volume_t *volume,
    uint32_t block_number,
    uint32_t depth
)
{
    if (!block_number)
        return true;
    uint8_t pointer_data[4096];
    if (depth)
    {
        if (!ext2_read_block(volume, block_number, pointer_data))
            return false;
        uint32_t pointers = volume->block_size / sizeof(uint32_t);
        for (uint32_t i = 0; i < pointers; i++)
        {
            uint32_t child = read_le32(pointer_data + i * 4);
            if (child && !ext2_free_indirect(
                    volume, child, depth - 1))
                return false;
        }
    }
    return ext2_free_block(volume, block_number);
}

static bool ext2_truncate_inode(
    ext2_volume_t *volume,
    uint32_t inode_number,
    uint8_t inode[EXT2_INODE_MAX_SIZE]
)
{
    for (uint32_t i = 0; i < EXT2_DIRECT_BLOCKS; i++)
    {
        uint32_t block = read_le32(inode + 40 + i * 4);
        if (block && !ext2_free_block(volume, block))
            return false;
        write_le32(inode + 40 + i * 4, 0);
    }
    for (uint32_t level = 1; level <= 3; level++)
    {
        uint32_t index = EXT2_DIRECT_BLOCKS + level - 1;
        uint32_t block = read_le32(inode + 40 + index * 4);
        if (block && !ext2_free_indirect(volume, block, level))
            return false;
        write_le32(inode + 40 + index * 4, 0);
    }
    write_le32(inode + 4, 0);
    write_le32(inode + 108, 0);
    write_le32(inode + 28, 0);
    write_le32(inode + 20, 0);
    (void)inode_number;
    return ext2_write_inode(volume, inode_number, inode);
}

static bool ext2_directory_find(
    ext2_volume_t *volume,
    uint32_t directory_inode,
    const char *name,
    size_t name_length,
    uint32_t *found_inode,
    uint8_t *found_type
)
{
    uint8_t inode[EXT2_INODE_MAX_SIZE];
    if (!ext2_read_inode(volume, directory_inode, inode) ||
        (read_le16(inode) & EXT2_MODE_TYPE_MASK) != EXT2_MODE_DIRECTORY)
        return false;
    uint64_t size = read_le32(inode + 4);
    uint64_t offset = 0;
    uint8_t block[4096];
    while (offset < size)
    {
        uint32_t block_number;
        if (!ext2_map_file_block(
                volume, inode, offset / volume->block_size,
                false, false, &block_number) ||
            !block_number ||
            !ext2_read_block(volume, block_number, block))
            return false;
        uint32_t in_block = (uint32_t)(offset % volume->block_size);
        uint32_t consumed = 0;
        while (consumed < volume->block_size && offset < size)
        {
            uint8_t *entry = block + consumed;
            uint32_t entry_inode = read_le32(entry);
            uint16_t record_length = read_le16(entry + 4);
            uint8_t entry_name_length = entry[6];
            uint8_t entry_type = entry[7];
            if (record_length < EXT2_DIR_ENTRY_HEADER ||
                (record_length & 3) ||
                record_length > volume->block_size - consumed ||
                entry_name_length > record_length - EXT2_DIR_ENTRY_HEADER)
                return false;
            if (entry_inode && entry_name_length == name_length)
            {
                size_t i = 0;
                while (i < name_length && entry[8 + i] == name[i])
                    i++;
                if (i == name_length)
                {
                    if (found_inode)
                        *found_inode = entry_inode;
                    if (found_type)
                        *found_type = entry_type;
                    return true;
                }
            }
            consumed += record_length;
            offset += record_length;
        }
        if (!consumed || in_block >= volume->block_size)
            return false;
    }
    return false;
}

static bool ext2_resolve_path(
    ext2_volume_t *volume,
    const char *path,
    uint32_t *inode_number,
    uint8_t inode[EXT2_INODE_MAX_SIZE]
)
{
    if (!volume || !volume->mounted || !path)
        return false;
    const char *cursor = path;
    while (*cursor == '/')
        cursor++;
    uint32_t current = EXT2_ROOT_INODE;
    uint8_t current_inode[EXT2_INODE_MAX_SIZE];
    if (!ext2_read_inode(volume, current, current_inode))
        return false;
    while (*cursor)
    {
        const char *component = cursor;
        while (*cursor && *cursor != '/')
            cursor++;
        size_t length = (size_t)(cursor - component);
        if (!length || length > EXT2_NAME_MAX ||
            (length == 1 && component[0] == '.') ||
            (length == 2 && component[0] == '.' &&
             component[1] == '.'))
            return false;
        if ((read_le16(current_inode) & EXT2_MODE_TYPE_MASK) !=
            EXT2_MODE_DIRECTORY)
            return false;
        if (!ext2_directory_find(
                volume, current, component, length, &current, NULL) ||
            !ext2_read_inode(volume, current, current_inode))
            return false;
        while (*cursor == '/')
            cursor++;
    }
    if (inode_number)
        *inode_number = current;
    if (inode)
        bytes_copy(inode, current_inode, volume->inode_size);
    return true;
}

static bool ext2_parent_and_name(
    ext2_volume_t *volume,
    const char *path,
    uint32_t *parent_inode,
    uint8_t parent_data[EXT2_INODE_MAX_SIZE],
    char name[EXT2_NAME_MAX + 1]
)
{
    if (!path || !*path)
        return false;
    const char *start = path;
    while (*start == '/')
        start++;
    if (!*start)
        return false;
    const char *last = start;
    for (const char *cursor = start; *cursor; cursor++)
    {
        if (*cursor == '/')
            last = cursor + 1;
    }
    size_t name_length = 0;
    while (last[name_length])
        name_length++;
    if (!name_length || name_length > EXT2_NAME_MAX ||
        (name_length == 1 && last[0] == '.') ||
        (name_length == 2 && last[0] == '.' && last[1] == '.'))
        return false;
    for (size_t i = 0; i < name_length; i++)
        name[i] = last[i];
    name[name_length] = '\0';

    size_t parent_length = (size_t)(last - start);
    char parent_path[EXT2_MOUNT_PATH_MAX];
    if (parent_length >= sizeof(parent_path))
        return false;
    if (!parent_length)
        parent_path[0] = '\0';
    else
    {
        while (parent_length && start[parent_length - 1] == '/')
            parent_length--;
        for (size_t i = 0; i < parent_length; i++)
            parent_path[i] = start[i];
        parent_path[parent_length] = '\0';
    }
    return ext2_resolve_path(
        volume,
        parent_path,
        parent_inode,
        parent_data);
}

static bool ext2_directory_insert(
    ext2_volume_t *volume,
    uint32_t directory_inode,
    uint8_t directory_data[EXT2_INODE_MAX_SIZE],
    uint32_t child_inode,
    const char *name,
    uint8_t file_type
)
{
    size_t name_length = 0;
    while (name[name_length])
        name_length++;
    if (!name_length || name_length > EXT2_NAME_MAX)
        return false;
    uint32_t needed = (EXT2_DIR_ENTRY_HEADER + (uint32_t)name_length + 3) &
        ~3U;
    uint64_t directory_size = read_le32(directory_data + 4);
    uint8_t block[4096];
    for (uint64_t offset = 0; offset < directory_size;)
    {
        uint32_t block_number;
        if (!ext2_map_file_block(
                volume,
                directory_data,
                offset / volume->block_size,
                false,
                false,
                &block_number) ||
            !block_number ||
            !ext2_read_block(volume, block_number, block))
            return false;
        uint32_t consumed = 0;
        while (consumed < volume->block_size && offset < directory_size)
        {
            uint8_t *entry = block + consumed;
            uint16_t record_length = read_le16(entry + 4);
            if (record_length < EXT2_DIR_ENTRY_HEADER ||
                (record_length & 3) ||
                record_length > volume->block_size - consumed)
                return false;
            uint32_t entry_inode = read_le32(entry);
            uint32_t available = record_length;
            if (entry_inode)
            {
                uint32_t old_name_length = entry[6];
                uint32_t old_used =
                    (EXT2_DIR_ENTRY_HEADER + old_name_length + 3) & ~3U;
                if (old_used > record_length)
                    return false;
                available -= old_used;
                if (available >= needed)
                {
                    write_le16(entry + 4, (uint16_t)old_used);
                    entry += old_used;
                    record_length = (uint16_t)available;
                }
                else
                {
                    consumed += read_le16(entry + 4);
                    offset += read_le16(entry + 4);
                    continue;
                }
            }
            if (record_length < needed)
                return false;
            bytes_zero(entry, record_length);
            write_le32(entry, child_inode);
            write_le16(entry + 4, record_length);
            entry[6] = (uint8_t)name_length;
            entry[7] = file_type;
            bytes_copy(entry + 8, name, name_length);
            if (!ext2_write_block(volume, block_number, block) ||
                !ext2_write_inode(volume, directory_inode, directory_data))
                return false;
            return true;
        }
        if (!consumed)
            return false;
    }

    uint64_t block_index = directory_size / volume->block_size;
    if (directory_size % volume->block_size)
        return false;
    uint32_t block_number;
    if (!ext2_map_file_block(
            volume, directory_data, block_index, true, true, &block_number))
        return false;
    bytes_zero(block, volume->block_size);
    write_le32(block, child_inode);
    write_le16(block + 4, (uint16_t)volume->block_size);
    block[6] = (uint8_t)name_length;
    block[7] = file_type;
    bytes_copy(block + 8, name, name_length);
    write_le32(directory_data + 4,
        (uint32_t)(directory_size + volume->block_size));
    return ext2_write_block(volume, block_number, block) &&
        ext2_write_inode(volume, directory_inode, directory_data);
}

bool ext2_mount(
    ext2_volume_t *volume,
    uint32_t device_index,
    uint64_t partition_lba,
    uint64_t partition_sectors,
    const char *mount_path
)
{
    if (!volume || !mount_path || mount_path[0] != '/' ||
        !partition_sectors)
        return false;
    const block_device_t *device = block_device_get(device_index);
    if (!device || !device->writable)
        return false;
    uint8_t superblock[EXT2_SUPER_BYTES];
    if (!ext2_read_super(
            device_index,
            partition_lba,
            partition_sectors,
            superblock) ||
        read_le16(superblock + 56) != EXT2_SUPER_MAGIC)
        return false;

    uint32_t log_block_size = read_le32(superblock + 24);
    if (log_block_size > 2)
        return false;
    uint32_t block_size = 1024U << log_block_size;
    uint32_t inode_size = read_le32(superblock + 76) == 0
        ? EXT2_INODE_MIN_SIZE
        : read_le16(superblock + 88);
    uint32_t incompat = read_le32(superblock + 96);
    uint32_t compat = read_le32(superblock + 92);
    uint32_t ro_compat = read_le32(superblock + 100);
    uint32_t blocks_count = read_le32(superblock + 4);
    uint32_t inodes_count = read_le32(superblock);
    uint32_t first_data_block = read_le32(superblock + 20);
    uint32_t blocks_per_group = read_le32(superblock + 32);
    uint32_t inodes_per_group = read_le32(superblock + 40);
    if (partition_sectors > UINT64_MAX / BLOCK_SECTOR_SIZE)
        return false;
    uint64_t partition_blocks =
        (partition_sectors * BLOCK_SECTOR_SIZE) / block_size;
    uint64_t partition_bytes = partition_sectors * BLOCK_SECTOR_SIZE;
    if (partition_bytes / block_size != partition_blocks ||
        blocks_count < 16 || blocks_count > partition_blocks ||
        !inodes_count || !blocks_per_group || !inodes_per_group ||
        first_data_block != (block_size == 1024 ? 1U : 0U) ||
        inode_size < EXT2_INODE_MIN_SIZE ||
        inode_size > EXT2_INODE_MAX_SIZE ||
        inode_size > block_size ||
        block_size % inode_size ||
        (incompat & ~EXT2_FEATURE_INCOMPAT_FILETYPE) ||
        (compat & EXT2_FEATURE_COMPAT_HAS_JOURNAL) ||
        (ro_compat & ~EXT2_FEATURE_RO_COMPAT_SUPPORTED))
        return false;

    uint32_t groups = divide_round_up(
        blocks_count - first_data_block,
        blocks_per_group);
    if (!groups ||
        groups != divide_round_up(inodes_count, inodes_per_group))
        return false;

    *volume = (ext2_volume_t) {
        .device_index = device_index,
        .partition_lba = partition_lba,
        .partition_sectors = partition_sectors,
        .block_size = block_size,
        .blocks_per_group = blocks_per_group,
        .inodes_per_group = inodes_per_group,
        .inode_size = inode_size,
        .first_data_block = first_data_block,
        .group_count = groups,
        .gdt_start = first_data_block + 1,
        .blocks_count = blocks_count,
        .inodes_count = inodes_count,
        .feature_compat = compat,
        .feature_incompat = incompat,
        .feature_ro_compat = ro_compat,
        .mounted = false,
        .writable = true
    };
    for (uint32_t i = 0; i < 16 && superblock[120 + i]; i++)
        volume->volume_name[i] = (char)superblock[120 + i];
    volume->volume_name[16] = '\0';
    if (!string_copy(
            volume->mount_path,
            sizeof(volume->mount_path),
            mount_path))
        return false;

    uint8_t root_inode[EXT2_INODE_MAX_SIZE];
    if (!ext2_read_inode(volume, EXT2_ROOT_INODE, root_inode) ||
        (read_le16(root_inode) & EXT2_MODE_TYPE_MASK) !=
            EXT2_MODE_DIRECTORY)
    {
        *volume = (ext2_volume_t){0};
        return false;
    }
    volume->mounted = true;
    return true;
}

void ext2_unmount(ext2_volume_t *volume)
{
    if (volume && !volume->open_files && ext2_sync(volume))
    {
        *volume = (ext2_volume_t){0};
    }
}

bool ext2_path_matches(
    const ext2_volume_t *volume,
    const char *path,
    const char **relative_path
)
{
    if (!volume || !volume->mounted || !path)
        return false;
    size_t length = 0;
    while (volume->mount_path[length])
        length++;
    for (size_t i = 0; i < length; i++)
    {
        if (path[i] != volume->mount_path[i])
            return false;
    }
    if (length == 1 && volume->mount_path[0] == '/' &&
        path[0] == '/')
    {
        if (relative_path)
            *relative_path = path;
        return true;
    }
    if (path[length] && path[length] != '/')
        return false;
    if (relative_path)
    {
        *relative_path = path + length;
        while (**relative_path == '/')
            (*relative_path)++;
    }
    return true;
}

bool ext2_is_directory(ext2_volume_t *volume, const char *path)
{
    uint8_t inode[EXT2_INODE_MAX_SIZE];
    if (!ext2_resolve_path(volume, path, NULL, inode))
        return false;
    return (read_le16(inode) & EXT2_MODE_TYPE_MASK) ==
        EXT2_MODE_DIRECTORY;
}

static int64_t ext2_create_regular(
    ext2_volume_t *volume,
    const char *path,
    uint32_t *new_inode
)
{
    uint32_t parent_number;
    uint8_t parent[EXT2_INODE_MAX_SIZE];
    char name[EXT2_NAME_MAX + 1];
    if (!ext2_parent_and_name(
            volume, path, &parent_number, parent, name))
        return -SYSCALL_ENOENT;
    size_t name_length = 0;
    while (name[name_length])
        name_length++;
    uint32_t existing;
    if (ext2_directory_find(
            volume, parent_number, name, name_length,
            &existing, NULL))
        return -SYSCALL_EEXIST;
    uint32_t inode_number;
    if (!ext2_allocate_inode(volume, false, &inode_number))
        return -SYSCALL_ENOSPC;
    uint8_t inode[EXT2_INODE_MAX_SIZE];
    bytes_zero(inode, volume->inode_size);
    write_le16(inode, EXT2_MODE_REGULAR | 0644);
    write_le16(inode + 26, 1);
    if (!ext2_write_inode(volume, inode_number, inode) ||
        !ext2_directory_insert(
            volume, parent_number, parent, inode_number, name,
            EXT2_DIR_TYPE_REGULAR))
    {
        (void)ext2_free_inode(volume, inode_number, false);
        return -SYSCALL_EIO;
    }
    *new_inode = inode_number;
    return 0;
}

int64_t ext2_open(
    ext2_volume_t *volume,
    const char *path,
    bool create,
    bool truncate,
    ext2_file_t *file
)
{
    if (!volume || !volume->mounted || !path || !file)
        return -SYSCALL_EINVAL;
    uint8_t inode[EXT2_INODE_MAX_SIZE];
    uint32_t inode_number;
    if (!ext2_resolve_path(volume, path, &inode_number, inode))
    {
        if (!create)
            return -SYSCALL_ENOENT;
        int64_t result = ext2_create_regular(
            volume, path, &inode_number);
        if (result < 0)
            return result;
        if (!ext2_read_inode(volume, inode_number, inode))
            return -SYSCALL_EIO;
    }
    uint16_t mode = read_le16(inode);
    if ((mode & EXT2_MODE_TYPE_MASK) == EXT2_MODE_DIRECTORY)
        return -SYSCALL_EISDIR;
    if ((mode & EXT2_MODE_TYPE_MASK) != EXT2_MODE_REGULAR)
        return -SYSCALL_EINVAL;
    if (truncate)
    {
        if (!volume->writable ||
            !ext2_truncate_inode(volume, inode_number, inode))
            return -SYSCALL_EIO;
    }
    if (volume->open_files == UINT32_MAX)
        return -SYSCALL_ENOMEM;
    *file = (ext2_file_t) {
        .volume = volume,
        .inode = inode_number,
        .size = inode_size_bytes(volume, inode),
        .offset = 0,
        .is_directory = false,
        .open = true
    };
    volume->open_files++;
    return 0;
}

bool ext2_clone_file(ext2_file_t *destination, const ext2_file_t *source)
{
    if (!destination || !source || !source->open || !source->volume ||
        !source->volume->mounted ||
        source->volume->open_files == UINT32_MAX)
        return false;
    *destination = *source;
    destination->volume->open_files++;
    return true;
}

void ext2_close(ext2_file_t *file)
{
    if (!file || !file->open || !file->volume)
        return;
    if (file->volume->open_files)
        file->volume->open_files--;
    file->open = false;
}

int64_t ext2_read(ext2_file_t *file, void *buffer, uint64_t size)
{
    if (!file || !file->volume || !file->volume->mounted ||
        (!buffer && size) || file->is_directory)
        return -SYSCALL_EINVAL;
    if (file->offset >= file->size)
        return 0;
    if (size > file->size - file->offset)
        size = file->size - file->offset;
    uint8_t inode[EXT2_INODE_MAX_SIZE];
    if (!ext2_read_inode(file->volume, file->inode, inode))
        return -SYSCALL_EIO;
    uint8_t block[4096];
    uint64_t copied = 0;
    while (copied < size)
    {
        uint64_t current_offset = file->offset + copied;
        uint32_t block_offset =
            (uint32_t)(current_offset % file->volume->block_size);
        uint64_t amount =
            file->volume->block_size - block_offset;
        if (amount > size - copied)
            amount = size - copied;
        uint32_t block_number;
        if (!ext2_map_file_block(
                file->volume,
                inode,
                current_offset / file->volume->block_size,
                false,
                false,
                &block_number))
            return copied ? (int64_t)copied : -SYSCALL_EIO;
        if (block_number)
        {
            if (!ext2_read_block(
                    file->volume, block_number, block))
                return copied ? (int64_t)copied : -SYSCALL_EIO;
            bytes_copy(
                (uint8_t *)buffer + copied,
                block + block_offset,
                (size_t)amount);
        }
        else
        {
            bytes_zero((uint8_t *)buffer + copied, (size_t)amount);
        }
        copied += amount;
    }
    file->offset += copied;
    return (int64_t)copied;
}

int64_t ext2_write(ext2_file_t *file, const void *buffer, uint64_t size)
{
    if (!file || !file->volume || !file->volume->mounted ||
        !file->volume->writable || (!buffer && size) ||
        file->is_directory)
        return -SYSCALL_EINVAL;
    if (!size)
        return 0;
    if (file->offset > UINT32_MAX || size > UINT32_MAX - file->offset)
        return -SYSCALL_ENOSPC;
    uint8_t inode[EXT2_INODE_MAX_SIZE];
    if (!ext2_read_inode(file->volume, file->inode, inode))
        return -SYSCALL_EIO;
    uint8_t block[4096];
    uint64_t written = 0;
    while (written < size)
    {
        uint64_t current_offset = file->offset + written;
        uint32_t block_offset =
            (uint32_t)(current_offset % file->volume->block_size);
        uint64_t amount =
            file->volume->block_size - block_offset;
        if (amount > size - written)
            amount = size - written;
        uint32_t block_number;
        if (!ext2_map_file_block(
                file->volume,
                inode,
                current_offset / file->volume->block_size,
                true,
                block_offset || amount != file->volume->block_size,
                &block_number))
            return written ? (int64_t)written : -SYSCALL_ENOSPC;
        if ((block_offset || amount != file->volume->block_size) &&
            !ext2_read_block(file->volume, block_number, block))
            return written ? (int64_t)written : -SYSCALL_EIO;
        bytes_copy(
            block + block_offset,
            (const uint8_t *)buffer + written,
            (size_t)amount);
        if (!ext2_write_block(file->volume, block_number, block))
            return written ? (int64_t)written : -SYSCALL_EIO;
        written += amount;
    }
    file->offset += written;
    if (file->offset > file->size)
        file->size = file->offset;
    write_le32(inode + 4, (uint32_t)file->size);
    write_le32(inode + 108, 0);
    if (!ext2_write_inode(file->volume, file->inode, inode))
        return written ? (int64_t)written : -SYSCALL_EIO;
    return (int64_t)written;
}

int64_t ext2_mkdir(ext2_volume_t *volume, const char *path)
{
    if (!volume || !volume->mounted || !volume->writable || !path)
        return -SYSCALL_EINVAL;
    uint32_t parent_number;
    uint8_t parent[EXT2_INODE_MAX_SIZE];
    char name[EXT2_NAME_MAX + 1];
    if (!ext2_parent_and_name(
            volume, path, &parent_number, parent, name))
        return -SYSCALL_ENOENT;
    uint32_t existing;
    size_t name_length = 0;
    while (name[name_length])
        name_length++;
    if (ext2_directory_find(
            volume, parent_number, name, name_length,
            &existing, NULL))
        return -SYSCALL_EEXIST;

    uint32_t inode_number;
    uint32_t data_block;
    if (!ext2_allocate_inode(volume, true, &inode_number))
        return -SYSCALL_ENOSPC;
    if (!ext2_allocate_block(volume, &data_block, true))
    {
        (void)ext2_free_inode(volume, inode_number, true);
        return -SYSCALL_ENOSPC;
    }
    uint8_t inode[EXT2_INODE_MAX_SIZE];
    bytes_zero(inode, volume->inode_size);
    write_le16(inode, EXT2_MODE_DIRECTORY | 0755);
    write_le32(inode + 4, volume->block_size);
    write_le32(inode + 28, volume->block_size / 512);
    write_le16(inode + 26, 2);
    write_le32(inode + 40, data_block);

    uint8_t block[4096];
    bytes_zero(block, volume->block_size);
    write_le32(block, inode_number);
    write_le16(block + 4, 12);
    block[6] = 1;
    block[7] = EXT2_DIR_TYPE_DIRECTORY;
    block[8] = '.';
    write_le32(block + 12, parent_number);
    write_le16(block + 16, (uint16_t)(volume->block_size - 12));
    block[18] = 2;
    block[19] = EXT2_DIR_TYPE_DIRECTORY;
    block[20] = '.';
    block[21] = '.';
    if (!ext2_write_block(volume, data_block, block) ||
        !ext2_write_inode(volume, inode_number, inode))
    {
        (void)ext2_free_block(volume, data_block);
        (void)ext2_free_inode(volume, inode_number, true);
        return -SYSCALL_EIO;
    }
    uint32_t parent_links = read_le16(parent + 26);
    if (parent_links == UINT16_MAX)
        return -SYSCALL_ENOSPC;
    write_le16(parent + 26, (uint16_t)(parent_links + 1));
    if (!ext2_directory_insert(
            volume,
            parent_number,
            parent,
            inode_number,
            name,
            EXT2_DIR_TYPE_DIRECTORY))
        return -SYSCALL_EIO;
    return 0;
}

static bool ext2_directory_remove(
    ext2_volume_t *volume,
    uint32_t directory_inode,
    uint8_t directory_data[EXT2_INODE_MAX_SIZE],
    uint32_t target_inode,
    const char *name
)
{
    size_t name_length = 0;
    while (name[name_length])
        name_length++;
    uint64_t directory_size = read_le32(directory_data + 4);
    uint8_t block[4096];
    for (uint64_t offset = 0; offset < directory_size;)
    {
        uint32_t block_number;
        if (!ext2_map_file_block(
                volume,
                directory_data,
                offset / volume->block_size,
                false,
                false,
                &block_number) ||
            !block_number ||
            !ext2_read_block(volume, block_number, block))
            return false;
        uint32_t consumed = 0;
        while (consumed < volume->block_size && offset < directory_size)
        {
            uint8_t *entry = block + consumed;
            uint32_t entry_inode = read_le32(entry);
            uint16_t record_length = read_le16(entry + 4);
            uint8_t entry_name_length = entry[6];
            if (record_length < EXT2_DIR_ENTRY_HEADER ||
                (record_length & 3) ||
                record_length > volume->block_size - consumed ||
                entry_name_length > record_length - EXT2_DIR_ENTRY_HEADER)
                return false;
            bool name_matches = entry_name_length == name_length;
            for (size_t i = 0; name_matches && i < name_length; i++)
                name_matches = entry[8 + i] == (uint8_t)name[i];
            if (entry_inode == target_inode && name_matches)
            {
                write_le32(entry, 0);
                entry[6] = 0;
                entry[7] = EXT2_DIR_TYPE_UNKNOWN;
                return ext2_write_block(volume, block_number, block) &&
                    ext2_write_inode(
                        volume, directory_inode, directory_data);
            }
            consumed += record_length;
            offset += record_length;
        }
        if (!consumed)
            return false;
    }
    return false;
}

int64_t ext2_unlink(ext2_volume_t *volume, const char *path)
{
    if (!volume || !volume->mounted || !volume->writable || !path)
        return -SYSCALL_EINVAL;

    uint32_t parent_number;
    uint8_t parent[EXT2_INODE_MAX_SIZE];
    char name[EXT2_NAME_MAX + 1];
    if (!ext2_parent_and_name(
            volume, path, &parent_number, parent, name))
        return -SYSCALL_ENOENT;

    size_t name_length = 0;
    while (name[name_length])
        name_length++;
    uint32_t inode_number;
    if (!ext2_directory_find(
            volume,
            parent_number,
            name,
            name_length,
            &inode_number,
            NULL))
        return -SYSCALL_ENOENT;

    uint8_t inode[EXT2_INODE_MAX_SIZE];
    if (!ext2_read_inode(volume, inode_number, inode))
        return -SYSCALL_EIO;
    uint16_t mode = read_le16(inode);
    if ((mode & EXT2_MODE_TYPE_MASK) == EXT2_MODE_DIRECTORY)
        return -SYSCALL_EISDIR;
    if ((mode & EXT2_MODE_TYPE_MASK) != EXT2_MODE_REGULAR)
        return -SYSCALL_ENOTSUP;

    if (!ext2_truncate_inode(volume, inode_number, inode) ||
        !ext2_directory_remove(
            volume,
            parent_number,
            parent,
            inode_number,
            name))
        return -SYSCALL_EIO;
    bytes_zero(inode, volume->inode_size);
    return ext2_write_inode(volume, inode_number, inode) &&
            ext2_free_inode(volume, inode_number, false)
        ? 0
        : -SYSCALL_EIO;
}

int64_t ext2_rmdir(ext2_volume_t *volume, const char *path)
{
    if (!volume || !volume->mounted || !volume->writable || !path)
        return -SYSCALL_EINVAL;

    uint32_t parent_number;
    uint8_t parent[EXT2_INODE_MAX_SIZE];
    char name[EXT2_NAME_MAX + 1];
    if (!ext2_parent_and_name(
            volume, path, &parent_number, parent, name))
        return -SYSCALL_ENOENT;

    size_t name_length = 0;
    while (name[name_length])
        name_length++;
    uint32_t inode_number;
    if (!ext2_directory_find(
            volume,
            parent_number,
            name,
            name_length,
            &inode_number,
            NULL))
        return -SYSCALL_ENOENT;

    uint8_t inode[EXT2_INODE_MAX_SIZE];
    if (!ext2_read_inode(volume, inode_number, inode))
        return -SYSCALL_EIO;
    if ((read_le16(inode) & EXT2_MODE_TYPE_MASK) != EXT2_MODE_DIRECTORY)
        return -SYSCALL_ENOTDIR;

    ext2_dirent_t child;
    int64_t child_count = ext2_list(volume, path, 0, &child, 1);
    if (child_count < 0)
        return child_count;
    if (child_count)
        return -SYSCALL_ENOTEMPTY;

    uint32_t parent_links = read_le16(parent + 26);
    if (!parent_links)
        return -SYSCALL_EIO;
    if (!ext2_directory_remove(
            volume,
            parent_number,
            parent,
            inode_number,
            name))
        return -SYSCALL_EIO;
    write_le16(parent + 26, (uint16_t)(parent_links - 1));
    if (!ext2_write_inode(volume, parent_number, parent) ||
        !ext2_truncate_inode(volume, inode_number, inode))
        return -SYSCALL_EIO;
    bytes_zero(inode, volume->inode_size);
    return ext2_write_inode(volume, inode_number, inode) &&
            ext2_free_inode(volume, inode_number, true)
        ? 0
        : -SYSCALL_EIO;
}

int64_t ext2_list(
    ext2_volume_t *volume,
    const char *path,
    uint64_t offset,
    ext2_dirent_t *entries,
    uint64_t capacity
)
{
    if (!volume || !volume->mounted || !path ||
        (capacity && !entries))
        return -SYSCALL_EINVAL;
    uint32_t directory_number;
    uint8_t inode[EXT2_INODE_MAX_SIZE];
    if (!ext2_resolve_path(volume, path, &directory_number, inode))
        return -SYSCALL_ENOENT;
    if ((read_le16(inode) & EXT2_MODE_TYPE_MASK) != EXT2_MODE_DIRECTORY)
        return -SYSCALL_ENOTDIR;

    uint64_t size = read_le32(inode + 4);
    uint64_t file_offset = 0;
    uint64_t entry_index = 0;
    uint64_t returned = 0;
    uint8_t block[4096];
    while (file_offset < size && returned < capacity)
    {
        uint32_t block_number;
        if (!ext2_map_file_block(
                volume, inode, file_offset / volume->block_size,
                false, false, &block_number) ||
            !block_number ||
            !ext2_read_block(volume, block_number, block))
            return returned ? (int64_t)returned : -SYSCALL_EIO;
        uint32_t consumed = 0;
        while (consumed < volume->block_size && file_offset < size)
        {
            uint8_t *entry = block + consumed;
            uint32_t child_inode = read_le32(entry);
            uint16_t record_length = read_le16(entry + 4);
            uint8_t name_length = entry[6];
            uint8_t file_type = entry[7];
            if (record_length < EXT2_DIR_ENTRY_HEADER ||
                (record_length & 3) ||
                record_length > volume->block_size - consumed ||
                name_length > record_length - EXT2_DIR_ENTRY_HEADER)
                return returned ? (int64_t)returned : -SYSCALL_EIO;
            if (child_inode && name_length &&
                !(name_length == 1 && entry[8] == '.') &&
                !(name_length == 2 && entry[8] == '.' &&
                  entry[9] == '.'))
            {
                if (entry_index >= offset)
                {
                    ext2_dirent_t *output = &entries[returned++];
                    bytes_zero(output, sizeof(*output));
                    bytes_copy(output->name, entry + 8, name_length);
                    output->name[name_length] = '\0';
                    output->inode = child_inode;
                    output->is_directory =
                        file_type == EXT2_DIR_TYPE_DIRECTORY;
                    uint8_t child[EXT2_INODE_MAX_SIZE];
                    if (ext2_read_inode(volume, child_inode, child))
                        output->size = inode_size_bytes(volume, child);
                    if (returned == capacity)
                        break;
                }
                entry_index++;
            }
            consumed += record_length;
            file_offset += record_length;
        }
        if (!consumed)
            return returned ? (int64_t)returned : -SYSCALL_EIO;
    }
    (void)directory_number;
    return (int64_t)returned;
}

static bool ext2_group_has_backup(uint32_t group)
{
    if (group == 0 || group == 1)
        return true;
    if (!group)
        return false;
    for (uint32_t base = 3; base <= group; base *= 3)
    {
        if (base == group)
            return true;
        if (base > UINT32_MAX / 3)
            break;
    }
    for (uint32_t base = 5; base <= group; base *= 5)
    {
        if (base == group)
            return true;
        if (base > UINT32_MAX / 5)
            break;
    }
    for (uint32_t base = 7; base <= group; base *= 7)
    {
        if (base == group)
            return true;
        if (base > UINT32_MAX / 7)
            break;
    }
    return false;
}

static bool ext2_write_superblock_copy(
    const ext2_volume_t *volume,
    const uint8_t superblock[EXT2_SUPER_BYTES],
    uint32_t group
)
{
    uint8_t block[4096];
    bytes_zero(block, volume->block_size);
    uint32_t superblock_offset =
        volume->block_size == 1024 ? 0 : EXT2_SUPER_OFFSET;
    bytes_copy(
        block + superblock_offset,
        superblock,
        EXT2_SUPER_BYTES);
    write_le16(block + 90, (uint16_t)group);
    uint64_t block_number =
        volume->first_data_block +
        (uint64_t)group * volume->blocks_per_group;
    return block_number < volume->blocks_count &&
        ext2_write_block(volume, (uint32_t)block_number, block);
}

bool ext2_sync(ext2_volume_t *volume)
{
    if (!volume || !volume->mounted || !volume->writable)
        return false;
    if (!ext2_flush_allocation_cache(volume))
        return false;
    if (!volume->metadata_dirty)
        return true;

    uint8_t superblock[EXT2_SUPER_BYTES];
    if (!ext2_read_super(
            volume->device_index,
            volume->partition_lba,
            volume->partition_sectors,
            superblock))
        return false;
    uint32_t gdt_blocks = divide_round_up(
        volume->group_count * EXT2_GROUP_DESC_SIZE,
        volume->block_size);
    uint8_t block[4096];
    for (uint32_t group = 1; group < volume->group_count; group++)
    {
        if (!ext2_group_has_backup(group))
            continue;
        uint64_t group_first = volume->first_data_block +
            (uint64_t)group * volume->blocks_per_group;
        for (uint32_t index = 0; index < gdt_blocks; index++)
        {
            uint64_t backup_block = group_first + 1 + index;
            if (backup_block >= volume->blocks_count ||
                !ext2_read_block(
                    volume, volume->gdt_start + index, block) ||
                !ext2_write_block(
                    volume, (uint32_t)backup_block, block))
                return false;
        }
        if (!ext2_write_superblock_copy(volume, superblock, group))
            return false;
    }
    volume->metadata_dirty = false;
    return true;
}

bool ext2_format(
    uint32_t device_index,
    uint64_t partition_lba,
    uint64_t partition_sectors,
    const char *volume_name
)
{
    const block_device_t *device = block_device_get(device_index);
    if (!device || !device->writable || !partition_sectors ||
        partition_sectors > UINT64_MAX / BLOCK_SECTOR_SIZE)
        return false;
    const uint32_t block_size = 4096;
    uint64_t partition_bytes =
        partition_sectors * BLOCK_SECTOR_SIZE;
    uint64_t blocks_64 = partition_bytes / block_size;
    if (blocks_64 > UINT32_MAX || blocks_64 < 64)
        return false;

    const uint32_t first_data_block = 0;
    const uint32_t blocks_per_group = 8192;
    const uint32_t inodes_per_group = 1024;
    uint32_t blocks_count = (uint32_t)blocks_64;
    uint32_t group_count = divide_round_up(
        blocks_count - first_data_block,
        blocks_per_group);
    if (!group_count ||
        group_count > UINT32_MAX / inodes_per_group)
        return false;
    uint32_t inodes_count = group_count * inodes_per_group;
    uint32_t inode_size = EXT2_INODE_MIN_SIZE;
    uint32_t inode_table_blocks =
        inodes_per_group * inode_size / block_size;
    uint32_t gdt_blocks = divide_round_up(
        group_count * EXT2_GROUP_DESC_SIZE,
        block_size);
    ext2_volume_t volume = {
        .device_index = device_index,
        .partition_lba = partition_lba,
        .partition_sectors = partition_sectors,
        .block_size = block_size,
        .blocks_per_group = blocks_per_group,
        .inodes_per_group = inodes_per_group,
        .inode_size = inode_size,
        .first_data_block = first_data_block,
        .group_count = group_count,
        .gdt_start = first_data_block + 1,
        .blocks_count = blocks_count,
        .inodes_count = inodes_count,
        .feature_incompat = EXT2_FEATURE_INCOMPAT_FILETYPE,
        .feature_ro_compat = 0x0003U,
        .mounted = true,
        .writable = true
    };

    uint8_t superblock[EXT2_SUPER_BYTES];
    bytes_zero(superblock, sizeof(superblock));
    write_le32(superblock, inodes_count);
    write_le32(superblock + 4, blocks_count);
    write_le32(superblock + 20, first_data_block);
    write_le32(superblock + 24, 2);
    write_le32(superblock + 28, 2);
    write_le32(superblock + 32, blocks_per_group);
    write_le32(superblock + 36, blocks_per_group);
    write_le32(superblock + 40, inodes_per_group);
    write_le16(superblock + 52, 0);
    write_le16(superblock + 54, UINT16_MAX);
    write_le16(superblock + 56, EXT2_SUPER_MAGIC);
    write_le16(superblock + 58, 1);
    write_le16(superblock + 60, 1);
    write_le16(superblock + 62, 0);
    write_le32(superblock + 64, 0);
    write_le32(superblock + 68, 0);
    write_le32(superblock + 72, 0);
    write_le32(superblock + 76, 1);
    write_le16(superblock + 80, 0);
    write_le16(superblock + 82, 0);
    write_le32(superblock + 84, EXT2_FIRST_NON_RESERVED_INODE);
    write_le16(superblock + 88, (uint16_t)inode_size);
    write_le16(superblock + 90, 0);
    write_le32(superblock + 92, 0);
    write_le32(superblock + 96, EXT2_FEATURE_INCOMPAT_FILETYPE);
    write_le32(superblock + 100, 0x0003U);
    if (volume_name)
    {
        for (uint32_t i = 0; i < 16 && volume_name[i]; i++)
            superblock[120 + i] = (uint8_t)volume_name[i];
        for (uint32_t i = 0; i < 16 && volume_name[i]; i++)
            volume.volume_name[i] = volume_name[i];
    }

    uint64_t metadata_blocks_total = 0;
    uint64_t used_inodes_total = 10;
    uint8_t zero_block[4096];
    uint8_t bitmap[4096];
    uint8_t descriptor[EXT2_GROUP_DESC_SIZE];
    uint8_t gdt_block[4096];
    bytes_zero(zero_block, block_size);
    for (uint32_t i = 0; i < gdt_blocks; i++)
    {
        if (!ext2_write_block(&volume, volume.gdt_start + i, zero_block))
            return false;
    }
    for (uint32_t group = 0; group < group_count; group++)
    {
        uint32_t group_first =
            first_data_block + group * blocks_per_group;
        uint32_t valid_blocks = blocks_count - group_first;
        if (valid_blocks > blocks_per_group)
            valid_blocks = blocks_per_group;
        bool backup = ext2_group_has_backup(group);
        uint32_t prefix = backup ? 1 + gdt_blocks : 0;
        uint32_t metadata_blocks = prefix + 2 + inode_table_blocks;
        if (metadata_blocks + (group == 0 ? 1U : 0U) >= valid_blocks)
            return false;
        uint32_t block_bitmap_block = group_first + prefix;
        uint32_t inode_bitmap_block = block_bitmap_block + 1;
        uint32_t inode_table_block = inode_bitmap_block + 1;
        uint32_t root_block = group_first + metadata_blocks;

        bytes_zero(bitmap, block_size);
        for (uint32_t bit = 0; bit < valid_blocks; bit++)
        {
            if (bit < metadata_blocks ||
                (group == 0 && bit == metadata_blocks) ||
                bit >= blocks_per_group)
                bitmap[bit >> 3] |= (uint8_t)(1U << (bit & 7));
        }
        for (uint32_t bit = valid_blocks; bit < block_size * 8; bit++)
            bitmap[bit >> 3] |= (uint8_t)(1U << (bit & 7));
        if (!ext2_write_block(
                &volume, block_bitmap_block, bitmap))
            return false;

        bytes_zero(bitmap, block_size);
        if (group == 0)
        {
            for (uint32_t bit = 0; bit < 10; bit++)
                bitmap[bit >> 3] |= (uint8_t)(1U << (bit & 7));
        }
        for (uint32_t bit = inodes_per_group;
             bit < block_size * 8;
             bit++)
            bitmap[bit >> 3] |= (uint8_t)(1U << (bit & 7));
        if (!ext2_write_block(
                &volume, inode_bitmap_block, bitmap))
            return false;
        for (uint32_t i = 0; i < inode_table_blocks; i++)
        {
            if (!ext2_write_block(
                    &volume, inode_table_block + i, zero_block))
                return false;
        }
        if (group == 0)
        {
            bytes_zero(zero_block, block_size);
            write_le32(zero_block, EXT2_ROOT_INODE);
            write_le16(zero_block + 4, 12);
            zero_block[6] = 1;
            zero_block[7] = EXT2_DIR_TYPE_DIRECTORY;
            zero_block[8] = '.';
            write_le32(zero_block + 12, EXT2_ROOT_INODE);
            write_le16(
                zero_block + 16,
                (uint16_t)(block_size - 12));
            zero_block[18] = 2;
            zero_block[19] = EXT2_DIR_TYPE_DIRECTORY;
            zero_block[20] = '.';
            zero_block[21] = '.';
            if (!ext2_write_block(&volume, root_block, zero_block))
                return false;
        }

        bytes_zero(descriptor, sizeof(descriptor));
        write_le32(descriptor, block_bitmap_block);
        write_le32(descriptor + 4, inode_bitmap_block);
        write_le32(descriptor + 8, inode_table_block);
        uint32_t used_blocks = metadata_blocks + (group == 0 ? 1 : 0);
        write_le16(
            descriptor + 12,
            (uint16_t)(valid_blocks - used_blocks));
        write_le16(
            descriptor + 14,
            (uint16_t)(inodes_per_group - (group == 0 ? 10 : 0)));
        write_le16(descriptor + 16, group == 0 ? 1 : 0);
        uint32_t byte_offset = group * EXT2_GROUP_DESC_SIZE;
        uint32_t desc_block_number =
            volume.gdt_start + byte_offset / block_size;
        uint32_t desc_offset = byte_offset % block_size;
        if (!ext2_read_block(
                &volume, desc_block_number, gdt_block))
        {
            bytes_zero(gdt_block, block_size);
        }
        bytes_copy(
            gdt_block + desc_offset,
            descriptor,
            EXT2_GROUP_DESC_SIZE);
        if (!ext2_write_block(
                &volume, desc_block_number, gdt_block))
            return false;

        metadata_blocks_total += used_blocks;
        if (group == 0)
            bytes_zero(zero_block, block_size);
    }

    uint64_t free_blocks = blocks_count - first_data_block -
        metadata_blocks_total;
    uint64_t free_inodes = inodes_count - used_inodes_total;
    if (free_blocks > UINT32_MAX || free_inodes > UINT32_MAX)
        return false;
    write_le32(superblock + 12, (uint32_t)free_blocks);
    write_le32(superblock + 16, (uint32_t)free_inodes);

    uint8_t root_inode[EXT2_INODE_MIN_SIZE];
    bytes_zero(root_inode, sizeof(root_inode));
    write_le16(root_inode, EXT2_MODE_DIRECTORY | 0755);
    write_le32(root_inode + 4, block_size);
    write_le32(root_inode + 20, 0);
    write_le32(root_inode + 28, block_size / 512);
    write_le16(root_inode + 26, 2);
    write_le32(
        root_inode + 40,
        first_data_block + 1 + gdt_blocks + 2 + inode_table_blocks);
    uint32_t root_inode_table =
        first_data_block + 1 + gdt_blocks + 2;
    uint8_t inode_table_data[4096];
    if (!ext2_read_block(&volume, root_inode_table, inode_table_data))
        return false;
    bytes_copy(
        inode_table_data + (EXT2_ROOT_INODE - 1) * inode_size,
        root_inode,
        inode_size);
    if (!ext2_write_block(
            &volume, root_inode_table, inode_table_data))
        return false;

    uint8_t table_copy[4096];
    for (uint32_t group = 0; group < group_count; group++)
    {
        if (!ext2_group_has_backup(group))
            continue;
        uint64_t group_first =
            first_data_block + (uint64_t)group * blocks_per_group;
        if (group_first >= blocks_count)
            continue;
        if (!ext2_write_superblock_copy(&volume, superblock, group))
            return false;
        for (uint32_t i = 0; i < gdt_blocks; i++)
        {
            if (!ext2_read_block(
                    &volume, volume.gdt_start + i, table_copy) ||
                !ext2_write_block(
                    &volume, (uint32_t)group_first + 1 + i,
                    table_copy))
                return false;
        }
    }
    if (!ext2_write_super(&volume, superblock))
        return false;
    return true;
}
