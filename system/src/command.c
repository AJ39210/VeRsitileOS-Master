#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <os/gui.h>

static void put_text(const char *text)
{
    if (!text)
        return;
    size_t length = strlen(text);
    size_t written = 0;
    while (written < length)
    {
        ssize_t result = write(
            STDOUT_FILENO,
            text + written,
            length - written);
        if (result > 0)
            written += (size_t)result;
        else if (result < 0 && errno == EAGAIN)
            yield();
        else
            return;
    }
}

#if defined(APP_IFCONFIG) || defined(APP_PING) || defined(APP_RESOLVE)
static void put_ipv4(const uint8_t address[4])
{
    char digits[4];
    for (uint32_t octet = 0; octet < 4; octet++)
    {
        uint32_t value = address[octet];
        uint32_t count = 0;
        do
        {
            digits[count++] = (char)('0' + value % 10);
            value /= 10;
        } while (value);
        while (count)
            (void)write(STDOUT_FILENO, &digits[--count], 1);
        if (octet < 3)
            put_text(".");
    }
}
#endif

#if defined(APP_PING) || defined(APP_LSBLK) || defined(APP_INSTALL)
static void put_number(uint64_t value)
{
    char digits[21];
    uint32_t count = 0;
    do
    {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value && count < sizeof(digits));
    while (count)
        (void)write(STDOUT_FILENO, &digits[--count], 1);
}
#endif

#if defined(APP_LSBLK) || defined(APP_INSTALL)
static void put_size(uint64_t bytes)
{
    const uint64_t gibibyte = 1024ULL * 1024 * 1024;
    const uint64_t mebibyte = 1024ULL * 1024;
    if (bytes >= gibibyte)
    {
        put_number(bytes / gibibyte);
        put_text(".");
        put_number((bytes % gibibyte) * 10 / gibibyte);
        put_text("G");
    }
    else if (bytes >= mebibyte)
    {
        put_number(bytes / mebibyte);
        put_text("M");
    }
    else if (bytes >= 1024)
    {
        put_number(bytes / 1024);
        put_text("K");
    }
    else
    {
        put_number(bytes);
        put_text("B");
    }
}
#endif

#if defined(APP_LSBLK)
static int command_lsblk(void)
{
    os_directory_entry_t entries[8];
    os_directory_request_t request = { .capacity = 8 };
    memcpy(request.path, "/dev/disk", sizeof("/dev/disk"));
    put_text("NAME       SIZE  TYPE\n");

    for (;;)
    {
        int64_t count = os_readdir(&request, entries);
        if (count < 0)
        {
            put_text("lsblk: cannot read /dev/disk\n");
            return 1;
        }
        for (int64_t i = 0; i < count; i++)
        {
            const char *name = entries[i].name;
            bool partition = false;
            for (size_t j = 0; name[j]; j++)
            {
                if (name[j] == 'p' && name[j + 1] >= '0' &&
                    name[j + 1] <= '9')
                {
                    partition = true;
                    break;
                }
            }
            put_text(partition ? "  " : "");
            put_text(name);
            size_t name_length = strlen(name);
            while (name_length++ < 11)
                put_text(" ");
            put_size(entries[i].size);
            put_text(partition ? "  part\n" : "  disk\n");
        }
        yield();
        request.offset += (uint64_t)count;
        if (count < (int64_t)(sizeof(entries) / sizeof(entries[0])))
            return 0;
    }
}
#endif

