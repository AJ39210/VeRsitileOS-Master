#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <os/gui.h>
#include <unistd.h>

#define LINE_SIZE 256
#define COLOR_DEFAULT 0x00E6EDF3
#define COLOR_GREEN 0x0040FF40
#define COLOR_RED 0x00FF4040
#define COLOR_YELLOW 0x00FFFF00
#define COLOR_ORANGE 0x00FF8C00

static const char *systemd_config_path;
static uint32_t log_level = 4;
static uint32_t watchdog_seconds;

typedef enum
{
    STATUS_ERROR,
    STATUS_WARNING,
    STATUS_INFO,
    STATUS_OK
} status_level_t;

static void trim_line(char *line)
{
    char *comment = strchr(line, '#');
    if (comment)
        *comment = '\0';
    size_t length = strlen(line);
    while (length &&
           (line[length - 1] == '\n' || line[length - 1] == '\r' ||
            line[length - 1] == ' ' || line[length - 1] == '\t'))
        line[--length] = '\0';

    size_t start = 0;
    while (line[start] == ' ' || line[start] == '\t')
        start++;
    if (start)
        memmove(line, line + start, strlen(line + start) + 1);
}

static bool config_value(
    const char *config_path,
    const char *key,
    char *value,
    size_t capacity)
{
    if (!config_path || !key || !value || capacity < 2)
        return false;
    FILE *config = fopen(config_path, "rb");
    if (!config)
        return false;

    char line[LINE_SIZE];
    bool found = false;
    size_t key_length = strlen(key);
    while (fgets(line, sizeof(line), config))
    {
        trim_line(line);
        if (!line[0] || line[0] == '[')
            continue;

        char *separator = strchr(line, '=');
        if (!separator)
            continue;
        *separator++ = '\0';
        trim_line(line);
        while (*separator == ' ' || *separator == '\t')
            separator++;
        char *end = separator + strlen(separator);
        while (end > separator &&
               (end[-1] == ' ' || end[-1] == '\t'))
            *--end = '\0';

        if (strlen(line) != key_length || strcmp(line, key) != 0)
            continue;
        size_t length = strlen(separator);
        if (!length || length >= capacity)
            break;
        memcpy(value, separator, length + 1);
        found = true;
        break;
    }

    if (ferror(config))
        found = false;
    (void)fclose(config);
    return found;
}

static bool status_enabled(status_level_t level)
{
    if (log_level == 0)
        return false;
    if (log_level == 4)
        return true;
    switch (level)
    {
        case STATUS_ERROR: return log_level == 1;
        case STATUS_WARNING: return log_level == 2;
        case STATUS_INFO: return log_level == 3;
        case STATUS_OK: return false;
        default: return false;
    }
}

static void status_message(
    status_level_t level,
    uint32_t color,
    const char *tag,
    const char *format,
    ...)
{
    if (!status_enabled(level))
        return;

    (void)os_console_set_color(color);
    printf("%s", tag);
    (void)os_console_set_color(COLOR_DEFAULT);
    va_list arguments;
    va_start(arguments, format);
    (void)vprintf(format, arguments);
    va_end(arguments);
    putchar('\n');
}

static void status_ok(const char *message)
{
    status_message(STATUS_OK, COLOR_GREEN, "[  OK  ] ", "%s", message);
}

static void status_failed(const char *message)
{
    status_message(STATUS_ERROR, COLOR_RED, "[FAILED] ", "%s", message);
}

static void status_info(const char *message)
{
    status_message(STATUS_INFO, COLOR_YELLOW, "[ INFO ] ", "%s", message);
}

static void status_warning(const char *message)
{
    status_message(STATUS_WARNING, COLOR_ORANGE, "[WARN ] ", "%s", message);
}

static bool parse_unsigned(
    const char *text,
    uint32_t maximum,
    uint32_t *value)
{
    if (!text || !*text || !value)
        return false;
    uint32_t parsed = 0;
    for (const char *cursor = text; *cursor; cursor++)
    {
        if (*cursor < '0' || *cursor > '9')
            return false;
        uint32_t digit = (uint32_t)(*cursor - '0');
        if (digit > maximum)
            return false;
        if (parsed > (maximum - digit) / 10)
            return false;
        parsed = parsed * 10 + digit;
    }
    if (parsed > maximum)
        return false;
    *value = parsed;
    return true;
}

