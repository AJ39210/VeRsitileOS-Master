#include <stdbool.h>
#include <stdint.h>

#include "syscall.h"
#include "pci.h"
#include "radeonhd.h"
#include "sysfs.h"
#include "vfs.h"

extern const char kernel_version[];

typedef struct
{
    const char *path;
    const char *content;
} sysfs_entry_t;

static const sysfs_entry_t entries[] = {
    { "/sys/kernel/osrelease", kernel_version },
    { "/sys/devices/system/cpu/online", "0\n" },
    { "/sys/class/tty/console", "/dev/console\n" },
    { "/sys/block/sr0/device/type", "5\n" }
};

static const char *pci_attributes[] = {
    "vendor",
    "device",
    "class",
    "revision",
    "subsystem_vendor",
    "subsystem_device",
    "manufacturer",
    "device_name",
    "class_name",
    "interrupt_line",
    "interrupt_pin",
    "command",
    "status",
    "resource"
};

static bool string_equal(const char *left, const char *right)
{
    while (*left && *right && *left == *right)
    {
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static size_t string_length(const char *text)
{
    size_t length = 0;
    while (text[length])
        length++;
    return length;
}

static bool pci_sysfs_device_path(
    const char *path,
    const pci_device_t **device,
    const char **attribute
)
{
    static const char prefix[] = "/sys/bus/pci/devices/";
    if (!path)
        return false;
    for (size_t i = 0; i < sizeof(prefix) - 1; i++)
    {
        if (path[i] != prefix[i])
            return false;
    }

    char bdf[PCI_BDF_NAME_LENGTH];
    size_t bdf_length = PCI_BDF_NAME_LENGTH - 1;
    for (size_t i = 0; i < bdf_length; i++)
    {
        char character = path[sizeof(prefix) - 1 + i];
        if (!character)
            return false;
        bdf[i] = character;
    }
    bdf[bdf_length] = '\0';
    const pci_device_t *found = pci_find_bdf(bdf);
    if (!found)
        return false;

    const char *suffix = path + sizeof(prefix) - 1 + bdf_length;
    if (*suffix && *suffix != '/')
        return false;
    if (device)
        *device = found;
    if (attribute)
        *attribute = *suffix == '/' ? suffix + 1 : NULL;
    return true;
}

static bool is_pci_attribute(const char *attribute)
{
    if (!attribute)
        return false;
    for (size_t i = 0;
         i < sizeof(pci_attributes) / sizeof(pci_attributes[0]);
         i++)
    {
        if (string_equal(attribute, pci_attributes[i]))
            return true;
    }
    return false;
}

static bool is_radeonhd_attribute(const char *attribute)
{
    return attribute &&
        (string_equal(attribute, "driver") ||
         string_equal(attribute, "generation") ||
         string_equal(attribute, "driver_status") ||
         string_equal(attribute, "debug"));
}

bool sysfs_is_directory(const char *path)
{
    static const char *directories[] = {
        "/sys",
        "/sys/kernel",
        "/sys/devices",
        "/sys/devices/system",
        "/sys/devices/system/cpu",
        "/sys/class",
        "/sys/class/tty",
        "/sys/bus",
        "/sys/bus/pci",
        "/sys/bus/pci/devices",
        "/sys/block",
        "/sys/block/sr0",
        "/sys/block/sr0/device"
    };

    if (!path)
        return false;

    for (uint32_t i = 0; i < sizeof(directories) / sizeof(directories[0]); i++)
    {
        if (i >= 10 && !vfs_root_is_mounted())
            continue;
        if (string_equal(path, directories[i]))
            return true;
    }

    const pci_device_t *device;
    const char *attribute;
    return pci_sysfs_device_path(path, &device, &attribute) &&
        attribute == NULL;
}

bool sysfs_file_size(const char *path, uint64_t *size)
{
    if (!path || !size)
        return false;

    const pci_device_t *pci_device;
    const char *pci_attribute;
    if (pci_sysfs_device_path(path, &pci_device, &pci_attribute) &&
        is_pci_attribute(pci_attribute))
    {
        char buffer[256];
        size_t length = pci_format_attribute(
            pci_device, pci_attribute, buffer, sizeof(buffer));
        if (!length)
            return false;
        *size = length;
        return true;
    }
    if (pci_sysfs_device_path(path, &pci_device, &pci_attribute) &&
        is_radeonhd_attribute(pci_attribute))
    {
        char buffer[512];
        const char *attribute =
            string_equal(pci_attribute, "driver_status")
                ? "status"
                : pci_attribute;
        size_t length = radeonhd_format_attribute(
            pci_device, attribute, buffer, sizeof(buffer));
        if (!length)
            return false;
        *size = length;
        return true;
    }

    for (uint32_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++)
    {
        if (i == 3 && !vfs_root_is_mounted())
            continue;
        if (!string_equal(path, entries[i].path))
            continue;
        uint64_t length = 0;
        while (entries[i].content[length])
            length++;
        *size = length;
        return true;
    }
    return false;
}

int64_t sysfs_list(
    const char *path,
    uint64_t offset,
    vfs_dirent_t *entries,
    uint64_t capacity
)
{
    static const char *device_attributes[] = {
        "vendor", "device", "class", "revision", "subsystem_vendor",
        "subsystem_device", "manufacturer", "device_name", "class_name",
        "interrupt_line", "interrupt_pin", "command", "status", "resource",
        "driver", "generation", "driver_status", "debug"
    };
    if (!path || (capacity && !entries))
        return -SYSCALL_EINVAL;

    if (string_equal(path, "/sys/bus/pci/devices"))
    {
        uint64_t total = pci_device_count();
        if (offset >= total)
            return 0;
        uint64_t returned = total - offset;
        if (returned > capacity)
            returned = capacity;
        for (uint64_t i = 0; i < returned; i++)
        {
            const pci_device_t *device =
                pci_device_get((uint32_t)(offset + i));
            if (!device || !pci_format_bdf(
                    device, entries[i].name, sizeof(entries[i].name)))
                return -SYSCALL_EIO;
            entries[i].size = 0;
            entries[i].is_directory = true;
        }
        return (int64_t)returned;
    }

    const pci_device_t *device;
    const char *attribute;
    if (!pci_sysfs_device_path(path, &device, &attribute) || attribute)
        return -SYSCALL_ENOENT;

    uint64_t total =
        sizeof(device_attributes) / sizeof(device_attributes[0]);
    if (offset >= total)
        return 0;
    uint64_t returned = total - offset;
    if (returned > capacity)
        returned = capacity;
    for (uint64_t i = 0; i < returned; i++)
    {
        const char *name = device_attributes[offset + i];
        size_t length = string_length(name);
        if (length >= sizeof(entries[i].name))
            return -SYSCALL_EIO;
        for (size_t j = 0; j <= length; j++)
            entries[i].name[j] = name[j];
        char value[512];
        if (is_radeonhd_attribute(name))
        {
            const char *attribute = string_equal(name, "driver_status")
                ? "status"
                : name;
            entries[i].size = radeonhd_format_attribute(
                device, attribute, value, sizeof(value));
        }
        else
        {
            entries[i].size = pci_format_attribute(
                device, name, value, sizeof(value));
        }
        entries[i].is_directory = false;
    }
    return (int64_t)returned;
}

int64_t sysfs_read(
    const char *path,
    uint64_t offset,
    void *buffer,
    uint64_t size
)
{
    uint64_t length;
    if (!sysfs_file_size(path, &length))
        return -SYSCALL_ENOENT;

    const pci_device_t *pci_device;
    const char *pci_attribute;
    if (pci_sysfs_device_path(path, &pci_device, &pci_attribute) &&
        is_pci_attribute(pci_attribute))
    {
        char content[256];
        size_t content_length = pci_format_attribute(
            pci_device, pci_attribute, content, sizeof(content));
        if (!content_length)
            return -SYSCALL_EIO;
        if (offset >= content_length)
            return 0;
        if (size > content_length - offset)
            size = content_length - offset;
        for (uint64_t i = 0; i < size; i++)
            ((char *)buffer)[i] = content[offset + i];
        return (int64_t)size;
    }
    if (pci_sysfs_device_path(path, &pci_device, &pci_attribute) &&
        is_radeonhd_attribute(pci_attribute))
    {
        char content[512];
        const char *attribute =
            string_equal(pci_attribute, "driver_status")
                ? "status"
                : pci_attribute;
        size_t content_length = radeonhd_format_attribute(
            pci_device, attribute, content, sizeof(content));
        if (!content_length)
            return -SYSCALL_EIO;
        if (offset >= content_length)
            return 0;
        if (size > content_length - offset)
            size = content_length - offset;
        for (uint64_t i = 0; i < size; i++)
            ((char *)buffer)[i] = content[offset + i];
        return (int64_t)size;
    }

    const char *content = 0;
    for (uint32_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++)
    {
        if (i == 3 && !vfs_root_is_mounted())
            continue;
        if (string_equal(path, entries[i].path))
        {
            content = entries[i].content;
            break;
        }
    }

    if (!content)
        return -SYSCALL_ENOENT;
    if (offset >= length)
        return 0;
    if (size > length - offset)
        size = length - offset;

    for (uint64_t i = 0; i < size; i++)
        ((char *)buffer)[i] = content[offset + i];
    return (int64_t)size;
}