#if defined(APP_INSTALL)
static bool read_input(
    const char *prompt,
    char *buffer,
    size_t capacity,
    bool allow_empty
)
{
    put_text(prompt);
    if (!buffer || capacity < 2)
        return false;

    size_t length = 0;
    size_t overflow_count = 0;
    for (;;)
    {
        char character;
        ssize_t result = read(STDIN_FILENO, &character, 1);
        if (result < 0)
        {
            if (errno == EAGAIN)
            {
                yield();
                continue;
            }
            return false;
        }
        if (!result)
            return false;
        if (character == '\n' || character == '\r')
        {
            put_text("\n");
            break;
        }
        if (character == '\b' || character == 127)
        {
            if (overflow_count)
                overflow_count--;
            else if (length)
            {
                buffer[--length] = '\0';
                put_text("\b \b");
                continue;
            }
            else
                continue;
            put_text("\b \b");
            continue;
        }
        if (character < 32 || character > 126)
            continue;

        if (!overflow_count && length + 1 < capacity)
        {
            buffer[length++] = character;
            buffer[length] = '\0';
        }
        else if (overflow_count != (size_t)-1)
            overflow_count++;
        put_text((char[]){ character, '\0' });
    }

    buffer[length] = '\0';
    if (overflow_count)
        return false;
    if (!length && !allow_empty)
        return false;
    return true;
}

static bool entry_is_partition(const char *name)
{
    for (size_t i = 0; name[i]; i++)
    {
        if (name[i] == 'p' && name[i + 1] >= '0' &&
            name[i + 1] <= '9')
            return true;
    }
    return false;
}

static bool installer_list_devices(char first_device[16])
{
    os_directory_entry_t entries[8];
    os_directory_request_t request = {
        .capacity = sizeof(entries) / sizeof(entries[0])
    };
    memcpy(request.path, "/dev/disk", sizeof("/dev/disk"));
    bool found = false;
    put_text("Detected disks:\n");
    for (;;)
    {
        int64_t count = os_readdir(&request, entries);
        if (count < 0)
            return false;
        for (int64_t i = 0; i < count; i++)
        {
            if (entry_is_partition(entries[i].name))
                continue;
            if (!found)
            {
                size_t length = strlen(entries[i].name);
                if (!length || length >= 16)
                    return false;
                memcpy(first_device, entries[i].name, length + 1);
                found = true;
            }
            put_text("  ");
            put_text(entries[i].name);
            put_text("  ");
            put_size(entries[i].size);
            put_text("\n");
        }
        request.offset += (uint64_t)count;
        if (count < (int64_t)(sizeof(entries) / sizeof(entries[0])))
            break;
        yield();
    }
    return found;
}

static int command_install(void)
{
    os_install_request_t request = {0};
    char first_device[16] = {0};
    if (!installer_list_devices(first_device))
    {
        put_text("install: no writable disk information is available\n");
        return 1;
    }
    put_text("Target disk (default ");
    put_text(first_device);
    put_text("): ");
    char selected_device[16];
    if (!read_input(
            "",
            selected_device,
            sizeof(selected_device),
            true))
    {
        put_text("install: could not read target disk\n");
        return 1;
    }
    size_t selected_length = strlen(selected_device);
    if (!selected_length)
    {
        size_t length = strlen(first_device);
        memcpy(selected_device, first_device, length + 1);
    }
    if (strlen(selected_device) >= sizeof(request.device))
    {
        put_text("install: disk name is too long\n");
        return 2;
    }
    memcpy(
        request.device,
        selected_device,
        strlen(selected_device) + 1);

    put_text("\nWARNING: Installing will erase every partition and file on ");
    put_text(request.device);
    put_text(". It creates BIOS, EFI, and ext2 system partitions.\n");
    put_text("Type WIPE ");
    put_text(request.device);
    put_text(" to confirm: ");
    if (!read_input(
            "",
            request.confirmation,
            sizeof(request.confirmation),
            false))
    {
        put_text("install: confirmation was not read; disk unchanged\n");
        return 1;
    }

    put_text("Installing VeRsitileOS on ");
    put_text(request.device);
    put_text(" for both BIOS and UEFI...\n");
    int64_t result = os_install(&request);
    if (result < 0)
    {
        put_text("install: failed (error ");
        put_number((uint64_t)-result);
        put_text("). The disk may have been partially repartitioned.\n");
        return 1;
    }
    put_text("Installation complete. Reboot and remove the installation media.\n");
    return 0;
}
#endif