static bool load_settings(void)
{
    char value[LINE_SIZE];
    if (!config_value(
            systemd_config_path, "Log_Level", value, sizeof(value)) ||
        !parse_unsigned(value, 4, &log_level))
    {
        status_failed("Log_Level must be a value from 0 through 4");
        return false;
    }

    char watchdog_enabled[LINE_SIZE];
    if (!config_value(systemd_config_path, "RebootWatchdog",
                      watchdog_enabled, sizeof(watchdog_enabled)) ||
        (strcmp(watchdog_enabled, "y") != 0 &&
         strcmp(watchdog_enabled, "n") != 0))
    {
        status_failed("RebootWatchdog must be y or n");
        return false;
    }
    if (!config_value(systemd_config_path, "RebootWatchdogSec",
                      value, sizeof(value)) ||
        !parse_unsigned(value, 3600, &watchdog_seconds) ||
        (strcmp(watchdog_enabled, "y") == 0 && watchdog_seconds == 0))
    {
        status_failed("RebootWatchdogSec must be between 1 and 3600");
        return false;
    }
    if (strcmp(watchdog_enabled, "n") == 0)
        watchdog_seconds = 0;
    if (os_systemd_watchdog(watchdog_seconds) < 0)
    {
        status_failed("could not configure the systemd watchdog");
        return false;
    }
    if (watchdog_seconds)
        status_message(
            STATUS_INFO, COLOR_YELLOW, "[ INFO ] ",
            "software watchdog enabled (%u seconds)", watchdog_seconds);
    else
        status_info("software watchdog disabled");

    return true;
}

static bool read_setting(const char *path, char *value, size_t capacity)
{
    if (!value || capacity < 2)
        return false;
    FILE *file = fopen(path, "rb");
    if (!file)
        return false;

    size_t length = fread(value, 1, capacity - 1, file);
    bool failed = ferror(file) != 0;
    value[length] = '\0';
    while (length &&
           (value[length - 1] == '\n' || value[length - 1] == '\r'))
        value[--length] = '\0';
    (void)fclose(file);
    return !failed && length != 0;
}

static uint32_t directory_entry_count(const char *path)
{
    os_directory_entry_t entries[8];
    os_directory_request_t request = {
        .capacity = sizeof(entries) / sizeof(entries[0])
    };
    size_t length = strlen(path);
    if (!length || length >= sizeof(request.path))
        return UINT32_MAX;
    memcpy(request.path, path, length + 1);
    while (length > 1 && request.path[length - 1] == '/')
        request.path[--length] = '\0';

    uint32_t total = 0;
    for (;;)
    {
        int64_t count = os_readdir(&request, entries);
        if (count < 0 || (uint64_t)count > request.capacity ||
            (uint64_t)count > UINT32_MAX - total)
            return UINT32_MAX;
        if (!count)
            return total;
        total += (uint32_t)count;
        request.offset += (uint64_t)count;
    }
}

