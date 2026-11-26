#include <stdbool.h>
#include <stdint.h>

#include "devfs.h"
#include "block.h"
#include "keyboard.h"
#include "mbr.h"
#include "mouse.h"
#include "pci.h"
#include "radeonhd.h"
#include "syscall.h"
#include "vesa.h"
#include "vfs.h"
#include "serial.h"

static iso9660_sector_reader_t cdrom_reader;
static void *cdrom_context;
static uint32_t cdrom_sector_count;

void devfs_register_cdrom(
    iso9660_sector_reader_t reader,
    void *context,
    uint32_t sector_count
)
{
    cdrom_reader = reader;
    cdrom_context = context;
    cdrom_sector_count = sector_count;
}

static bool string_equal(const char *left, const char *right)
{
    while (*left && *right && *left == *right)
    {
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static bool string_starts_with(const char *text, const char *prefix)
{
    while (*prefix)
    {
        if (*text++ != *prefix++)
            return false;
    }
    return true;
}

static size_t string_length(const char *text)
{
    size_t length = 0;
    while (text[length])
        length++;
    return length;
}

static const pci_device_t *class_device_get(bool audio, uint32_t index)
{
    if (!audio)
        return pci_class_device_get(0x03, 0xFF, index);

    uint32_t audio_function_count = pci_class_device_count(0x04, 0x01);
    if (index < audio_function_count)
        return pci_class_device_get(0x04, 0x01, index);
    return pci_class_device_get(0x04, 0x03, index - audio_function_count);
}

static uint32_t class_device_count(bool audio)
{
    if (!audio)
        return pci_class_device_count(0x03, 0xFF);
    return pci_class_device_count(0x04, 0x01) +
        pci_class_device_count(0x04, 0x03);
}

static const pci_device_t *device_for_metadata_path(const char *path)
{
    static const char pci_prefix[] = "/dev/pci/";
    static const char dri_prefix[] = "/dev/dri/card";
    static const char snd_prefix[] = "/dev/snd/card";

    if (string_starts_with(path, pci_prefix))
        return pci_find_bdf(path + sizeof(pci_prefix) - 1);

    bool audio;
    const char *number;
    if (string_starts_with(path, dri_prefix))
    {
        audio = false;
        number = path + sizeof(dri_prefix) - 1;
    }
    else if (string_starts_with(path, snd_prefix))
    {
        audio = true;
        number = path + sizeof(snd_prefix) - 1;
    }
    else
    {
        return NULL;
    }

    if (!*number)
        return NULL;
    uint32_t index = 0;
    while (*number >= '0' && *number <= '9')
    {
        uint32_t digit = (uint32_t)(*number++ - '0');
        if (index > (UINT32_MAX - digit) / 10)
            return NULL;
        index = index * 10 + digit;
    }
    if (*number)
        return NULL;
    return class_device_get(audio, index);
}

static size_t format_metadata_path(
    const char *path,
    char *buffer,
    size_t capacity
)
{
    const pci_device_t *device = device_for_metadata_path(path);
    if (!device)
        return 0;
    if (string_starts_with(path, "/dev/pci/"))
        return pci_format_device(device, buffer, capacity);

    size_t position = 0;
    const char *label = string_starts_with(path, "/dev/snd/")
        ? "Sound card metadata\n"
        : "Display card metadata\n";
    while (*label)
    {
        if (position == capacity)
            return 0;
        buffer[position++] = *label++;
    }
    size_t remaining = pci_format_device(
        device, buffer + position, capacity - position);
    if (!remaining)
        return 0;
    position += remaining;
    if (string_starts_with(path, "/dev/dri/"))
    {
        size_t driver_length = radeonhd_format_attribute(
            device, "driver", buffer + position, capacity - position);
        if (!driver_length)
            return 0;
        position += driver_length;
        size_t generation_length = radeonhd_format_attribute(
            device, "generation", buffer + position, capacity - position);
        if (!generation_length)
            return 0;
        position += generation_length;
        size_t status_length = radeonhd_format_attribute(
            device, "status", buffer + position, capacity - position);
        if (!status_length)
            return 0;
        position += status_length;
        size_t debug_length = radeonhd_format_debug(
            device, buffer + position, capacity - position);
        if (!debug_length)
            return 0;
        position += debug_length;
    }
    return position;
}

bool devfs_is_directory(const char *path)
{
    return path &&
        (string_equal(path, "/dev") ||
         string_equal(path, "/dev/disk") ||
         string_equal(path, "/dev/input") ||
         string_equal(path, "/dev/pts") ||
         string_equal(path, "/dev/pci") ||
         string_equal(path, "/dev/dri") ||
         string_equal(path, "/dev/snd"));
}

static bool block_path_resolve(
    const char *path,
    uint32_t *device_index,
    uint64_t *start_lba,
    uint64_t *sector_count
)
{
    static const char prefix[] = "/dev/disk/";
    if (!path || !string_starts_with(path, prefix))
        return false;
    const char *name = path + sizeof(prefix) - 1;
    if (!*name)
        return false;

    for (uint32_t i = 0; i < block_device_count(); i++)
    {
        const block_device_t *device = block_device_get(i);
        if (!device)
            continue;
        if (string_equal(name, device->name))
        {
            *device_index = i;
            *start_lba = 0;
            *sector_count = device->sector_count;
            return true;
        }

        size_t name_length = 0;
        while (device->name[name_length])
            name_length++;
        size_t position = 0;
        while (position < name_length &&
            name[position] == device->name[position])
            position++;
        if (position != name_length || name[position] != 'p')
            continue;

        const char *digits = name + position + 1;
        if (*digits < '0' || *digits > '9')
            continue;
        uint32_t partition_number = 0;
        bool valid_number = true;
        while (*digits >= '0' && *digits <= '9')
        {
            uint32_t digit = (uint32_t)(*digits++ - '0');
            if (partition_number > (UINT32_MAX - digit) / 10)
            {
                valid_number = false;
                break;
            }
            partition_number = partition_number * 10 + digit;
        }
        if (!valid_number || *digits)
            continue;

        for (uint32_t partition_index = 0;
             partition_index < mbr_partition_count();
             partition_index++)
        {
            const mbr_partition_t *partition =
                mbr_partition_get(partition_index);
            if (partition &&
                partition->device_index == i &&
                partition->number == partition_number)
            {
                *device_index = i;
                *start_lba = partition->start_lba;
                *sector_count = partition->sector_count;
                return true;
            }
        }
    }
    return false;
}

bool devfs_file_size(const char *path, uint64_t *size)
{
    if (!path || !size)
        return false;

    uint32_t device_index;
    uint64_t start_lba;
    uint64_t sector_count;
    if (block_path_resolve(
            path, &device_index, &start_lba, &sector_count) &&
        sector_count <= UINT64_MAX / BLOCK_SECTOR_SIZE)
    {
        *size = sector_count * BLOCK_SECTOR_SIZE;
        return true;
    }

    if (string_starts_with(path, "/dev/pci/") ||
        string_starts_with(path, "/dev/dri/card") ||
        string_starts_with(path, "/dev/snd/card"))
    {
        char metadata[512];
        size_t length = format_metadata_path(
            path, metadata, sizeof(metadata));
        if (!length)
            return false;
        *size = length;
        return true;
    }

    if (string_equal(path, "/dev/console") ||
        string_equal(path, "/dev/keyboard"))
    {
        *size = UINT64_MAX;
        return true;
    }

    if (mouse_is_initialized() &&
        (string_equal(path, "/dev/mouse") ||
         string_equal(path, "/dev/input/mouse0")))
    {
        *size = UINT64_MAX;
        return true;
    }

    if (string_equal(path, "/dev/null") ||
        string_equal(path, "/dev/zero"))
    {
        *size = UINT64_MAX;
        return true;
    }

    if (string_equal(path, "/dev/fb0") && vesa_get_width())
    {
        *size = 0;
        return true;
    }

    if (string_equal(path, "/dev/sr0") &&
        cdrom_reader && cdrom_sector_count)
    {
        *size = (uint64_t)cdrom_sector_count * ISO9660_SECTOR_SIZE;
        return true;
    }

    return false;
}

int64_t devfs_read(
    const char *path,
    uint64_t offset,
    void *buffer,
    uint64_t size
)
{
    if (!path || (!buffer && size))
        return -SYSCALL_EINVAL;

    uint32_t device_index;
    uint64_t start_lba;
    uint64_t sector_count;
    if (block_path_resolve(
            path, &device_index, &start_lba, &sector_count))
    {
        if (sector_count > UINT64_MAX / BLOCK_SECTOR_SIZE ||
            start_lba > UINT64_MAX - sector_count)
            return -SYSCALL_EIO;
        uint64_t total_size = sector_count * BLOCK_SECTOR_SIZE;
        if (offset >= total_size)
            return 0;
        if (size > total_size - offset)
            size = total_size - offset;

        uint8_t sector[BLOCK_SECTOR_SIZE];
        uint64_t total = 0;
        while (total < size)
        {
            uint64_t absolute_offset = offset + total;
            uint64_t lba = start_lba +
                absolute_offset / BLOCK_SECTOR_SIZE;
            uint32_t sector_offset =
                (uint32_t)(absolute_offset % BLOCK_SECTOR_SIZE);
            if (!block_read(device_index, lba, 1, sector))
                return total ? (int64_t)total : -SYSCALL_EIO;
            uint64_t amount =
                BLOCK_SECTOR_SIZE - sector_offset;
            if (amount > size - total)
                amount = size - total;
            for (uint64_t i = 0; i < amount; i++)
                ((uint8_t *)buffer)[total + i] =
                    sector[sector_offset + i];
            total += amount;
        }
        return (int64_t)total;
    }

    if (string_equal(path, "/dev/null"))
        return 0;

    if (string_equal(path, "/dev/zero"))
    {
        for (uint64_t i = 0; i < size; i++)
            ((uint8_t *)buffer)[i] = 0;
        return (int64_t)size;
    }

    if (string_equal(path, "/dev/console") ||
        string_equal(path, "/dev/keyboard"))
    {
        uint64_t count = 0;
        while (count < size && keyboard_has_input())
            ((char *)buffer)[count++] = keyboard_getchar();
        return count ? (int64_t)count : -SYSCALL_EAGAIN;
    }

    if (string_equal(path, "/dev/mouse") ||
        string_equal(path, "/dev/input/mouse0"))
        return mouse_read_events(buffer, size);

    if (string_equal(path, "/dev/fb0"))
        return 0;

    if (string_starts_with(path, "/dev/pci/") ||
        string_starts_with(path, "/dev/dri/card") ||
        string_starts_with(path, "/dev/snd/card"))
    {
        char metadata[512];
        size_t length = format_metadata_path(
            path, metadata, sizeof(metadata));
        if (!length)
            return -SYSCALL_ENODEV;
        if (offset >= length)
            return 0;
        if (size > length - offset)
            size = length - offset;
        for (uint64_t i = 0; i < size; i++)
            ((char *)buffer)[i] = metadata[offset + i];
        return (int64_t)size;
    }

    if (string_equal(path, "/dev/sr0"))
    {
        if (!cdrom_reader || !cdrom_sector_count)
            return -SYSCALL_ENODEV;

        uint8_t sector[ISO9660_SECTOR_SIZE];
        uint64_t total = 0;
        while (total < size)
        {
            uint64_t absolute_offset = offset + total;
            uint32_t lba =
                (uint32_t)(absolute_offset / ISO9660_SECTOR_SIZE);
            uint32_t sector_offset =
                (uint32_t)(absolute_offset % ISO9660_SECTOR_SIZE);
            if (lba >= cdrom_sector_count ||
                !cdrom_reader(cdrom_context, lba, sector))
                return total ? (int64_t)total : -SYSCALL_EIO;

            uint64_t amount =
                ISO9660_SECTOR_SIZE - sector_offset;
            if (amount > size - total)
                amount = size - total;
            for (uint64_t i = 0; i < amount; i++)
                ((uint8_t *)buffer)[total + i] =
                    sector[sector_offset + i];
            total += amount;
        }
        return (int64_t)total;
    }

    return -SYSCALL_ENODEV;
}

static size_t append_decimal(char *buffer, size_t capacity, uint32_t value)
{
    char digits[10];
    size_t count = 0;
    do
    {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value && count < sizeof(digits));
    if (count + 1 > capacity)
        return 0;

    size_t length = 0;
    while (count)
        buffer[length++] = digits[--count];
    buffer[length] = '\0';
    return length;
}

int64_t devfs_list(
    const char *path,
    uint64_t offset,
    vfs_dirent_t *entries,
    uint64_t capacity
)
{
    if (!path || (capacity && !entries))
        return -SYSCALL_EINVAL;

    bool pci_directory = string_equal(path, "/dev/pci");
    bool display_directory = string_equal(path, "/dev/dri");
    bool audio_directory = string_equal(path, "/dev/snd");
    if (!pci_directory && !display_directory && !audio_directory)
        return -SYSCALL_ENOENT;

    uint64_t total = pci_directory
        ? pci_device_count()
        : class_device_count(audio_directory);
    if (offset >= total)
        return 0;
    uint64_t returned = total - offset;
    if (returned > capacity)
        returned = capacity;

    for (uint64_t i = 0; i < returned; i++)
    {
        char name[sizeof(entries[i].name)];
        if (pci_directory)
        {
            const pci_device_t *device =
                pci_device_get((uint32_t)(offset + i));
            if (!device || !pci_format_bdf(
                    device, name, sizeof(name)))
                return -SYSCALL_EIO;
        }
        else
        {
            name[0] = 'c';
            name[1] = 'a';
            name[2] = 'r';
            name[3] = 'd';
            if (!append_decimal(
                    name + 4, sizeof(name) - 4, (uint32_t)(offset + i)))
                return -SYSCALL_EIO;
        }

        size_t length = string_length(name);
        for (size_t j = 0; j <= length; j++)
            entries[i].name[j] = name[j];
        const pci_device_t *device = pci_directory
            ? pci_device_get((uint32_t)(offset + i))
            : class_device_get(audio_directory, (uint32_t)(offset + i));
        if (!device)
            return -SYSCALL_EIO;
        char metadata[512];
        size_t metadata_length = pci_format_device(
            device, metadata, sizeof(metadata));
        entries[i].size = metadata_length;
        entries[i].is_directory = false;
    }
    return (int64_t)returned;
}

int64_t devfs_write(
    const char *path,
    uint64_t offset,
    const void *buffer,
    uint64_t size
)
{
    if (!path || (!buffer && size))
        return -SYSCALL_EINVAL;

    uint32_t device_index;
    uint64_t start_lba;
    uint64_t sector_count;
    if (block_path_resolve(
            path, &device_index, &start_lba, &sector_count))
    {
        const block_device_t *device =
            block_device_get(device_index);
        if (!device || !device->writable)
            return -SYSCALL_EROFS;
        if (sector_count > UINT64_MAX / BLOCK_SECTOR_SIZE ||
            start_lba > UINT64_MAX - sector_count)
            return -SYSCALL_EIO;
        uint64_t total_size = sector_count * BLOCK_SECTOR_SIZE;
        if (offset >= total_size)
            return 0;
        if (size > total_size - offset)
            size = total_size - offset;

        uint8_t sector[BLOCK_SECTOR_SIZE];
        uint64_t total = 0;
        while (total < size)
        {
            uint64_t absolute_offset = offset + total;
            uint64_t lba = start_lba +
                absolute_offset / BLOCK_SECTOR_SIZE;
            uint32_t sector_offset =
                (uint32_t)(absolute_offset % BLOCK_SECTOR_SIZE);
            uint64_t amount =
                BLOCK_SECTOR_SIZE - sector_offset;
            if (amount > size - total)
                amount = size - total;
            if ((sector_offset || amount != BLOCK_SECTOR_SIZE) &&
                !block_read(device_index, lba, 1, sector))
                return total ? (int64_t)total : -SYSCALL_EIO;
            for (uint64_t i = 0; i < amount; i++)
                sector[sector_offset + i] =
                    ((const uint8_t *)buffer)[total + i];
            if (!block_write(device_index, lba, 1, sector))
                return total ? (int64_t)total : -SYSCALL_EIO;
            total += amount;
        }
        return (int64_t)total;
    }

    if (string_equal(path, "/dev/null") ||
        string_equal(path, "/dev/zero"))
        return (int64_t)size;

    if (string_equal(path, "/dev/sr0"))
        return -SYSCALL_EROFS;

    if (string_equal(path, "/dev/console"))
    {
        serial_console_write(buffer, size);
        return (int64_t)size;
    }

    return -SYSCALL_EBADF;
}
