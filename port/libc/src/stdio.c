#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "errno.h"
#include "assert.h"
#include "fcntl.h"
#include "stdlib.h"
#include "stdio.h"
#include "string.h"
#include "unistd.h"

static FILE standard_input = {
    .descriptor = STDIN_FILENO, .error = 0, .eof = 0
};
static FILE standard_output = {
    .descriptor = STDOUT_FILENO, .error = 0, .eof = 0
};
static FILE standard_error = {
    .descriptor = STDERR_FILENO, .error = 0, .eof = 0
};

FILE *stdin = &standard_input;
FILE *stdout = &standard_output;
FILE *stderr = &standard_error;

typedef struct
{
    char *buffer;
    size_t capacity;
    size_t length;
} output_t;

static void output_char(output_t *output, char character)
{
    if (output->capacity && output->length + 1 < output->capacity)
        output->buffer[output->length] = character;
    output->length++;
}

static void output_string(output_t *output, const char *string, size_t size)
{
    for (size_t i = 0; i < size; i++)
        output_char(output, string[i]);
}

static void output_unsigned(
    output_t *output,
    uint64_t value,
    unsigned base,
    bool uppercase,
    int width,
    char padding,
    int precision
)
{
    char digits[1024];
    const char *alphabet =
        uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    size_t count = 0;

    do
    {
        digits[count++] = alphabet[value % base];
        value /= base;
    } while (value && count < sizeof(digits));

    while ((int)count < precision && count < sizeof(digits))
        digits[count++] = '0';
    while ((int)count < width && count < sizeof(digits))
        digits[count++] = padding;
    while (count)
        output_char(output, digits[--count]);
}

static int format_output(
    output_t *output,
    const char *format,
    va_list arguments
)
{
    while (*format)
    {
        if (*format != '%')
        {
            output_char(output, *format++);
            continue;
        }
        format++;

        char padding = ' ';
        if (*format == '0')
        {
            padding = '0';
            format++;
        }

        int width = 0;
        if (*format == '*')
        {
            width = va_arg(arguments, int);
            format++;
            if (width < 0)
            {
                width = -width;
                padding = ' ';
            }
        }
        else
        {
            while (*format >= '0' && *format <= '9')
            {
                width = width * 10 + (*format++ - '0');
                if (width > 1024)
                    width = 1024;
            }
        }

        int precision = -1;
        if (*format == '.')
        {
            format++;
            precision = 0;
            if (*format == '*')
            {
                precision = va_arg(arguments, int);
                format++;
                if (precision < 0)
                    precision = -1;
            }
            else
            {
                while (*format >= '0' && *format <= '9')
                {
                    if (precision < 1024)
                        precision = precision * 10 + (*format - '0');
                    format++;
                }
            }
        }

        enum
        {
            LENGTH_DEFAULT,
            LENGTH_LONG,
            LENGTH_LONG_LONG,
            LENGTH_SIZE,
            LENGTH_INTMAX
        } length = LENGTH_DEFAULT;
        if (*format == 'l')
        {
            length = LENGTH_LONG;
            format++;
            if (*format == 'l')
            {
                length = LENGTH_LONG_LONG;
                format++;
            }
        }
        else if (*format == 'z' || *format == 't')
        {
            length = LENGTH_SIZE;
            format++;
        }
        else if (*format == 'j')
        {
            length = LENGTH_INTMAX;
            format++;
        }
        else if (*format == 'h')
        {
            format++;
            if (*format == 'h')
                format++;
        }

        switch (*format)
        {
            case 's':
            {
                const char *string = va_arg(arguments, const char *);
                if (!string)
                    string = "(null)";
                size_t size = precision >= 0
                    ? strnlen(string, (size_t)precision)
                    : strlen(string);
                if (width > (int)size)
                {
                    for (int i = 0; i < width - (int)size; i++)
                        output_char(output, ' ');
                }
                output_string(output, string, size);
                break;
            }
            case 'c':
                output_char(output, (char)va_arg(arguments, int));
                break;
            case 'd':
            case 'i':
            {
                int64_t value;
                if (length == LENGTH_LONG_LONG)
                    value = va_arg(arguments, long long);
                else if (length == LENGTH_LONG)
                    value = va_arg(arguments, long);
                else if (length == LENGTH_SIZE)
                    value = va_arg(arguments, ssize_t);
                else if (length == LENGTH_INTMAX)
                    value = va_arg(arguments, intmax_t);
                else
                    value = va_arg(arguments, int);
                uint64_t magnitude;
                if (value < 0)
                {
                    output_char(output, '-');
                    magnitude = (uint64_t)(-(value + 1)) + 1;
                }
                else
                    magnitude = (uint64_t)value;
                output_unsigned(
                    output,
                    magnitude,
                    10,
                    false,
                    width,
                    precision >= 0 ? ' ' : padding,
                    precision
                );
                break;
            }
            case 'u':
            case 'o':
            case 'x':
            case 'X':
            case 'p':
            {
                uint64_t value;
                if (*format == 'p')
                {
                    value = (uintptr_t)va_arg(arguments, void *);
                    output_string(output, "0x", 2);
                }
                else if (length == LENGTH_LONG_LONG)
                    value = va_arg(arguments, unsigned long long);
                else if (length == LENGTH_LONG)
                    value = va_arg(arguments, unsigned long);
                else if (length == LENGTH_SIZE)
                    value = va_arg(arguments, size_t);
                else if (length == LENGTH_INTMAX)
                    value = va_arg(arguments, uintmax_t);
                else
                    value = va_arg(arguments, unsigned int);
                unsigned base = *format == 'o' ? 8 :
                    ((*format == 'x' || *format == 'X' ||
                      *format == 'p') ? 16 : 10);
                output_unsigned(
                    output,
                    value,
                    base,
                    *format == 'X',
                    width,
                    precision >= 0 ? ' ' : padding,
                    precision
                );
                break;
            }
            case '%':
                output_char(output, '%');
                break;
            case '\0':
                format--;
                break;
            default:
                output_char(output, '%');
                output_char(output, *format);
                break;
        }
        if (*format)
            format++;
    }
    if (output->length > INT_MAX)
    {
        errno = EOVERFLOW;
        return -1;
    }
    return (int)output->length;
}