static bool verify_configured_paths(void)
{
    FILE *config = fopen(systemd_config_path, "rb");
    if (!config)
    {
        status_failed("could not read configured systemd file");
        return false;
    }

    char line[LINE_SIZE];
    uint32_t directories_checked = 0;
    uint32_t files_checked = 0;
    uint32_t failures = 0;
    while (fgets(line, sizeof(line), config))
    {
        trim_line(line);
        if (line[0] == '#' || !line[0])
            continue;
        if (line[0] == '[')
            continue;

        char *separator = strchr(line, '=');
        if (separator)
        {
            *separator++ = '\0';
            trim_line(separator);
            if (separator[0] != '/')
                continue;
            memmove(line, separator, strlen(separator) + 1);
        }

        size_t length = strlen(line);
        if (line[0] != '/' || length >= sizeof(line) - 1)
        {
            status_failed("invalid path entry in main.cfg");
            failures++;
            continue;
        }

        if (line[length - 1] == '/')
        {
            uint32_t count = directory_entry_count(line);
            if (count == UINT32_MAX)
            {
                status_message(
                    STATUS_ERROR, COLOR_RED, "[FAILED] ",
                    "directory scan failed: %s", line);
                failures++;
            }
            else
            {
                status_message(
                    STATUS_OK, COLOR_GREEN, "[  OK  ] ",
                    "verified %s (%u entries)", line, count);
            }
            directories_checked++;
        }
        else
        {
            FILE *file = fopen(line, "rb");
            if (!file)
            {
                status_message(
                    STATUS_ERROR, COLOR_RED, "[FAILED] ",
                    "required file missing: %s", line);
                failures++;
            }
            else
            {
                if (fgetc(file) == EOF && ferror(file))
                {
                    status_message(
                        STATUS_ERROR, COLOR_RED, "[FAILED] ",
                        "could not read: %s", line);
                    failures++;
                }
                else
                {
                    status_message(
                        STATUS_OK, COLOR_GREEN, "[  OK  ] ",
                        "verified %s", line);
                }
                (void)fclose(file);
            }
            files_checked++;
        }
    }

    if (ferror(config))
        failures++;
    (void)fclose(config);

    status_message(
        STATUS_INFO, COLOR_YELLOW, "[ INFO ] ",
        "checked %u directories and %u files",
        directories_checked, files_checked);
    return failures == 0;
}

static void report_system_identity(void)
{
    static const struct
    {
        const char *label;
        const char *key;
    } settings[] = {
        { "OS", "OSName" },
        { "Host", "Host" },
        { "Desktop", "Desktop" }
    };

    char value_path[LINE_SIZE];
    char value[LINE_SIZE];
    for (size_t i = 0; i < sizeof(settings) / sizeof(settings[0]); i++)
    {
        if (!config_value(
                systemd_config_path, settings[i].key,
                value_path, sizeof(value_path)) ||
            !read_setting(value_path, value, sizeof(value)))
        {
            status_message(
                STATUS_ERROR, COLOR_RED, "[FAILED] ",
                "%s identity setting unavailable: %s",
                settings[i].label, settings[i].key);
            continue;
        }
        status_message(
            STATUS_OK, COLOR_GREEN, "[  OK  ] ",
            "%s: %s", settings[i].label, value);
    }

    if (config_value(
            systemd_config_path, "Version",
            value_path, sizeof(value_path)) &&
        read_setting(value_path, value, sizeof(value)))
        status_message(
            STATUS_OK, COLOR_GREEN, "[  OK  ] ",
            "Systemd: %s", value);
    else
        status_failed("systemd version file unavailable");
}

static void report_kernel_modules(void)
{
    char modules_path[LINE_SIZE];
    if (!config_value(systemd_config_path, "Modules", modules_path,
                      sizeof(modules_path)))
    {
        status_failed("kernel module directory is not configured");
        return;
    }

    uint32_t entries = directory_entry_count(modules_path);
    if (entries == UINT32_MAX)
    {
        status_info("kernel module directory is absent; drivers are built in");
        return;
    }

    os_directory_entry_t items[8];
    os_directory_request_t request = {
        .capacity = sizeof(items) / sizeof(items[0])
    };
    size_t length = strlen(modules_path);
    while (length > 1 && modules_path[length - 1] == '/')
        modules_path[--length] = '\0';
    memcpy(request.path, modules_path, length + 1);
    uint32_t ko_count = 0;
    uint32_t loaded_count = 0;
    uint32_t failed_count = 0;
    bool scan_failed = false;
    for (;;)
    {
        int64_t count = os_readdir(&request, items);
        if (count < 0 || (uint64_t)count > request.capacity)
        {
            scan_failed = true;
            break;
        }
        if (count == 0)
            break;
        for (int64_t i = 0; i < count; i++)
        {
            size_t name_length = strlen(items[i].name);
            if (name_length >= 3 &&
                !items[i].is_directory &&
                strcmp(items[i].name + name_length - 3, ".ko") == 0)
            {
                char module_path[LINE_SIZE];
                int path_length = snprintf(
                    module_path,
                    sizeof(module_path),
                    "%s/%s",
                    modules_path,
                    items[i].name);
                ko_count++;
                if (path_length < 0 ||
                    (size_t)path_length >= sizeof(module_path))
                {
                    status_message(
                        STATUS_ERROR, COLOR_RED, "[FAILED] ",
                        "module path is too long: %s", items[i].name);
                    failed_count++;
                    continue;
                }

                int64_t result = os_module_load(module_path);
                if (result == 0)
                {
                    status_message(
                        STATUS_OK, COLOR_GREEN, "[  OK  ] ",
                        "loaded kernel module %s", items[i].name);
                    loaded_count++;
                }
                else
                {
                    status_message(
                        STATUS_ERROR, COLOR_RED, "[FAILED] ",
                        "could not load %s (error %ld)",
                        items[i].name,
                        (long)result);
                    failed_count++;
                }
            }
        }
        request.offset += (uint64_t)count;
    }

    if (scan_failed)
    {
        status_failed("could not enumerate kernel module directory");
        return;
    }
    if (!ko_count)
        status_info("no .ko modules; remaining drivers are built in");
    else
        status_message(
            failed_count ? STATUS_WARNING : STATUS_INFO,
            failed_count ? COLOR_ORANGE : COLOR_YELLOW,
            failed_count ? "[WARN ] " : "[ INFO ] ",
            "loaded %u of %u kernel modules (%u failures)",
            loaded_count,
            ko_count,
            failed_count);
}

