#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "block.h"
#include "ext2.h"
#include "fat32.h"
#include "installer.h"
#include "mbr.h"
#include "serial.h"
#include "syscall.h"
#include "vfs.h"
#include "../../limine/limine-bios-hdd.h"

#define INSTALL_BIOS_START 2048ULL
#define INSTALL_BIOS_SECTORS 32768ULL
#define INSTALL_EFI_SECTORS 262144ULL
#define INSTALL_MIN_ROOT_SECTORS 131072ULL
#define INSTALL_SECTOR_ALIGNMENT 2048ULL
#define INSTALL_COPY_BUFFER_SIZE 8192
#define INSTALL_ROOT_MOUNT "/mnt/installer-root"
#define INSTALL_EFI_MOUNT "/mnt/installer-efi"

static uint8_t install_copy_buffer[INSTALL_COPY_BUFFER_SIZE];
static const char *installer_failure_message;

static const uint8_t gpt_bios_boot_guid[16] = {
    0x48, 0x61, 0x68, 0x21, 0x49, 0x6E, 0x6F, 0x74,
    0x4E, 0x65, 0x65, 0x64, 0x45, 0x46, 0x49, 0x20
};

static const uint8_t gpt_efi_system_guid[16] = {
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
};

static const uint8_t gpt_linux_filesystem_guid[16] = {
    0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
    0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4
};

static const char installed_limine_config[] =
    "timeout: 5\n"
    "default_entry: 1\n"
    "\n"
    "/VeRsitileOS\n"
    "protocol: limine\n"
    "path: boot():/boot/kernel.elf\n"
    "cmdline: root=VeRsitileOS\n";

static size_t string_length(const char *text, size_t limit)
{
    size_t length = 0;
    while (length < limit && text[length])
        length++;
    return length;
}

static void memory_copy(void *destination, const void *source, size_t size)
{
    uint8_t *output = destination;
    const uint8_t *input = source;
    for (size_t i = 0; i < size; i++)
        output[i] = input[i];
}

static void memory_move(void *destination, const void *source, size_t size)
{
    uint8_t *output = destination;
    const uint8_t *input = source;
    if (output < input)
    {
        for (size_t i = 0; i < size; i++)
            output[i] = input[i];
    }
    else if (output > input)
    {
        while (size)
        {
            size--;
            output[size] = input[size];
        }
    }
}

static bool string_equal(const char *left, const char *right)
{
    while (*left && *right)
    {
        if (*left++ != *right++)
            return false;
    }
    return *left == '\0' && *right == '\0';
}

static bool string_copy(char *destination, size_t capacity, const char *source)
{
    if (!destination || !capacity || !source)
        return false;
    size_t length = string_length(source, capacity);
    if (length == capacity)
        return false;
    for (size_t i = 0; i <= length; i++)
        destination[i] = source[i];
    return true;
}

static int device_by_name(const char *name)
{
    for (uint32_t i = 0; i < block_device_count(); i++)
    {
        const block_device_t *device = block_device_get(i);
        if (device && string_equal(device->name, name))
            return (int)i;
    }
    return -1;
}

static bool confirmation_matches(
    const os_install_request_t *request,
    const char *device_name
)
{
    static const char prefix[] = "WIPE ";
    size_t out = 0;
    for (size_t i = 0; i < sizeof(prefix) - 1; i++)
    {
        if (request->confirmation[out++] != prefix[i])
            return false;
    }
    for (size_t i = 0; device_name[i]; i++)
    {
        if (request->confirmation[out] != device_name[i])
            return false;
        out++;
    }
    return out < sizeof(request->confirmation) &&
        request->confirmation[out] == '\0';
}

static bool source_file_exists(const char *path)
{
    vfs_file_t file;
    if (vfs_open(path, &file) < 0)
        return false;
    vfs_close_file(&file);
    return true;
}

static bool path_join(
    char *destination,
    size_t capacity,
    const char *parent,
    const char *name
)
{
    size_t parent_length = string_length(parent, capacity);
    size_t name_length = string_length(name, capacity);
    if (!parent_length || parent_length >= capacity ||
        !name_length || name_length >= capacity)
        return false;
    size_t separator = string_equal(parent, "/") ? 0 : 1;
    if (parent_length + separator + name_length >= capacity)
        return false;
    size_t out = 0;
    for (size_t i = 0; i < parent_length; i++)
        destination[out++] = parent[i];
    if (separator)
        destination[out++] = '/';
    for (size_t i = 0; i < name_length; i++)
        destination[out++] = name[i];
    destination[out] = '\0';
    return true;
}

