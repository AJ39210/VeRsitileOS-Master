#ifndef VERSITILE_LIBC_STDIO_H
#define VERSITILE_LIBC_STDIO_H

#include <stdarg.h>
#include <limits.h>
#include <stddef.h>
#include <sys/types.h>

#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

typedef struct
{
    int descriptor;
    int error;
    int eof;
} FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

int printf(const char *format, ...);
int fprintf(FILE *stream, const char *format, ...);
int vprintf(const char *format, va_list arguments);
int vfprintf(FILE *stream, const char *format, va_list arguments);
int vsnprintf(char *buffer, size_t size, const char *format,
              va_list arguments);
int puts(const char *string);
int fputs(const char *string, FILE *stream);
int putchar(int character);
int getchar(void);
int fputc(int character, FILE *stream);
int fgetc(FILE *stream);
#define putc(character, stream) fputc((character), (stream))
#define getc(stream) fgetc((stream))
int snprintf(char *buffer, size_t size, const char *format, ...);
FILE *fopen(const char *path, const char *mode);
int fclose(FILE *stream);
size_t fread(void *buffer, size_t size, size_t count, FILE *stream);
size_t fwrite(const void *buffer, size_t size, size_t count, FILE *stream);
int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);
void rewind(FILE *stream);
int feof(FILE *stream);
int ferror(FILE *stream);
void clearerr(FILE *stream);
int fflush(FILE *stream);
char *fgets(char *buffer, int size, FILE *stream);
void perror(const char *prefix);

#endif
