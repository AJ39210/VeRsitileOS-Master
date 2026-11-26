#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <os/gui.h>

#define USERNAME_SIZE 32
#define HOSTNAME_SIZE 64
#define DOMAIN_SIZE 128
#define DESKTOP_SIZE 256

static void write_text(const char *text)
{
    if (text)
        (void)write(STDOUT_FILENO, text, strlen(text));
}

static bool read_value(
    const char *prompt,
    char *value,
    size_t capacity
)
{
    write_text(prompt);
    if (!value || capacity < 2)
        return false;

    size_t length = 0;
    size_t overflow_count = 0;
    for (;;)
    {
        char character;
        ssize_t result = read(STDIN_FILENO, &character, 1);
        if (result < 0 && errno == EAGAIN)
        {
            yield();
            continue;
        }
        if (result <= 0)
            return false;
        if (character == '\r' || character == '\n')
        {
            write_text("\n");
            break;
        }
        if (character == '\b' || character == 127)
        {
            if (overflow_count)
                overflow_count--;
            else if (length)
                value[--length] = '\0';
            else
                continue;
            write_text("\b \b");
            continue;
        }
        if (character < 32 || character > 126)
            continue;
        if (!overflow_count && length + 1 < capacity)
        {
            value[length++] = character;
            value[length] = '\0';
        }
        else
        {
            if (overflow_count != (size_t)-1)
                overflow_count++;
        }
        (void)write(STDOUT_FILENO, &character, 1);
    }

    value[length] = '\0';
    return !overflow_count && length > 0;
}