static void report_install_operation_failure(
    const char *action,
    const char *path,
    int64_t error,
    const char *display_message
)
{
    if (!installer_failure_message)
        installer_failure_message = display_message;
    serial_log_begin(SERIAL_LOG_ERROR);
    serial_write("Installer: ");
    serial_write(action);
    serial_write(" ");
    serial_write(path);
    serial_write(" (error ");
    serial_write_dec(error < 0 ? (uint64_t)-error : (uint64_t)error);
    serial_write(")");
    serial_log_end();
}

static bool copy_file(
    const char *source_path,
    const char *destination_path,
    const char *display_message
)
{
    vfs_file_t source;
    vfs_file_t destination;
    int64_t result = vfs_open(source_path, &source);
    if (result < 0)
    {
        report_install_operation_failure(
            "could not open source file",
            source_path,
            result,
            display_message);
        return false;
    }
    result = vfs_open_write(destination_path, true, true, &destination);
    if (result < 0)
    {
        vfs_close_file(&source);
        report_install_operation_failure(
            "could not create destination file",
            destination_path,
            result,
            display_message);
        return false;
    }
    for (;;)
    {
        int64_t count = vfs_read(
            &source, install_copy_buffer, sizeof(install_copy_buffer));
        if (count < 0)
        {
            report_install_operation_failure(
                "could not read source file",
                source_path,
                count,
                display_message);
            vfs_close_file(&destination);
            vfs_close_file(&source);
            return false;
        }
        if (!count)
            break;
        uint64_t written = 0;
        while (written < (uint64_t)count)
        {
            int64_t amount = vfs_write(
                &destination,
                install_copy_buffer + written,
                (uint64_t)count - written);
            if (amount <= 0)
            {
                report_install_operation_failure(
                    "could not write destination file",
                    destination_path,
                    amount,
                    display_message);
                vfs_close_file(&destination);
                vfs_close_file(&source);
                return false;
            }
            written += (uint64_t)amount;
        }
    }
    vfs_close_file(&destination);
    vfs_close_file(&source);
    return true;
}

static bool install_directory(const char *path, const char *display_message)
{
    if (vfs_is_directory(path))
        return true;
    int64_t result = vfs_mkdir(path);
    if (result < 0)
    {
        report_install_operation_failure(
            "could not create directory",
            path,
            result,
            display_message);
        return false;
    }
    return true;
}

static bool write_file(
    const char *path,
    const void *bytes,
    uint64_t size,
    const char *display_message
)
{
    vfs_file_t file;
    int64_t result = vfs_open_write(path, true, true, &file);
    if (result < 0)
    {
        report_install_operation_failure(
            "could not create file",
            path,
            result,
            display_message);
        return false;
    }
    uint64_t written = 0;
    while (written < size)
    {
        int64_t amount = vfs_write(
            &file,
            (const uint8_t *)bytes + written,
            size - written);
        if (amount <= 0)
        {
            vfs_close_file(&file);
            report_install_operation_failure(
                "could not write file",
                path,
                amount,
                display_message);
            return false;
        }
        written += (uint64_t)amount;
    }
    vfs_close_file(&file);
    return true;
}

static void log_install_path_error(
    const char *action,
    const char *path,
    uint64_t offset
)
{
    if (!installer_failure_message)
        installer_failure_message = action;
    serial_log_begin(SERIAL_LOG_ERROR);
    serial_write("Installer: ");
    serial_write(action);
    serial_write(" ");
    serial_write(path);
    serial_write(" at offset ");
    serial_write_dec(offset);
    serial_log_end();
}

static bool make_directory(const char *path)
{
    if (vfs_is_directory(path))
        return true;
    return vfs_mkdir(path) == 0;
}

static uint64_t tar_octal_value(const uint8_t *field, size_t length)
{
    uint64_t value = 0;
    size_t i = 0;
    while (i < length && (field[i] == ' ' || field[i] == '\0'))
        i++;
    for (; i < length && field[i] >= '0' && field[i] <= '7'; i++)
    {
        uint8_t digit = (uint8_t)(field[i] - '0');
        if (value > (UINT64_MAX - digit) / 8)
            return UINT64_MAX;
        value = value * 8 + digit;
    }
    return value;
}