int printf(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    int length = vfprintf(stdout, format, arguments);
    va_end(arguments);
    return length;
}

int fprintf(FILE *stream, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    int result = vfprintf(stream, format, arguments);
    va_end(arguments);
    return result;
}

int vprintf(const char *format, va_list arguments)
{
    return vfprintf(stdout, format, arguments);
}

int vfprintf(FILE *stream, const char *format, va_list arguments)
{
    if (!stream || !format)
    {
        errno = EINVAL;
        return -1;
    }

    va_list count_arguments;
    va_copy(count_arguments, arguments);
    int length = format_output(
        &(output_t){ .buffer = 0, .capacity = 0, .length = 0 },
        format,
        count_arguments
    );
    va_end(count_arguments);
    if (length < 0)
        return -1;

    char *buffer = malloc((size_t)length + 1);
    if (!buffer)
        return -1;

    va_list output_arguments;
    va_copy(output_arguments, arguments);
    output_t output = {
        .buffer = buffer,
        .capacity = (size_t)length + 1,
        .length = 0
    };
    format_output(&output, format, output_arguments);
    va_end(output_arguments);

    size_t written = 0;
    while (written < output.length)
    {
        ssize_t result = write(
            stream->descriptor,
            buffer + written,
            output.length - written
        );
        if (result <= 0)
        {
            stream->error = 1;
            free(buffer);
            return -1;
        }
        written += (size_t)result;
    }

    free(buffer);
    return length;
}

int snprintf(char *buffer, size_t size, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    int result = vsnprintf(buffer, size, format, arguments);
    va_end(arguments);
    return result;
}

int vsnprintf(
    char *buffer,
    size_t size,
    const char *format,
    va_list arguments
)
{
    if (!format || (!buffer && size))
    {
        errno = EINVAL;
        return -1;
    }

    output_t output = {
        .buffer = buffer,
        .capacity = size,
        .length = 0
    };

    va_list copy;
    va_copy(copy, arguments);
    int length = format_output(&output, format, copy);
    va_end(copy);

    if (size)
    {
        size_t terminator = output.length < size
            ? output.length
            : size - 1;
        buffer[terminator] = '\0';
    }
    return length;
}

int puts(const char *string)
{
    if (fputs(string, stdout) < 0 || putchar('\n') < 0)
        return -1;
    return (int)(strlen(string) + 1);
}

void perror(const char *prefix)
{
    if (prefix && *prefix)
        fprintf(stderr, "%s: %s\n", prefix, strerror(errno));
    else
        fprintf(stderr, "%s\n", strerror(errno));
}

void __assert_fail(
    const char *expression,
    const char *file,
    unsigned line,
    const char *function
)
{
    fprintf(
        stderr,
        "assertion failed: %s (%s:%u, %s)\n",
        expression,
        file,
        line,
        function
    );
    abort();
}

int fputs(const char *string, FILE *stream)
{
    if (!string || !stream)
    {
        errno = EINVAL;
        return -1;
    }
    size_t length = strlen(string);
    size_t written = 0;
    while (written < length)
    {
        ssize_t result = write(
            stream->descriptor,
            string + written,
            length - written
        );
        if (result <= 0)
        {
            stream->error = 1;
            return -1;
        }
        written += (size_t)result;
    }
    return 0;
}

int putchar(int character)
{
    char value = (char)character;
    return write(STDOUT_FILENO, &value, 1) < 0 ? -1 : (unsigned char)value;
}

int getchar(void)
{
    return fgetc(stdin);
}

