#include "ctype.h"
#include "string.h"
#include "strings.h"

int strcasecmp(const char *left, const char *right)
{
    while (*left && tolower((unsigned char)*left) ==
                    tolower((unsigned char)*right))
    {
        left++;
        right++;
    }
    return tolower((unsigned char)*left) -
        tolower((unsigned char)*right);
}

int strncasecmp(const char *left, const char *right, size_t size)
{
    for (size_t i = 0; i < size; i++)
    {
        unsigned char a = (unsigned char)left[i];
        unsigned char b = (unsigned char)right[i];
        int lower_a = tolower(a);
        int lower_b = tolower(b);
        if (lower_a != lower_b || a == '\0' || b == '\0')
            return lower_a - lower_b;
    }
    return 0;
}

char *strcasestr(const char *string, const char *substring)
{
    size_t length = strlen(substring);
    if (!length)
        return (char *)string;

    while (*string)
    {
        if (strncasecmp(string, substring, length) == 0)
            return (char *)string;
        string++;
    }
    return 0;
}