static bool archive_read_exact(
    vfs_file_t *archive,
    void *buffer,
    uint64_t size
)
{
    uint8_t *output = buffer;
    uint64_t read_count = 0;
    while (read_count < size)
    {
        int64_t amount = vfs_read(
            archive,
            output + read_count,
            size - read_count);
        if (amount <= 0)
            return false;
        read_count += (uint64_t)amount;
    }
    return true;
}

static bool archive_skip(vfs_file_t *archive, uint64_t size)
{
    while (size)
    {
        uint64_t chunk = size < sizeof(install_copy_buffer)
            ? size
            : sizeof(install_copy_buffer);
        if (!archive_read_exact(archive, install_copy_buffer, chunk))
            return false;
        size -= chunk;
    }
    return true;
}

static bool archive_entry_path(
    const uint8_t header[512],
    char relative_path[256]
)
{
    size_t name_length = 0;
    while (name_length < 100 && header[name_length])
        name_length++;
    size_t prefix_length = 0;
    while (prefix_length < 155 && header[345 + prefix_length])
        prefix_length++;
    if (!name_length)
        return false;

    size_t length = 0;
    if (prefix_length)
    {
        if (prefix_length + 1 + name_length >= 256)
            return false;
        memory_copy(relative_path, header + 345, prefix_length);
        length = prefix_length;
        relative_path[length++] = '/';
    }
    else if (name_length >= 256)
        return false;
    memory_copy(relative_path + length, header, name_length);
    length += name_length;
    relative_path[length] = '\0';

    if (relative_path[0] == '/')
        return false;
    while (relative_path[0] == '.' && relative_path[1] == '/')
    {
        size_t remaining = string_length(relative_path + 2, 256);
        if (remaining == 256)
            return false;
        memory_move(relative_path, relative_path + 2, remaining + 1);
    }
    length = string_length(relative_path, 256);
    if (length == 256)
        return false;
    while (length && relative_path[length - 1] == '/')
        relative_path[--length] = '\0';
    if (!length || length >= 256)
        return length == 0;

    size_t component_start = 0;
    for (size_t i = 0; i <= length; i++)
    {
        if (relative_path[i] != '/' && relative_path[i] != '\0')
            continue;
        size_t component_length = i - component_start;
        if (!component_length ||
            (component_length == 1 &&
             relative_path[component_start] == '.') ||
            (component_length == 2 &&
             relative_path[component_start] == '.' &&
             relative_path[component_start + 1] == '.'))
            return false;
        component_start = i + 1;
    }
    return true;
}

static bool ensure_parent_directories(const char *path)
{
    char parent[256];
    size_t length = string_length(path, sizeof(parent));
    if (!length || length >= sizeof(parent))
        return false;
    memory_copy(parent, path, length + 1);
    for (size_t i = 1; i < length; i++)
    {
        if (parent[i] != '/')
            continue;
        parent[i] = '\0';
        if (!make_directory(parent))
            return false;
        parent[i] = '/';
    }
    return true;
}