#if defined(APP_LS)
static int command_ls(const char *path)
{
    os_directory_entry_t entries[8];
    uint64_t offset = 0;
    for (;;)
    {
        os_directory_request_t request = {
            .offset = offset,
            .capacity = sizeof(entries) / sizeof(entries[0])
        };
        size_t length = strlen(path);
        if (length >= sizeof(request.path))
        {
            put_text("ls: path too long\n");
            return 1;
        }
        memcpy(request.path, path, length + 1);
        int64_t count = os_readdir(&request, entries);
        if (count < 0)
        {
            put_text("ls: cannot read directory\n");
            return 1;
        }
        for (int64_t i = 0; i < count; i++)
        {
            put_text(entries[i].name);
            if (entries[i].is_directory)
                put_text("/");
            put_text("\n");
        }
        yield();
        offset += (uint64_t)count;
        if (count < (int64_t)(sizeof(entries) / sizeof(entries[0])))
            return 0;
    }
}
#endif

#if defined(APP_CAT)
static int command_cat(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file)
    {
        put_text("cat: cannot open file\n");
        return 1;
    }
    char buffer[256];
    size_t count;
    int status = 0;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) != 0)
    {
        size_t written = 0;
        while (written < count)
        {
            ssize_t result = write(
                STDOUT_FILENO,
                buffer + written,
                count - written);
            if (result <= 0)
            {
                if (result < 0 && errno == EAGAIN)
                {
                    yield();
                    continue;
                }
                status = 1;
                break;
            }
            written += (size_t)result;
        }
        if (status)
            break;
        yield();
    }
    if (ferror(file))
        status = 1;
    if (fclose(file) != 0)
        status = 1;
    if (status)
        put_text("cat: read failed\n");
    return status;
}
#endif

#if defined(APP_NEOFETCH)
static void put_logo(const char *text)
{
    const uint8_t *cursor = (const uint8_t *)text;
    while (*cursor)
    {
        uint8_t first = *cursor++;
        if (first < 0x80)
        {
            (void)write(STDOUT_FILENO, &first, 1);
            continue;
        }
        uint32_t codepoint;
        if ((first & 0xE0) == 0xC0 && *cursor)
            codepoint = ((uint32_t)(first & 0x1F) << 6) |
                (*cursor++ & 0x3F);
        else if ((first & 0xF0) == 0xE0 && cursor[0] && cursor[1])
        {
            codepoint = ((uint32_t)(first & 0x0F) << 12) |
                ((uint32_t)(cursor[0] & 0x3F) << 6) |
                (cursor[1] & 0x3F);
            cursor += 2;
        }
        else if ((first & 0xF8) == 0xF0 &&
                 cursor[0] && cursor[1] && cursor[2])
        {
            codepoint = ((uint32_t)(first & 0x07) << 18) |
                ((uint32_t)(cursor[0] & 0x3F) << 12) |
                ((uint32_t)(cursor[1] & 0x3F) << 6) |
                (cursor[2] & 0x3F);
            cursor += 3;
        }
        else
        {
            put_text("?");
            continue;
        }

        char output = '?';
        if (codepoint >= 0x2800 && codepoint <= 0x28FF)
        {
            uint8_t dots = (uint8_t)(codepoint - 0x2800);
            uint32_t density = 0;
            while (dots)
            {
                density += dots & 1;
                dots >>= 1;
            }
            output = density == 0 ? ' ' :
                density < 3 ? '.' : density < 6 ? '+' : '#';
        }
        (void)write(STDOUT_FILENO, &output, 1);
    }
}

