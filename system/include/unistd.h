#ifndef VERSITILE_LIBC_UNISTD_H
#define VERSITILE_LIBC_UNISTD_H

#include <stddef.h>
#include <sys/types.h>

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

ssize_t read(int descriptor, void *buffer, size_t size);
ssize_t write(int descriptor, const void *buffer, size_t size);
int open(const char *path, int flags, ...);
int close(int descriptor);
int unlink(const char *path);
off_t lseek(int descriptor, off_t offset, int whence);
int getpid(void);
int isatty(int descriptor);
void yield(void);
_Noreturn void _exit(int status);

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#endif