FILE *fopen(const char *path, const char *mode)
{
    if (!path || !mode)
    {
        errno = EINVAL;
        return 0;
    }

    int flags;
    if ((mode[0] == 'r' || mode[0] == 'w' || mode[0] == 'a') &&
        (mode[1] == '\0' ||
         (mode[1] == 'b' && mode[2] == '\0')))
    {
        flags = mode[0] == 'r'
            ? O_RDONLY
            : mode[0] == 'w'
                ? O_WRONLY | O_CREAT | O_TRUNC
                : O_WRONLY | O_CREAT | O_APPEND;
    }
    else
    {
        errno = EINVAL;
        return 0;
    }

    int descriptor = open(path, flags, 0666);
    if (descriptor < 0)
        return 0;

    FILE *stream = malloc(sizeof(*stream));
    if (!stream)
    {
        close(descriptor);
        return 0;
    }
    *stream = (FILE){ .descriptor = descriptor, .error = 0, .eof = 0 };
    return stream;
}

int fclose(FILE *stream)
{
    if (!stream)
    {
        errno = EINVAL;
        return -1;
    }
    if (stream == stdin || stream == stdout || stream == stderr)
        return 0;

    int result = close(stream->descriptor);
    free(stream);
    return result;
}

size_t fread(void *buffer, size_t size, size_t count, FILE *stream)
{
    if (!stream || (!buffer && size && count))
    {
        errno = EINVAL;
        return 0;
    }
    if (!size || !count)
        return 0;
    if (count > SIZE_MAX / size)
    {
        errno = EOVERFLOW;
        stream->error = 1;
        return 0;
    }

    size_t total = size * count;
    size_t received = 0;
    while (received < total)
    {
        size_t amount = total - received;
        if (amount > 65536)
            amount = 65536;
        ssize_t result = read(
            stream->descriptor,
            (unsigned char *)buffer + received,
            amount
        );
        if (result < 0)
        {
            stream->error = 1;
            break;
        }
        if (result == 0)
        {
            stream->eof = 1;
            break;
        }
        received += (size_t)result;
        if ((size_t)result < amount)
        {
            stream->eof = 1;
            break;
        }
    }
    return received / size;
}

size_t fwrite(
    const void *buffer,
    size_t size,
    size_t count,
    FILE *stream
)
{
    if (!stream || (!buffer && size && count))
    {
        errno = EINVAL;
        return 0;
    }
    if (!size || !count)
        return 0;
    if (count > SIZE_MAX / size)
    {
        errno = EOVERFLOW;
        stream->error = 1;
        return 0;
    }

    size_t total = size * count;
    size_t written = 0;
    while (written < total)
    {
        size_t amount = total - written;
        if (amount > 65536)
            amount = 65536;
        ssize_t result = write(
            stream->descriptor,
            (const unsigned char *)buffer + written,
            amount
        );
        if (result <= 0)
        {
            stream->error = 1;
            break;
        }
        written += (size_t)result;
    }
    return written / size;
}

int fseek(FILE *stream, long offset, int whence)
{
    if (!stream)
    {
        errno = EINVAL;
        return -1;
    }
    if (lseek(stream->descriptor, offset, whence) < 0)
    {
        stream->error = 1;
        return -1;
    }
    stream->eof = 0;
    return 0;
}

long ftell(FILE *stream)
{
    if (!stream)
    {
        errno = EINVAL;
        return -1;
    }
    off_t offset = lseek(stream->descriptor, 0, SEEK_CUR);
    return offset < 0 ? -1 : (long)offset;
}

void rewind(FILE *stream)
{
    if (fseek(stream, 0, SEEK_SET) == 0)
        clearerr(stream);
}

int feof(FILE *stream)
{
    return stream ? stream->eof : 0;
}

int ferror(FILE *stream)
{
    return stream ? stream->error : 0;
}

void clearerr(FILE *stream)
{
    if (stream)
    {
        stream->eof = 0;
        stream->error = 0;
    }
}

int fflush(FILE *stream)
{
    if (stream && stream->error)
        return -1;
    return 0;
}

int fgetc(FILE *stream)
{
    unsigned char character;
    if (!stream)
    {
        errno = EINVAL;
        return -1;
    }
    ssize_t result = read(stream->descriptor, &character, 1);
    if (result == 1)
        return character;
    if (result == 0)
        stream->eof = 1;
    else
        stream->error = 1;
    return -1;
}

int fputc(int character, FILE *stream)
{
    unsigned char value = (unsigned char)character;
    if (!stream)
    {
        errno = EINVAL;
        return -1;
    }
    if (write(stream->descriptor, &value, 1) != 1)
    {
        stream->error = 1;
        return -1;
    }
    return value;
}

char *fgets(char *buffer, int size, FILE *stream)
{
    if (!buffer || size <= 0 || !stream)
    {
        errno = EINVAL;
        return 0;
    }
    int count = 0;
    while (count + 1 < size)
    {
        int character = fgetc(stream);
        if (character < 0)
            break;
        buffer[count++] = (char)character;
        if (character == '\n')
            break;
    }
    if (!count)
        return 0;
    buffer[count] = '\0';
    return buffer;
}