static void systemd_yield(void)
{
    if (watchdog_seconds)
        (void)os_systemd_watchdog(watchdog_seconds);
    yield();
}

static void run_terminal(void)
{
    char terminal_path[LINE_SIZE];
    if (!config_value(
            systemd_config_path, "Terminal",
            terminal_path, sizeof(terminal_path)))
    {
        status_failed("Terminal path is not configured in main.cfg");
        for (;;)
            systemd_yield();
    }

    for (;;)
    {
        if (os_console_clear() < 0)
            status_failed("could not clear the console before starting Terminal");

        int64_t pid = os_spawn_elf(terminal_path);
        if (pid < 0)
        {
            status_failed("Terminal could not start; retrying");
        }
        else
        {
            while (os_task_active((uint64_t)pid) > 0)
                systemd_yield();
            status_warning("Terminal exited; restarting");
        }
        for (uint32_t i = 0; i < 1000; i++)
            systemd_yield();
    }
}

int main(int argc, char **argv)
{
    if (argc < 2 || !argv[1] || !argv[1][0])
    {
        status_failed("systemd config path argument is missing");
        for (;;)
            systemd_yield();
    }
    systemd_config_path = argv[1];
    if (os_systemd_log_control(1) < 0)
    {
        status_failed("could not enable the systemd log");
        for (;;)
            systemd_yield();
    }

    (void)os_console_clear();
    char configured_path[LINE_SIZE];
    FILE *config = fopen(systemd_config_path, "rb");
    if (!config ||
        !config_value(systemd_config_path, "Config", configured_path,
                      sizeof(configured_path)) ||
        strcmp(configured_path, systemd_config_path) != 0)
    {
        status_failed("systemd configuration is missing or unreadable");
        if (config)
            (void)fclose(config);
        for (;;)
            systemd_yield();
    }
    (void)fclose(config);
    if (!load_settings())
        for (;;)
            systemd_yield();

    status_ok("systemd configuration found");
    if (log_level != 0)
        status_info("VeRsitileOS systemd starting");

    report_system_identity();
    bool paths_ok = verify_configured_paths();
    report_kernel_modules();
    int64_t network_result = os_network_init();
    if (network_result == 0)
        status_ok("network stack initialized");
    else
        status_warning("network stack unavailable");
    if (paths_ok)
        status_ok("system paths verified");
    else
        status_failed("some configured paths failed verification");

    char log_directory[LINE_SIZE];
    if (config_value(systemd_config_path, "Write_Logs_At",
                     log_directory, sizeof(log_directory)))
        status_ok("systemd log configured");
    else
        status_info("systemd log directory is not configured in main.cfg");

    status_ok("starting terminal");
    run_terminal();
}