static bool extract_rootfs_archive(void)
{
    static const char archive_path[] = "/system/install/rootfs.tar";
    static uint8_t file_buffer[64 * 1024];
    vfs_file_t archive;
    int64_t open_result = vfs_open(archive_path, &archive);
    if (open_result < 0)
    {
        report_install_operation_failure(
            "could not open rootfs archive",
            archive_path,
            open_result,
            "Could not open rootfs archive");
        return false;
    }

    uint8_t header[512];
    bool success = false;
    while (archive.offset <= archive.size &&
           archive.size - archive.offset >= sizeof(header))
    {
        if (!archive_read_exact(&archive, header, sizeof(header)))
        {
            log_install_path_error(
                "could not read rootfs archive",
                archive_path,
                archive.offset);
            break;
        }
        bool empty = true;
        for (size_t i = 0; i < sizeof(header); i++)
            empty = empty && header[i] == 0;
        if (empty)
        {
            success = true;
            break;
        }

        uint64_t file_size = tar_octal_value(header + 124, 12);
        uint8_t type = header[156];
        char relative_path[256];
        if (file_size == UINT64_MAX ||
            (type != '\0' && type != '0' && type != '5') ||
            (type == '5' && file_size != 0) ||
            !archive_entry_path(header, relative_path))
        {
            log_install_path_error(
                "found invalid rootfs archive entry in",
                archive_path,
                archive.offset - sizeof(header));
            break;
        }

        uint64_t padding = (512 - (file_size & 511)) & 511;
        if (file_size > archive.size - archive.offset ||
            padding > archive.size - archive.offset - file_size)
        {
            log_install_path_error(
                "found truncated rootfs archive entry in",
                archive_path,
                archive.offset - sizeof(header));
            break;
        }

        if (relative_path[0])
        {
            char destination_path[256];
            if (!path_join(
                    destination_path,
                    sizeof(destination_path),
                    INSTALL_ROOT_MOUNT,
                    relative_path) ||
                !ensure_parent_directories(destination_path))
            {
                log_install_path_error(
                    "could not prepare rootfs entry",
                    relative_path,
                    archive.offset);
                break;
            }

            if (type == '5')
            {
                if (!make_directory(destination_path))
                {
                    log_install_path_error(
                        "could not create rootfs directory",
                        destination_path,
                        archive.offset);
                    break;
                }
            }
            else
            {
                vfs_file_t destination;
                int64_t open_result = vfs_open_write(
                    destination_path, true, true, &destination);
                if (open_result < 0)
                {
                    if (!installer_failure_message)
                        installer_failure_message =
                            "Could not create root filesystem file";
                    serial_log_begin(SERIAL_LOG_ERROR);
                    serial_write("Installer: could not open rootfs file ");
                    serial_write(destination_path);
                    serial_write(" at offset ");
                    serial_write_dec(archive.offset);
                    serial_write(" (error ");
                    serial_write_dec((uint64_t)-open_result);
                    serial_write(")");
                    serial_log_end();
                    break;
                }
                uint64_t remaining = file_size;
                bool file_success = true;
                while (remaining)
                {
                    uint64_t chunk = remaining < sizeof(file_buffer)
                        ? remaining
                        : sizeof(file_buffer);
                    if (!archive_read_exact(&archive, file_buffer, chunk))
                    {
                        log_install_path_error(
                            "could not read payload for",
                            relative_path,
                            archive.offset);
                        file_success = false;
                        break;
                    }
                    uint64_t written = 0;
                    while (written < chunk)
                    {
                        int64_t amount = vfs_write(
                            &destination,
                            file_buffer + written,
                            chunk - written);
                        if (amount <= 0)
                        {
                            log_install_path_error(
                                "could not write rootfs file",
                                destination_path,
                                file_size - remaining + written);
                            file_success = false;
                            break;
                        }
                        written += (uint64_t)amount;
                    }
                    if (!file_success)
                        break;
                    remaining -= chunk;
                    if (archive.size)
                    {
                        uint64_t interval = archive.size / 40;
                        if (archive.size % 40)
                            interval++;
                        if (!interval)
                            interval = 1;
                        uint64_t step = archive.offset / interval;
                        if (step > 40)
                            step = 40;
                        serial_install_progress_update(
                            10 + (uint32_t)step * 2);
                    }
                }
                vfs_close_file(&destination);
                if (!file_success)
                    break;
                if (!archive_skip(&archive, padding))
                {
                    log_install_path_error(
                        "could not skip rootfs archive padding for",
                        relative_path,
                        archive.offset);
                    break;
                }
                continue;
            }
        }

        if (!archive_skip(&archive, file_size + padding))
        {
            log_install_path_error(
                "could not skip rootfs archive entry in",
                archive_path,
                archive.offset);
            break;
        }
    }
    vfs_close_file(&archive);
    if (!success)
        log_install_path_error(
            "rootfs archive extraction did not complete in",
            archive_path,
            archive.offset);
    else
        serial_install_progress_update(90);
    return success;
}

