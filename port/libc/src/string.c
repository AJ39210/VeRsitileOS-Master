#include <stdbool.h>
#include <stdint.h>

#include "errno.h"
#include "stdlib.h"
#include "string.h"

void *memcpy(void *destination, const void *source, size_t size)
{
    unsigned char *dst = destination;
    const unsigned char *src = source;
    for (size_t i = 0; i < size; i++)
        dst[i] = src[i];
    return destination;
}

void *memmove(void *destination, const void *source, size_t size)
{
    unsigned char *dst = destination;
    const unsigned char *src = source;
    if ((uintptr_t)dst < (uintptr_t)src)
    {
        for (size_t i = 0; i < size; i++)
            dst[i] = src[i];
    }
    else if ((uintptr_t)dst > (uintptr_t)src)
    {
        while (size)
        {
            size--;
            dst[size] = src[size];
        }
    }
    return destination;
}

void *memset(void *destination, int value, size_t size)
{
    unsigned char *dst = destination;
    for (size_t i = 0; i < size; i++)
        dst[i] = (unsigned char)value;
    return destination;
}

int memcmp(const void *left, const void *right, size_t size)
{
    const unsigned char *a = left;
    const unsigned char *b = right;
    for (size_t i = 0; i < size; i++)
    {
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

void *memchr(const void *memory, int character, size_t size)
{
    const unsigned char *bytes = memory;
    unsigned char target = (unsigned char)character;
    for (size_t i = 0; i < size; i++)
    {
        if (bytes[i] == target)
            return (void *)(bytes + i);
    }
    return 0;
}

size_t strlen(const char *string)
{
    size_t length = 0;
    while (string[length])
        length++;
    return length;
}

size_t strnlen(const char *string, size_t max_length)
{
    size_t length = 0;
    while (length < max_length && string[length])
        length++;
    return length;
}

int strcmp(const char *left, const char *right)
{
    while (*left && *left == *right)
    {
        left++;
        right++;
    }
    return (unsigned char)*left - (unsigned char)*right;
}

int strncmp(const char *left, const char *right, size_t size)
{
    for (size_t i = 0; i < size; i++)
    {
        unsigned char a = (unsigned char)left[i];
        unsigned char b = (unsigned char)right[i];
        if (a != b || a == '\0')
            return a - b;
    }
    return 0;
}

char *strcpy(char *destination, const char *source)
{
    char *result = destination;
    while ((*destination++ = *source++))
        ;
    return result;
}

char *strncpy(char *destination, const char *source, size_t size)
{
    size_t i = 0;
    for (; i < size && source[i]; i++)
        destination[i] = source[i];
    for (; i < size; i++)
        destination[i] = '\0';
    return destination;
}

char *strcat(char *destination, const char *source)
{
    strcpy(destination + strlen(destination), source);
    return destination;
}

char *strncat(char *destination, const char *source, size_t size)
{
    size_t destination_length = strlen(destination);
    size_t source_length = strnlen(source, size);
    memcpy(destination + destination_length, source, source_length);
    destination[destination_length + source_length] = '\0';
    return destination;
}

static bool string_has_character(const char *string, char character);

char *strchr(const char *string, int character)
{
    char target = (char)character;
    for (;; string++)
    {
        if (*string == target)
            return (char *)string;
        if (*string == '\0')
            return 0;
    }
}

char *strrchr(const char *string, int character)
{
    char target = (char)character;
    const char *last = 0;
    do
    {
        if (*string == target)
            last = string;
    } while (*string++);
    return (char *)last;
}

char *strpbrk(const char *string, const char *accept)
{
    while (*string)
    {
        if (string_has_character(accept, *string))
            return (char *)string;
        string++;
    }
    return 0;
}

char *strstr(const char *string, const char *substring)
{
    if (!*substring)
        return (char *)string;
    for (; *string; string++)
    {
        size_t i = 0;
        while (substring[i] && string[i] == substring[i])
            i++;
        if (!substring[i])
            return (char *)string;
    }
    return 0;
}

char *strdup(const char *string)
{
    size_t size = strlen(string) + 1;
    char *copy = malloc(size);
    if (copy)
        memcpy(copy, string, size);
    return copy;
}

char *strndup(const char *string, size_t size)
{
    size_t length = strnlen(string, size);
    char *copy = malloc(length + 1);
    if (!copy)
        return 0;
    memcpy(copy, string, length);
    copy[length] = '\0';
    return copy;
}

static bool string_has_character(const char *string, char character)
{
    while (*string)
    {
        if (*string++ == character)
            return true;
    }
    return false;
}

size_t strspn(const char *string, const char *accept)
{
    size_t count = 0;
    while (string[count] && string_has_character(accept, string[count]))
        count++;
    return count;
}

size_t strcspn(const char *string, const char *reject)
{
    size_t count = 0;
    while (string[count] && !string_has_character(reject, string[count]))
        count++;
    return count;
}

char *strerror(int error_number)
{
    switch (error_number)
    {
        case 0: return "Success";
        case EPERM: return "Operation not permitted";
        case ENOENT: return "No such file or directory";
        case EIO: return "Input/output error";
        case EBADF: return "Bad file descriptor";
        case EAGAIN: return "Resource temporarily unavailable";
        case ENOMEM: return "Out of memory";
        case EFAULT: return "Bad address";
        case EINVAL: return "Invalid argument";
        case ESPIPE: return "Illegal seek";
        case EROFS: return "Read-only filesystem";
        case ERANGE: return "Result out of range";
        default: return "Unknown error";
    }
}

char *strtok(char *string, const char *delimiters)
{
    static char *next;
    if (string)
        next = string;
    if (!next)
        return 0;

    while (*next && string_has_character(delimiters, *next))
        next++;
    if (!*next)
    {
        next = 0;
        return 0;
    }

    char *token = next;
    while (*next && !string_has_character(delimiters, *next))
        next++;
    if (*next)
        *next++ = '\0';
    else
        next = 0;
    return token;
}
