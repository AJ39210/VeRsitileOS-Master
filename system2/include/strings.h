#ifndef VERSITILE_LIBC_STRINGS_H
#define VERSITILE_LIBC_STRINGS_H

#include <stddef.h>

int strcasecmp(const char *left, const char *right);
int strncasecmp(const char *left, const char *right, size_t size);
char *strcasestr(const char *string, const char *substring);

#endif