static bool valid_username(const char *value)
{
    if (!*value)
        return false;
    for (size_t i = 0; value[i]; i++)
    {
        char character = value[i];
        if (!((character >= 'a' && character <= 'z') ||
              (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') ||
              character == '_' || character == '-'))
            return false;
    }
    return true;
}

static bool valid_hostname(const char *value)
{
    size_t length = strlen(value);
    if (!length || length > 63 ||
        value[0] == '-' || value[length - 1] == '-')
        return false;
    for (size_t i = 0; i < length; i++)
    {
        char character = value[i];
        if (!((character >= 'a' && character <= 'z') ||
              (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') ||
              character == '-'))
            return false;
    }
    return true;
}

static bool normalize_domain(char domain[DOMAIN_SIZE])
{
    size_t length = strlen(domain);
    if (!length)
        return false;
    if (length < 4 ||
        memcmp(domain + length - 4, ".dom", 4) != 0)
    {
        if (length + 4 >= DOMAIN_SIZE)
            return false;
        memcpy(domain + length, ".dom", 5);
        length += 4;
    }
    if (length > 120 || domain[0] == '.' || domain[0] == '-' ||
        domain[length - 5] == '.' || domain[length - 5] == '-')
        return false;
    for (size_t i = 0; i < length - 4; i++)
    {
        char character = domain[i];
        if (!((character >= 'a' && character <= 'z') ||
              (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') ||
              character == '-' || character == '.'))
            return false;
        if (character == '.' &&
            (i == 0 || domain[i - 1] == '.' || domain[i - 1] == '-'))
            return false;
    }
    return true;
}

static bool detect_desktop(char desktop[DESKTOP_SIZE])
{
    os_directory_entry_t entries[8];
    os_directory_request_t request = {
        .capacity = sizeof(entries) / sizeof(entries[0])
    };
    memcpy(request.path, "/etc/xsession", sizeof("/etc/xsession"));
    for (;;)
    {
        int64_t count = os_readdir(&request, entries);
        if (count < 0)
            break;
        for (int64_t i = 0; i < count; i++)
        {
            if (entries[i].is_directory)
                continue;
            char path[DESKTOP_SIZE];
            int written = snprintf(
                path,
                sizeof(path),
                "/etc/xsession/%s",
                entries[i].name);
            if (written <= 0 || (size_t)written >= sizeof(path))
                continue;
            FILE *candidate = fopen(path, "rb");
            if (!candidate)
                continue;
            unsigned char magic[4];
            bool is_elf = fread(magic, 1, sizeof(magic), candidate) ==
                    sizeof(magic) &&
                magic[0] == 0x7f && magic[1] == 'E' &&
                magic[2] == 'L' && magic[3] == 'F';
            if (fclose(candidate) != 0)
                return false;
            if (is_elf)
            {
                memcpy(desktop, path, (size_t)written + 1);
                return true;
            }
        }
        request.offset += (uint64_t)count;
        if (count < (int64_t)(sizeof(entries) / sizeof(entries[0])))
            break;
    }
    memcpy(desktop, "Unknown / terminal",
        sizeof("Unknown / terminal"));
    return true;
}

static bool ensure_directory(const char *path)
{
    int64_t result = os_isdir(path);
    if (result == 1)
        return true;
    return os_mkdir(path) == 0;
}

static bool write_config(const char *path, const char *contents)
{
    FILE *file = fopen(path, "wb");
    if (!file)
        return false;
    size_t length = strlen(contents);
    bool success = fwrite(contents, 1, length, file) == length;
    if (fclose(file) != 0)
        success = false;
    return success;
}

static bool write_settings(
    const char *hostname,
    const char *username,
    const char *domain,
    const char *desktop
)
{
    char user_path[DESKTOP_SIZE];
    char user_contents[160];
    char home_path[DESKTOP_SIZE];
    int user_path_length = snprintf(
        user_path,
        sizeof(user_path),
        "/etc/system/users/%s",
        username);
    int home_path_length = snprintf(
        home_path,
        sizeof(home_path),
        "/home/%s",
        username);
    int user_contents_length = snprintf(
        user_contents,
        sizeof(user_contents),
        "instance_user=(%s) root?= n\nhome=%s\n",
        username,
        home_path);
    if (user_path_length <= 0 ||
        (size_t)user_path_length >= sizeof(user_path) ||
        home_path_length <= 0 ||
        (size_t)home_path_length >= sizeof(home_path) ||
        user_contents_length <= 0 ||
        (size_t)user_contents_length >= sizeof(user_contents) ||
        !ensure_directory("/etc") ||
        !ensure_directory("/etc/system") ||
        !ensure_directory("/etc/system/users") ||
        !ensure_directory("/home") ||
        !ensure_directory(home_path))
        return false;

    char line[DESKTOP_SIZE];
    int line_length = snprintf(line, sizeof(line), "%s\n", hostname);
    if (line_length <= 0 || (size_t)line_length >= sizeof(line) ||
        !write_config("/etc/system/host", line))
        return false;
    line_length = snprintf(line, sizeof(line), "%s\n", desktop);
    if (line_length <= 0 || (size_t)line_length >= sizeof(line) ||
        !write_config("/etc/system/desktop", line))
        return false;
    line_length = snprintf(line, sizeof(line), "%s\n", domain);
    if (line_length <= 0 || (size_t)line_length >= sizeof(line) ||
        !write_config("/etc/system/domain", line) ||
        !write_config(user_path, user_contents))
        return false;
    return true;
}

int main(void)
{
    char hostname[HOSTNAME_SIZE] = {0};
    char username[USERNAME_SIZE] = {0};
    char domain[DOMAIN_SIZE] = {0};
    char desktop[DESKTOP_SIZE] = {0};

    write_text("VeRsitileOS first-time setup\n");
    if (!read_value("Computer name: ", hostname, sizeof(hostname)) ||
        !valid_hostname(hostname))
    {
        write_text("setup: invalid hostname; use letters, numbers, and hyphens\n");
        return 1;
    }
    if (!read_value("Username: ", username, sizeof(username)) ||
        !valid_username(username))
    {
        write_text("setup: invalid username; use letters, numbers, _ or -\n");
        return 1;
    }
    if (!read_value("Local domain (.dom will be added if omitted): ",
                    domain, sizeof(domain)) ||
        !normalize_domain(domain))
    {
        write_text("setup: invalid domain name\n");
        return 1;
    }
    if (!detect_desktop(desktop))
    {
        write_text("setup: could not detect the desktop environment\n");
        return 1;
    }

    write_text("Please wait while we write everything to your operating system...\n");
    if (!write_settings(hostname, username, domain, desktop))
    {
        write_text("setup: failed to write system settings or create the home directory\n");
        return 1;
    }

    write_text("Setup finished. Desktop: ");
    write_text(desktop);
    write_text("\nWould you like to uninstall the setup command? [y/N]: ");
    char answer[8] = {0};
    if (!read_value("", answer, sizeof(answer)))
    {
        write_text("setup: setup is complete; command was left installed\n");
        return 0;
    }
    if (answer[0] == 'y' || answer[0] == 'Y')
    {
        if (unlink("/bin/setup") != 0)
        {
            write_text("setup: could not remove /bin/setup\n");
            return 1;
        }
        write_text("Setup complete. /bin/setup has been removed.\n");
        return 0;
    }
    write_text("Setup complete. You can run setup again to change these settings.\n");
    return 0;
}
