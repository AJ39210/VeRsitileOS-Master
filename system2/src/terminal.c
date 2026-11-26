#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <os/gui.h>

#define INPUT_SIZE 128
#define PATH_BUFFER_SIZE 256

static char current_directory[PATH_BUFFER_SIZE] = "/";

static void output_char(char character)
{
    (void)write(STDOUT_FILENO, &character, 1);
}

static void output_text(const char *text)
{
    if (text)
        (void)write(STDOUT_FILENO, text, strlen(text));
}

static bool make_path(
    const char *input_path,
    char output[PATH_BUFFER_SIZE])
{
    if (!input_path || !*input_path)
        return false;

    size_t length = 0;
    output[length++] = '/';

    if (*input_path != '/')
    {
        const char *base = current_directory;
        if (*base == '/')
            base++;
        while (*base)
        {
            if (length >= PATH_BUFFER_SIZE - 1)
                return false;
            output[length++] = *base++;
        }
        if (length > 1 && output[length - 1] != '/')
        {
            if (length >= PATH_BUFFER_SIZE - 1)
                return false;
            output[length++] = '/';
        }
    }

    const char *cursor = input_path;
    while (*cursor == '/')
        cursor++;
    while (*cursor)
    {
        const char *start = cursor;
        while (*cursor && *cursor != '/')
            cursor++;
        size_t component_length = (size_t)(cursor - start);
        while (*cursor == '/')
            cursor++;

        if (component_length == 1 && start[0] == '.')
            continue;
        if (component_length == 2 && start[0] == '.' &&
            start[1] == '.')
        {
            if (length > 1)
            {
                if (output[length - 1] == '/')
                    length--;
                while (length > 1 && output[length - 1] != '/')
                    length--;
                if (length > 1 && output[length - 1] == '/')
                    length--;
            }
            continue;
        }

        if (!component_length)
            continue;
        if (length > 1 && output[length - 1] != '/')
            output[length++] = '/';
        if (component_length >= PATH_BUFFER_SIZE - length)
            return false;
        memcpy(output + length, start, component_length);
        length += component_length;
    }

    if (length > 1 && output[length - 1] == '/')
        length--;
    output[length] = '\0';
    return true;
}

static void run_external_path(
    const char *executable,
    const char *display_name,
    const char *argument)
{
    int64_t pid = os_spawn_elf_command(executable, argument);
    if (pid < 0)
    {
        output_text(display_name);
        output_text(": could not start (");
        output_text(executable);
        output_text(", error ");
        uint64_t error = (uint64_t)(-pid);
        char digits[21];
        uint32_t count = 0;
        do
        {
            digits[count++] = (char)('0' + error % 10);
            error /= 10;
        } while (error && count < sizeof(digits));
        while (count)
            output_char(digits[--count]);
        output_text(")\n");
        return;
    }

    char buffer[256];
    while (os_task_active((uint64_t)pid) > 0)
    {
        int64_t count = os_read_child_output(buffer, sizeof(buffer));
        if (count > 0)
            (void)write(STDOUT_FILENO, buffer, (size_t)count);
        yield();
    }
    for (;;)
    {
        int64_t count = os_read_child_output(buffer, sizeof(buffer));
        if (count <= 0)
            break;
        (void)write(STDOUT_FILENO, buffer, (size_t)count);
    }
    if (strcmp(display_name, "doom") == 0 &&
        os_console_clear() < 0)
        output_text("doom: could not clear the screen after exit\n");
}

static void run_external_command(const char *name, const char *argument)
{
    char executable[64] = "/bin/";
    size_t name_length = strlen(name);
    if (name_length >= sizeof(executable) - sizeof("/bin/"))
    {
        output_text("command name too long\n");
        return;
    }
    memcpy(executable + sizeof("/bin/") - 1, name, name_length + 1);
    run_external_path(executable, name, argument);
}

static bool path_is_current_or_parent(const char *path)
{
    size_t length = strlen(path);
    return strncmp(current_directory, path, length) == 0 &&
        (current_directory[length] == '\0' || path[length - 1] == '/' ||
         current_directory[length] == '/');
}