static bool copy_bios_stage(
    uint32_t device_index,
    uint64_t bios_lba,
    uint64_t bios_sectors
)
{
    const size_t image_size = sizeof(binary_limine_hdd_bin_data);
    if (image_size <= BLOCK_SECTOR_SIZE)
        return false;
    uint64_t stage_size = image_size - BLOCK_SECTOR_SIZE;
    uint64_t stage_sectors =
        (stage_size + BLOCK_SECTOR_SIZE - 1) / BLOCK_SECTOR_SIZE;
    if (stage_sectors > bios_sectors)
        return false;

    uint8_t sector[BLOCK_SECTOR_SIZE];
    uint8_t original_mbr[BLOCK_SECTOR_SIZE];
    if (!block_read(device_index, 0, 1, original_mbr))
        return false;
    for (size_t i = 0; i < sizeof(sector); i++)
        sector[i] = binary_limine_hdd_bin_data[i];
    for (uint32_t i = 440; i < 510; i++)
        sector[i] = original_mbr[i];
    for (uint32_t i = 218; i < 224; i++)
        sector[i] = original_mbr[i];
    uint64_t stage2_byte_offset = bios_lba * BLOCK_SECTOR_SIZE;
    for (uint32_t i = 0; i < sizeof(stage2_byte_offset); i++)
        sector[0x1A4 + i] =
            (uint8_t)(stage2_byte_offset >> (i * 8));
    if (!block_write(device_index, 0, 1, sector))
        return false;

    for (uint64_t i = 0; i < stage_sectors; i++)
    {
        for (size_t j = 0; j < sizeof(sector); j++)
        {
            uint64_t image_offset =
                BLOCK_SECTOR_SIZE + i * BLOCK_SECTOR_SIZE + j;
            sector[j] = image_offset < image_size
                ? binary_limine_hdd_bin_data[image_offset]
                : 0;
        }
        if (!block_write(
                device_index,
                bios_lba + i,
                1,
                sector))
            return false;
    }
    return true;
}

static bool install_boot_files(void)
{
    static const char source_efi[] = "/EFI/BOOT/BOOTX64.EFI";
    static const char target_efi[] =
        INSTALL_EFI_MOUNT "/EFI/BOOT/BOOTX64.EFI";
    static const char source_kernel[] =
        "/boot/kernel.elf";
    static const char target_kernel[] =
        INSTALL_EFI_MOUNT "/boot/kernel.elf";
    static const char source_bios_file[] =
        "/boot/limine/limine-bios.sys";
    static const char target_bios_file[] =
        INSTALL_EFI_MOUNT "/boot/limine/limine-bios.sys";
    static const char efi_config[] =
        INSTALL_EFI_MOUNT "/boot/limine/limine.conf";
    static const char root_config[] =
        INSTALL_ROOT_MOUNT "/boot/limine/limine.conf";

    serial_install_progress_update(92);
    if (!install_directory(
            INSTALL_ROOT_MOUNT "/boot",
            "Could not create installed boot directory") ||
        !install_directory(
            INSTALL_ROOT_MOUNT "/boot/limine",
            "Could not create installed Limine directory") ||
        !copy_file(
            source_kernel,
            INSTALL_ROOT_MOUNT "/boot/kernel.elf",
            "Could not copy installed kernel") ||
        !copy_file(
            source_bios_file,
            INSTALL_ROOT_MOUNT "/boot/limine/limine-bios.sys",
            "Could not copy installed BIOS boot file") ||
        !write_file(
            root_config,
            installed_limine_config,
            sizeof(installed_limine_config) - 1,
            "Could not write installed Limine configuration"))
        return false;
    serial_install_progress_update(94);
    if (!install_directory(
            INSTALL_EFI_MOUNT "/EFI",
            "Could not create EFI directory") ||
        !install_directory(
            INSTALL_EFI_MOUNT "/EFI/BOOT",
            "Could not create EFI boot directory") ||
        !install_directory(
            INSTALL_EFI_MOUNT "/boot",
            "Could not create EFI boot files directory") ||
        !install_directory(
            INSTALL_EFI_MOUNT "/boot/limine",
            "Could not create EFI Limine directory"))
        return false;
    serial_install_progress_update(96);
    if (!copy_file(
            source_efi,
            target_efi,
            "Could not copy EFI bootloader") ||
        !copy_file(
            source_kernel,
            target_kernel,
            "Could not copy kernel to EFI partition") ||
        !copy_file(
            source_bios_file,
            target_bios_file,
            "Could not copy BIOS boot file to EFI partition") ||
        !write_file(
            efi_config,
            installed_limine_config,
            sizeof(installed_limine_config) - 1,
            "Could not write EFI Limine configuration"))
        return false;
    serial_install_progress_update(98);
    return true;
}

static int64_t installer_abort(int64_t error, const char *message)
{
    if (!installer_failure_message)
        installer_failure_message = message;
    serial_log(SERIAL_LOG_ERROR, installer_failure_message);
    serial_install_progress_error(installer_failure_message);
    return error;
}

