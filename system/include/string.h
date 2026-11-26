#ifndef VERSITILE_LIBC_STRING_H
#define VERSITILE_LIBC_STRING_H

#include <stddef.h>

void *memcpy(void *destination, const void *source, size_t size);
void *memmove(void *destination, const void *source, size_t size);
void *memset(void *destination, int value, size_t size);
int memcmp(const void *left, const void *right, size_t size);
void *memchr(const void *memory, int character, size_t size);
size_t strlen(const char *string);
size_t strnlen(const char *string, size_t max_length);
int strcmp(const char *left, const char *right);
int strncmp(const char *left, const char *right, size_t size);
char *strcpy(char *destination, const char *source);
char *strncpy(char *destination, const char *source, size_t size);
char *strcat(char *destination, const char *source);
char *strncat(char *destination, const char *source, size_t size);
char *strchr(const char *string, int character);
char *strrchr(const char *string, int character);
char *strpbrk(const char *string, const char *accept);
char *strstr(const char *string, const char *substring);
char *strdup(const char *string);
char *strndup(const char *string, size_t size);
char *strtok(char *string, const char *delimiters);
size_t strspn(const char *string, const char *accept);
size_t strcspn(const char *string, const char *reject);
char *strerror(int error_number);

#endif
