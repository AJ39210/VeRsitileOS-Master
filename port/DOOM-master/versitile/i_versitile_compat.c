#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fcntl.h"
#include <os/gui.h>

char *getenv(const char *name)
{
    if (!name)
        return NULL;
    if (strcmp(name, "HOME") == 0)
        return (char *)"/tmp";
    if (strcmp(name, "DOOMWADDIR") == 0)
        return (char *)"/usr/share/doom";
    return NULL;
}

int access(const char *path, int mode)
{
    (void)mode;
    if (!path)
        return -1;
    int descriptor = open(path, O_RDONLY);
    if (descriptor < 0)
        return -1;
    close(descriptor);
    return 0;
}

int sprintf(char *buffer, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    int result = vsnprintf(buffer, (size_t)-1, format, arguments);
    va_end(arguments);
    return result;
}

static int is_space(char character)
{
    return character == ' ' || character == '\t' ||
        character == '\n' || character == '\r' ||
        character == '\f' || character == '\v';
}

static int scan_input(const char *input, const char *format, va_list arguments)
{
    const char *source = input;
    int assigned = 0;

    while (*format)
    {
        if (is_space(*format))
        {
            while (is_space(*format))
                format++;
            while (is_space(*source))
                source++;
            continue;
        }
        if (*format != '%')
        {
            if (*source != *format)
                break;
            source++;
            format++;
            continue;
        }

        format++;
        if (*format == '%')
        {
            if (*source != '%')
                break;
            source++;
            format++;
            continue;
        }

        int width = 0;
        while (*format >= '0' && *format <= '9')
        {
            width = width * 10 + (*format++ - '0');
            if (width > 1024)
                width = 1024;
        }
        char conversion = *format++;
        if (!width)
            width = conversion == '[' ? 99 : 255;
        while (conversion != 'c' && is_space(*source))
            source++;
        if (!*source)
            break;

        if (conversion == 's')
        {
            char *destination = va_arg(arguments, char *);
            int count = 0;
            while (*source && !is_space(*source) && count < width)
                destination[count++] = *source++;
            if (!count)
                break;
            destination[count] = '\0';
            assigned++;
        }
        else if (conversion == '[')
        {
            int invert = *format == '^';
            if (invert)
                format++;
            char set[128];
            size_t set_length = 0;
            if (*format == ']')
                set[set_length++] = *format++;
            while (*format && *format != ']' &&
                   set_length < sizeof(set) - 1)
                set[set_length++] = *format++;
            if (*format != ']')
                break;
            format++;

            char *destination = va_arg(arguments, char *);
            int count = 0;
            while (*source && count < width)
            {
                int matches = 0;
                for (size_t i = 0; i < set_length; i++)
                {
                    if (set[i] == *source)
                        matches = 1;
                    if (i + 2 < set_length && set[i + 1] == '-' &&
                        *source >= set[i] && *source <= set[i + 2])
                    {
                        matches = 1;
                        i += 2;
                    }
                }
                if (matches == invert)
                    break;
                destination[count++] = *source++;
            }
            if (!count)
                break;
            destination[count] = '\0';
            assigned++;
        }
        else if (conversion == 'c')
        {
            char *destination = va_arg(arguments, char *);
            int count = width == 255 ? 1 : width;
            for (int i = 0; i < count; i++)
            {
                if (!source[i])
                    return assigned;
                destination[i] = source[i];
            }
            source += count;
            assigned++;
        }
        else if (conversion == 'd' || conversion == 'i' ||
                 conversion == 'u' || conversion == 'x')
        {
            char *end;
            int base = conversion == 'x' ? 16 :
                (conversion == 'i' ? 0 : 10);
            long value = strtol(source, &end, base);
            if (end == source)
                break;
            *va_arg(arguments, int *) = (int)value;
            source = end;
            assigned++;
        }
        else
            break;
    }

    return assigned;
}

int sscanf(const char *input, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    int result = scan_input(input, format, arguments);
    va_end(arguments);
    return result;
}

int fscanf(FILE *stream, const char *format, ...)
{
    char line[256];
    size_t length = 0;
    int character;
    while ((character = fgetc(stream)) != EOF && character != '\n')
    {
        if (length + 1 < sizeof(line))
            line[length++] = (char)character;
    }
    if (character == EOF && !length)
        return EOF;
    line[length] = '\0';

    va_list arguments;
    va_start(arguments, format);
    int result = scan_input(line, format, arguments);
    va_end(arguments);
    return result;
}

int fstat(int descriptor, struct stat *status)
{
    if (!status)
        return -1;
    off_t current = lseek(descriptor, 0, SEEK_CUR);
    if (current < 0)
        return -1;
    off_t end = lseek(descriptor, 0, SEEK_END);
    if (end < 0 || lseek(descriptor, current, SEEK_SET) < 0)
        return -1;
    status->st_size = end;
    return 0;
}

int mkdir(const char *path, mode_t mode)
{
    (void)mode;
    return os_mkdir(path) < 0 ? -1 : 0;
}

void setbuf(FILE *stream, char *buffer)
{
    (void)stream;
    (void)buffer;
}
