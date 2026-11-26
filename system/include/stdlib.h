#ifndef VERSITILE_LIBC_STDLIB_H
#define VERSITILE_LIBC_STDLIB_H

#include <stddef.h>

void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void *realloc(void *pointer, size_t size);
void free(void *pointer);
_Noreturn void exit(int status);
_Noreturn void abort(void);
int atoi(const char *string);
long strtol(const char *string, char **end, int base);
unsigned long strtoul(const char *string, char **end, int base);
int abs(int value);
long labs(long value);
void qsort(void *base, size_t count, size_t size,
           int (*compare)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t count, size_t size,
              int (*compare)(const void *, const void *));
int rand(void);
void srand(unsigned seed);

#endif
