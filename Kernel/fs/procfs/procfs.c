#include <stdbool.h>
#include <stdint.h>

#include "iso9660.h"
#include "pmm.h"
#include "procfs.h"
#include "syscall.h"
#include "task.h"
#include "vfs.h"

#define PROC_CONTENT_MAX 4096

typedef struct
{
    char *data;
    uint64_t length;
    uint64_t capacity;
} text_builder_t;

static void append_char(text_builder_t *text, char character)
{
    if (text->length + 1 >= text->capacity)
        return;
    text->data[text->length++] = character;
    text->data[text->length] = '\0';
}

static void append_string(text_builder_t *text, const char *string)
{
    while (*string)
        append_char(text, *string++);
}

static void append_unsigned(text_builder_t *text, uint64_t value)
{
    char digits[21];
    uint32_t length = 0;

    do
    {
        digits[length++] = '0' + value % 10;
        value /= 10;
    } while (value && length < sizeof(digits));

    while (length)
        append_char(text, digits[--length]);
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

static void cpuid(
    uint32_t leaf,
    uint32_t subleaf,
    uint32_t *a,
    uint32_t *b,
    uint32_t *c,
    uint32_t *d
)
{
    __asm__ volatile (
        "cpuid"
        : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
        : "a"(leaf), "c"(subleaf)
    );
}

static void render_cpuinfo(text_builder_t *text)
{
    uint32_t a, b, c, d;
    cpuid(0, 0, &a, &b, &c, &d);

    char vendor[13];
    for (uint32_t i = 0; i < 4; i++)
    {
        vendor[i] = ((char *)&b)[i];
        vendor[i + 4] = ((char *)&d)[i];
        vendor[i + 8] = ((char *)&c)[i];
    }
    vendor[12] = '\0';

    append_string(text, "processor\t: 0\nvendor_id\t: ");
    append_string(text, vendor);
    append_string(text, "\n");

    if (a >= 1)
    {
        cpuid(1, 0, &a, &b, &c, &d);
        uint32_t base_family = (a >> 8) & 0xF;
        uint32_t family = base_family;
        uint32_t model = (a >> 4) & 0xF;
        uint32_t extended_family = (a >> 20) & 0xFF;
        uint32_t extended_model = (a >> 16) & 0xF;
        if (base_family == 0xF)
            family += extended_family;
        if (base_family == 6 || base_family == 0xF)
            model |= extended_model << 4;

        append_string(text, "cpu family\t: ");
        append_unsigned(text, family);
        append_string(text, "\nmodel\t\t: ");
        append_unsigned(text, model);
        append_string(text, "\nstepping\t: ");
        append_unsigned(text, a & 0xF);
        append_string(text, "\n");
    }

    append_string(text, "online processors: 1\n");
}

static void render_meminfo(text_builder_t *text)
{
    uint64_t total_kib = pmm_get_total_memory() / 1024;
    uint64_t free_kib = pmm_get_free_memory() / 1024;
    uint64_t used_kib = pmm_get_used_memory() / 1024;

    append_string(text, "MemTotal:       ");
    append_unsigned(text, total_kib);
    append_string(text, " kB\nMemFree:        ");
    append_unsigned(text, free_kib);
    append_string(text, " kB\nMemAvailable:   ");
    append_unsigned(text, free_kib);
    append_string(text, " kB\nMemUsed:        ");
    append_unsigned(text, used_kib);
    append_string(text, " kB\nBuffers:        0 kB\nCached:         0 kB\n");
    append_string(text, "SwapCached:     0 kB\nSwapTotal:      0 kB\n");
    append_string(text, "SwapFree:       0 kB\n");
}

static void render_devices(text_builder_t *text)
{
    append_string(text, "Character devices:\n");
    append_string(text, "  1 mem\n");
    append_string(text, "  4 tty\n");
    append_string(text, " 13 input\n");
    append_string(text, " 29 framebuffer\n\n");
    append_string(text, "Block devices:\n");
    if (vfs_root_is_mounted())
        append_string(text, " 11 cdrom\n");
}

static void render_partitions(text_builder_t *text)
{
    uint32_t sectors = iso9660_get_volume_sectors();

    append_string(text, "major minor  #blocks  name\n");
    if (sectors)
    {
        append_string(text, "  11        0 ");
        append_unsigned(text, (uint64_t)sectors * 2);
        append_string(text, " sr0\n");
    }
    append_char(text, '\n');
}

static bool parse_pid(const char *path, uint64_t *pid, const char **leaf)
{
    static const char prefix[] = "/proc/";
    for (uint32_t i = 0; i < sizeof(prefix) - 1; i++)
    {
        if (path[i] != prefix[i])
            return false;
    }

    const char *cursor = path + sizeof(prefix) - 1;
    if (*cursor < '0' || *cursor > '9')
        return false;

    uint64_t value = 0;
    do
    {
        uint32_t digit = (uint32_t)(*cursor - '0');
        if (value > (UINT64_MAX - digit) / 10)
            return false;
        value = value * 10 + digit;
        cursor++;
    } while (*cursor >= '0' && *cursor <= '9');

    *pid = value;
    *leaf = cursor;
    return true;
}

static bool get_task_by_pid(uint64_t pid, task_info_t *result)
{
    for (uint32_t i = 0; i < task_count(); i++)
    {
        task_info_t info;
        if (task_get_info(i, &info) && info.pid == pid)
        {
            *result = info;
            return true;
        }
    }
    return false;
}

static bool render_process_file(
    const char *path,
    text_builder_t *text,
    bool *is_directory
)
{
    uint64_t pid;
    const char *leaf;
    task_info_t info;
    *is_directory = false;

    if (!parse_pid(path, &pid, &leaf) || !get_task_by_pid(pid, &info))
        return false;

    if (*leaf == '\0')
    {
        *is_directory = true;
        return true;
    }

    if (*leaf != '/')
        return false;
    leaf++;

    if (string_equal(leaf, "status"))
    {
        append_string(text, "Name:\tVeRsitileOS\nState:\t");
        append_string(text, info.state);
        append_string(text, "\nPid:\t");
        append_unsigned(text, info.pid);
        append_string(text, "\nPPid:\t");
        append_unsigned(text, info.parent_pid);
        append_string(text, "\n");
        return true;
    }

    if (string_equal(leaf, "stat"))
    {
        append_unsigned(text, info.pid);
        append_string(text, " (VeRsitileOS) ");
        append_char(text, info.state[0] == 'R' ? 'R' : 'S');
        append_char(text, ' ');
        append_unsigned(text, info.parent_pid);
        append_char(text, '\n');
        return true;
    }

    if (string_equal(leaf, "comm"))
    {
        append_string(text, "VeRsitileOS\n");
        return true;
    }

    if (string_equal(leaf, "cmdline"))
    {
        append_string(text, "VeRsitileOS\n");
        return true;
    }

    return false;
}

static bool render_file(
    const char *path,
    char *buffer,
    uint64_t capacity,
    uint64_t *length,
    bool *is_directory
)
{
    text_builder_t text = {
        .data = buffer,
        .length = 0,
        .capacity = capacity
    };
    buffer[0] = '\0';
    *is_directory = false;

    if (string_equal(path, "/proc"))
    {
        *is_directory = true;
        return true;
    }
    if (string_equal(path, "/proc/cpuinfo"))
        render_cpuinfo(&text);
    else if (string_equal(path, "/proc/meminfo"))
        render_meminfo(&text);
    else if (string_equal(path, "/proc/devices"))
        render_devices(&text);
    else if (string_equal(path, "/proc/partitions"))
        render_partitions(&text);
    else if (!render_process_file(path, &text, is_directory))
        return false;

    *length = text.length;
    return true;
}

bool procfs_is_directory(const char *path)
{
    char buffer[PROC_CONTENT_MAX];
    uint64_t length = 0;
    bool is_directory = false;

    if (!path)
        return false;
    return render_file(
        path,
        buffer,
        sizeof(buffer),
        &length,
        &is_directory
    ) && is_directory;
}

bool procfs_file_size(const char *path, uint64_t *size)
{
    char buffer[PROC_CONTENT_MAX];
    bool is_directory = false;
    uint64_t length = 0;

    if (!path || !size ||
        !render_file(
            path,
            buffer,
            sizeof(buffer),
            &length,
            &is_directory) ||
        is_directory)
        return false;

    *size = length;
    return true;
}

int64_t procfs_read(
    const char *path,
    uint64_t offset,
    void *buffer,
    uint64_t size
)
{
    char content[PROC_CONTENT_MAX];
    uint64_t length = 0;
    bool is_directory = false;

    if (!path || (!buffer && size) ||
        !render_file(
            path,
            content,
            sizeof(content),
            &length,
            &is_directory) ||
        is_directory)
        return -SYSCALL_ENOENT;

    if (offset >= length)
        return 0;

    if (size > length - offset)
        size = length - offset;
    for (uint64_t i = 0; i < size; i++)
        ((char *)buffer)[i] = content[offset + i];
    return (int64_t)size;
}