static int command_neofetch(void)
{
    char buffer[2048];
    FILE *file = fopen("/etc/neofetch/logo", "rb");
    if (file)
    {
        size_t count = fread(buffer, 1, sizeof(buffer) - 1, file);
        if (count)
        {
            buffer[count] = '\0';
            put_logo(buffer);
            if (buffer[count - 1] != '\n')
                put_text("\n");
        }
        if (ferror(file))
            put_text("neofetch: could not read logo\n");
        (void)fclose(file);
    }
    else
    {
        put_text("VeRsitileOS\n");
    }

    static const char *labels[] = {
        "OS: ", "Host: ", "Kernel: ", "Desktop Environment: "
    };
    static const char *paths[] = {
        "/etc/system/os_name",
        "/etc/system/host",
        "/sys/kernel/osrelease",
        "/etc/system/desktop"
    };
    for (uint32_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++)
    {
        put_text(labels[i]);
        file = fopen(paths[i], "rb");
        if (!file)
        {
            put_text("unknown\n");
            continue;
        }
        size_t count = fread(buffer, 1, sizeof(buffer) - 1, file);
        buffer[count] = '\0';
        while (count && (buffer[count - 1] == '\n' ||
                         buffer[count - 1] == '\r'))
            buffer[--count] = '\0';
        put_text(count ? buffer : "unknown");
        put_text("\n");
        if (ferror(file))
            put_text("neofetch: file read failed\n");
        (void)fclose(file);
    }

    file = fopen("/proc/cpuinfo", "rb");
    if (file)
    {
        size_t count = fread(buffer, 1, sizeof(buffer) - 1, file);
        buffer[count] = '\0';
        char *line = buffer;
        static const char key[] = "vendor_id";
        while (*line)
        {
            if (strncmp(line, key, sizeof(key) - 1) == 0)
            {
                line += sizeof(key) - 1;
                while (*line == ' ' || *line == '\t')
                    line++;
                if (*line == ':')
                    line++;
                while (*line == ' ' || *line == '\t')
                    line++;
                char *end = line;
                while (*end && *end != '\n')
                    end++;
                *end = '\0';
                put_text("CPU: ");
                put_text(line);
                put_text("\n");
                break;
            }
            while (*line && *line != '\n')
                line++;
            if (*line)
                line++;
        }
        (void)fclose(file);
    }

    file = fopen("/proc/meminfo", "rb");
    if (file)
    {
        size_t count = fread(buffer, 1, sizeof(buffer) - 1, file);
        buffer[count] = '\0';
        const char *key = "MemTotal:";
        char *line = buffer;
        while (*line)
        {
            if (strncmp(line, key, strlen(key)) == 0)
            {
                line += strlen(key);
                while (*line == ' ' || *line == '\t')
                    line++;
                put_text("Memory: ");
                while (*line >= '0' && *line <= '9')
                    (void)write(STDOUT_FILENO, line++, 1);
                put_text(" kB installed\n");
                break;
            }
            while (*line && *line != '\n')
                line++;
            if (*line)
                line++;
        }
        (void)fclose(file);
    }
    return 0;
}
#endif

#if defined(APP_IFCONFIG)
static void put_mac(const uint8_t address[6])
{
    static const char digits[] = "0123456789abcdef";
    for (uint32_t i = 0; i < 6; i++)
    {
        char octet[2] = {
            digits[address[i] >> 4],
            digits[address[i] & 0x0F]
        };
        (void)write(STDOUT_FILENO, octet, sizeof(octet));
        if (i < 5)
            put_text(":");
    }
}

static int command_ifconfig(void)
{
    os_network_info_t info;
    if (os_network_info(&info) < 0)
    {
        put_text("ifconfig: unable to read network status\n");
        return 1;
    }
    put_text("eth0: ");
    put_text(info.link_up ? "link up" : "link down");
    put_text(info.configured ? ", configured\n" : ", awaiting DHCP\n");
    put_text("  ether ");
    put_mac(info.mac);
    put_text("  inet ");
    put_ipv4(info.address);
    put_text("\n  netmask ");
    put_ipv4(info.netmask);
    put_text("\n  gateway ");
    put_ipv4(info.gateway);
    put_text("\n  dns ");
    put_ipv4(info.dns[0]);
    if (info.dns[1][0] || info.dns[1][1] ||
        info.dns[1][2] || info.dns[1][3])
    {
        put_text(", ");
        put_ipv4(info.dns[1]);
    }
    put_text("\n");
    return 0;
}
#endif