static void execute_command(char *line, bool *should_exit)
{
    char *command = line;
    while (*command == ' ' || *command == '\t')
        command++;
    if (!*command)
        return;

    char *argument = command;
    while (*argument && *argument != ' ' && *argument != '\t')
        argument++;
    if (*argument)
    {
        *argument++ = '\0';
        while (*argument == ' ' || *argument == '\t')
            argument++;
    }

    if (command[0] == '/' ||
        (command[0] == '.' && command[1] == '/'))
    {
        char executable[PATH_BUFFER_SIZE];
        if (!make_path(command, executable))
        {
            output_text("terminal: invalid executable path\n");
            return;
        }
        run_external_path(executable, command, argument);
    }
    else if (strcmp(command, "help") == 0)
    {
        output_text("Commands: ls [path], cd [path], mkdir path, rmdir path,\n");
        output_text("          cat path, neofetch, ifconfig, ping IPv4, resolve host,\n");
        output_text("          lsblk, install, setup, kilo [file], doom [-iwad path],\n");
        output_text("          vcc source.c [-o program],\n");
        output_text("          ./program, clear, exit\n");
    }
    else if (strcmp(command, "clear") == 0)
    {
        if (os_console_clear() < 0)
            output_text("clear: could not clear the console\n");
    }
    else if (strcmp(command, "exit") == 0)
    {
        *should_exit = true;
    }
    else if (strcmp(command, "cd") == 0)
    {
        char path[PATH_BUFFER_SIZE];
        if (!*argument)
            memcpy(path, "/", 2);
        else if (!make_path(argument, path))
        {
            output_text("cd: invalid path\n");
            return;
        }
        if (os_isdir(path) <= 0)
        {
            output_text("cd: no such directory\n");
            return;
        }
        run_external_command("cd", path);
        memcpy(current_directory, path, strlen(path) + 1);
    }
    else if (strcmp(command, "ls") == 0 ||
             strcmp(command, "cat") == 0 ||
             strcmp(command, "mkdir") == 0 ||
             strcmp(command, "rmdir") == 0)
    {
        char path[PATH_BUFFER_SIZE];
        if (strcmp(command, "ls") == 0 && !*argument)
        {
            memcpy(path, current_directory,
                strlen(current_directory) + 1);
        }
        else if (!*argument || !make_path(argument, path))
        {
            output_text(command);
            output_text(": usage requires a valid path\n");
            return;
        }
        if (strcmp(command, "rmdir") == 0 &&
            path_is_current_or_parent(path))
        {
            output_text("rmdir: cannot remove the current directory\n");
            return;
        }
        run_external_command(command, path);
    }
    else if (strcmp(command, "kilo") == 0)
    {
        if (*argument && argument[0] != '-' && argument[0] != '/')
        {
            char path[PATH_BUFFER_SIZE];
            if (!make_path(argument, path))
            {
                output_text("kilo: invalid file path\n");
                return;
            }
            run_external_command(command, path);
        }
        else
        {
            run_external_command(command, argument);
        }
    }
    else if (strcmp(command, "vcc") == 0)
    {
        char expanded_argument[256];
        size_t used = 0;
        char *cursor = argument;
        bool output_path_next = false;
        while (*cursor)
        {
            while (*cursor == ' ' || *cursor == '\t')
                cursor++;
            if (!*cursor)
                break;
            char *token = cursor;
            while (*cursor && *cursor != ' ' && *cursor != '\t')
                cursor++;
            if (*cursor)
                *cursor++ = '\0';

            const char *value = token;
            char path[PATH_BUFFER_SIZE];
            if (output_path_next ||
                (token[0] != '-' && strcmp(token, "--help") != 0 &&
                 strcmp(token, "-h") != 0))
            {
                if (!make_path(token, path))
                {
                    output_text("vcc: invalid source or output path\n");
                    return;
                }
                value = path;
                output_path_next = false;
            }
            else if (strcmp(token, "-o") == 0)
            {
                output_path_next = true;
            }

            size_t value_length = strlen(value);
            if (used + value_length + (used ? 1 : 0) >=
                sizeof(expanded_argument))
            {
                output_text("vcc: arguments are too long\n");
                return;
            }
            if (used)
                expanded_argument[used++] = ' ';
            memcpy(expanded_argument + used, value, value_length);
            used += value_length;
        }
        if (output_path_next)
        {
            output_text("vcc: -o requires an output path\n");
            return;
        }
        expanded_argument[used] = '\0';
        run_external_command(command, expanded_argument);
    }
    else if (strcmp(command, "neofetch") == 0 ||
             strcmp(command, "ifconfig") == 0 ||
             strcmp(command, "ping") == 0 ||
             strcmp(command, "resolve") == 0 ||
             strcmp(command, "lsblk") == 0 ||
             strcmp(command, "install") == 0 ||
             strcmp(command, "setup") == 0 ||
             strcmp(command, "doom") == 0)
    {
        run_external_command(command, argument);
    }
    else
    {
        output_text(command);
        output_text(": command not found (try 'help')\n");
    }
}

int main(void)
{
    output_text("VeRsitileOS Terminal - type 'help' for commands\n");

    bool should_exit = false;
    while (!should_exit)
    {
        output_text(current_directory);
        output_text(" $ ");

        uint32_t length = 0;
        char line[INPUT_SIZE];
        output_char('_');
        for (;;)
        {
            char character;
            int64_t result = os_read(STDIN_FILENO, &character, 1);
            if (result == 1)
            {
                output_char('\b');
                if (character == '\r' || character == '\n')
                {
                    output_char('\n');
                    break;
                }
                if (character == '\b' || character == 127)
                {
                    if (length)
                    {
                        length--;
                        output_text("\b \b");
                    }
                    output_char('_');
                    continue;
                }
                if (character >= 32 && character <= 126 &&
                    length + 1 < sizeof(line))
                {
                    line[length++] = character;
                    output_char(character);
                    output_char('_');
                    continue;
                }
                output_char('_');
            }
            else if (result < 0 && result != -EAGAIN)
            {
                output_char('\b');
                output_text("\nterminal: keyboard input failed\n");
                return 1;
            }
            yield();
        }

        line[length] = '\0';
        execute_command(line, &should_exit);
    }
    return 0;
}