int64_t installer_run(const os_install_request_t *request)
{
    if (!request)
        return -SYSCALL_EINVAL;
    size_t device_name_length =
        string_length(request->device, sizeof(request->device));
    if (!device_name_length ||
        device_name_length == sizeof(request->device))
        return -SYSCALL_EINVAL;
    int device_index = device_by_name(request->device);
    if (device_index < 0)
        return -SYSCALL_ENODEV;
    const block_device_t *device =
        block_device_get((uint32_t)device_index);
    if (!device || !device->writable ||
        device->sector_size != BLOCK_SECTOR_SIZE)
        return -SYSCALL_EROFS;
    if (!confirmation_matches(request, device->name))
        return -SYSCALL_EPERM;
    if (vfs_root_uses_device((uint32_t)device_index))
        return -SYSCALL_EBUSY;

    uint64_t efi_start = INSTALL_BIOS_START + INSTALL_BIOS_SECTORS;
    efi_start = (efi_start + INSTALL_SECTOR_ALIGNMENT - 1) &
        ~(INSTALL_SECTOR_ALIGNMENT - 1);
    uint64_t root_start = efi_start + INSTALL_EFI_SECTORS;
    root_start = (root_start + INSTALL_SECTOR_ALIGNMENT - 1) &
        ~(INSTALL_SECTOR_ALIGNMENT - 1);
    if (device->sector_count <= root_start + INSTALL_MIN_ROOT_SECTORS + 34)
        return -SYSCALL_ENOSPC;
    uint64_t last_usable = device->sector_count - 34;
    uint64_t root_sectors = last_usable - root_start + 1;
    if (root_sectors > (uint64_t)UINT32_MAX * 2)
        root_sectors = (uint64_t)UINT32_MAX * 2;
    if (root_sectors < INSTALL_MIN_ROOT_SECTORS)
        return -SYSCALL_ENOSPC;

    installer_failure_message = NULL;
    serial_install_progress_begin();
    if (!source_file_exists("/boot/kernel.elf"))
        return installer_abort(
            -SYSCALL_ENOENT,
            "Missing installer source /boot/kernel.elf");
    if (!source_file_exists("/boot/limine/limine-bios.sys"))
        return installer_abort(
            -SYSCALL_ENOENT,
            "Missing installer source /boot/limine/limine-bios.sys");
    if (!source_file_exists("/EFI/BOOT/BOOTX64.EFI"))
        return installer_abort(
            -SYSCALL_ENOENT,
            "Missing installer source /EFI/BOOT/BOOTX64.EFI");
    if (!source_file_exists("/system/install/rootfs.tar"))
        return installer_abort(
            -SYSCALL_ENOENT,
            "Missing installer source /system/install/rootfs.tar");

    serial_log(SERIAL_LOG_WARN, "Installer: wiping selected disk after confirmation");
    if (!vfs_unmount_device_partitions((uint32_t)device_index))
        return installer_abort(
            -SYSCALL_EBUSY,
            "Could not unmount selected disk");
    serial_install_progress_update(2);

    mbr_gpt_partition_t partitions[3] = {0};
    for (size_t i = 0; i < sizeof(gpt_bios_boot_guid); i++)
        partitions[0].type_guid[i] = gpt_bios_boot_guid[i];
    partitions[0].start_lba = INSTALL_BIOS_START;
    partitions[0].sector_count = INSTALL_BIOS_SECTORS;
    string_copy(partitions[0].name, sizeof(partitions[0].name), "BIOS boot");

    for (size_t i = 0; i < sizeof(gpt_efi_system_guid); i++)
        partitions[1].type_guid[i] = gpt_efi_system_guid[i];
    partitions[1].start_lba = efi_start;
    partitions[1].sector_count = INSTALL_EFI_SECTORS;
    string_copy(partitions[1].name, sizeof(partitions[1].name), "EFI System");

    for (size_t i = 0; i < sizeof(gpt_linux_filesystem_guid); i++)
        partitions[2].type_guid[i] = gpt_linux_filesystem_guid[i];
    partitions[2].start_lba = root_start;
    partitions[2].sector_count = root_sectors;
    string_copy(partitions[2].name, sizeof(partitions[2].name), "VeRsitileOS root");

    if (!mbr_create_gpt(
            (uint32_t)device_index,
            partitions,
            sizeof(partitions) / sizeof(partitions[0])))
        return installer_abort(
            -SYSCALL_EIO,
            "Could not create disk partition table");
    serial_install_progress_update(4);
    serial_log(SERIAL_LOG_INFO, "Installer: partition table created");
    serial_log(SERIAL_LOG_INFO, "Installer: formatting EFI system partition");
    if (!fat32_format(
            (uint32_t)device_index,
            efi_start,
            INSTALL_EFI_SECTORS,
            "VERSITILEOS"))
        return installer_abort(
            -SYSCALL_EIO,
            fat32_format_error()
                ? fat32_format_error()
                : "Could not format EFI partition");
    serial_install_progress_update(6);
    serial_log(SERIAL_LOG_INFO, "Installer: formatting ext2 system partition");
    if (!ext2_format(
            (uint32_t)device_index,
            root_start,
            root_sectors,
            "VeRsitileOS"))
        return installer_abort(
            -SYSCALL_EIO,
            "Could not format root partition");
    serial_install_progress_update(8);
    serial_log(SERIAL_LOG_INFO, "Installer: installing BIOS boot code");
    if (!copy_bios_stage(
            (uint32_t)device_index,
            INSTALL_BIOS_START,
            INSTALL_BIOS_SECTORS))
        return installer_abort(
            -SYSCALL_EIO,
            "Could not install BIOS boot code");
    serial_install_progress_update(10);

    char root_mount_path[64];
    char efi_mount_path[64];
    if (!string_copy(
            root_mount_path,
            sizeof(root_mount_path),
            INSTALL_ROOT_MOUNT) ||
        !string_copy(
            efi_mount_path,
            sizeof(efi_mount_path),
            INSTALL_EFI_MOUNT))
        return installer_abort(
            -SYSCALL_EINVAL,
            "Could not prepare installer mount paths");
    if (!vfs_mount_ext2_partition(
            (uint32_t)device_index,
            root_start,
            root_sectors,
            root_mount_path))
        return installer_abort(
            -SYSCALL_EIO,
            "Could not mount root partition");

    bool efi_mounted = vfs_mount_fat32_partition(
            (uint32_t)device_index,
            efi_start,
            INSTALL_EFI_SECTORS,
            efi_mount_path);
    if (!efi_mounted)
    {
        (void)vfs_unmount_ext2(root_mount_path);
        return installer_abort(
            -SYSCALL_EIO,
            "Could not mount EFI partition");
    }

    serial_log(SERIAL_LOG_INFO, "Installer: extracting installed root filesystem");
    static const char *const installed_directories[] = {
        INSTALL_ROOT_MOUNT "/dev",
        INSTALL_ROOT_MOUNT "/proc",
        INSTALL_ROOT_MOUNT "/sys",
        INSTALL_ROOT_MOUNT "/tmp",
        INSTALL_ROOT_MOUNT "/mnt",
        INSTALL_ROOT_MOUNT "/var"
    };
    bool success = extract_rootfs_archive();
    for (size_t i = 0;
         success &&
         i < sizeof(installed_directories) / sizeof(installed_directories[0]);
         i++)
    {
        if (!make_directory(installed_directories[i]))
        {
            log_install_path_error(
                "could not create installed system directory",
                installed_directories[i],
                0);
            success = false;
        }
    }
    if (success)
        success = install_boot_files();
    if (!success)
        serial_log(
            SERIAL_LOG_ERROR,
            "Installer: root filesystem preparation failed");
    if (success)
        serial_log(SERIAL_LOG_INFO, "Installer: installed BIOS and UEFI boot files");
    bool efi_unmounted = vfs_unmount_fat32(efi_mount_path);
    bool root_unmounted = vfs_unmount_ext2(root_mount_path);
    if (!efi_unmounted)
        serial_log(SERIAL_LOG_ERROR, "Installer: failed to sync EFI partition");
    if (!root_unmounted)
        serial_log(SERIAL_LOG_ERROR, "Installer: failed to sync root partition");
    if (!success || !efi_unmounted || !root_unmounted)
    {
        const char *message = installer_failure_message;
        if (!message)
            message = !efi_unmounted
                ? "Could not sync EFI partition"
                : !root_unmounted
                    ? "Could not sync root partition"
                    : "Root filesystem preparation failed";
        return installer_abort(-SYSCALL_EIO, message);
    }

    serial_log(SERIAL_LOG_INFO, "Installer: installation completed");
    serial_install_progress_complete();
    return 0;
}