#if defined(APP_RESOLVE)
static int command_resolve(const char *hostname)
{
    os_network_resolve_t request = {0};
    size_t length = strlen(hostname);
    if (!length || length >= sizeof(request.hostname))
    {
        put_text("Usage: resolve hostname\n");
        return 2;
    }
    memcpy(request.hostname, hostname, length + 1);
    if (os_network_resolve(&request) < 0)
    {
        put_text("resolve: lookup failed (check network and /etc/resolv.conf)\n");
        return 1;
    }
    put_text(hostname);
    put_text(" ");
    put_ipv4(request.address);
    put_text("\n");
    return 0;
}
#endif

#if defined(APP_PING)
static int command_ping(const char *address_text)
{
    os_network_ping_t request = {0};
    const char *cursor = address_text;
    for (uint32_t octet = 0; octet < 4; octet++)
    {
        if (*cursor < '0' || *cursor > '9')
            goto invalid_address;
        uint32_t value = 0;
        uint32_t digits = 0;
        while (*cursor >= '0' && *cursor <= '9')
        {
            value = value * 10 + (uint32_t)(*cursor - '0');
            if (value > 255 || ++digits > 3)
                goto invalid_address;
            cursor++;
        }
        request.address[octet] = (uint8_t)value;
        if (octet < 3)
        {
            if (*cursor++ != '.')
                goto invalid_address;
        }
        else if (*cursor)
        {
            goto invalid_address;
        }
    }
    if (os_network_ping(&request) < 0)
    {
        put_text("ping: no reply (network unavailable or timed out)\n");
        return 1;
    }
    put_text("Reply from ");
    put_ipv4(request.address);
    put_text(": time=");
    put_number(request.round_trip_ms);
    put_text(" ms\n");
    return 0;

invalid_address:
    put_text("Usage: ping IPv4_ADDRESS\n");
    return 2;
}
#endif

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
#if defined(APP_LS)
    return command_ls(argc > 1 ? argv[1] : "/");
#elif defined(APP_CAT)
    if (argc < 2)
    {
        put_text("Usage: cat path\n");
        return 2;
    }
    return command_cat(argv[1]);
#elif defined(APP_MKDIR)
    if (argc < 2)
    {
        put_text("Usage: mkdir path\n");
        return 2;
    }
    if (os_mkdir(argv[1]) < 0)
    {
        put_text("mkdir: could not create directory\n");
        return 1;
    }
    return 0;
#elif defined(APP_RMDIR)
    if (argc < 2)
    {
        put_text("Usage: rmdir path\n");
        return 2;
    }
    if (os_rmdir(argv[1]) < 0)
    {
        put_text("rmdir: cannot remove directory\n");
        return 1;
    }
    return 0;
#elif defined(APP_CD)
    if (argc < 2 || os_isdir(argv[1]) <= 0)
    {
        put_text("cd: no such directory\n");
        return 1;
    }
    return 0;
#elif defined(APP_NEOFETCH)
    return command_neofetch();
#elif defined(APP_LSBLK)
    return command_lsblk();
#elif defined(APP_INSTALL)
    (void)argc;
    (void)argv;
    return command_install();
#elif defined(APP_IFCONFIG)
    return command_ifconfig();
#elif defined(APP_RESOLVE)
    if (argc < 2)
    {
        put_text("Usage: resolve hostname\n");
        return 2;
    }
    return command_resolve(argv[1]);
#elif defined(APP_PING)
    if (argc < 2)
    {
        put_text("Usage: ping IPv4_ADDRESS\n");
        return 2;
    }
    return command_ping(argv[1]);
#else
    (void)argc;
    (void)argv;
    return 127;
#endif
}
