#ifndef VERSITILE_DOOM_COMPAT_H
#define VERSITILE_DOOM_COMPAT_H

#include <stdio.h>
#include <stdint.h>
#include <strings.h>
#include <sys/stat.h>
#include <alloca.h>

int access(const char *path, int mode);
int fscanf(FILE *stream, const char *format, ...);
int sprintf(char *buffer, const char *format, ...);
int sscanf(const char *input, const char *format, ...);
void setbuf(FILE *stream, char *buffer);
char *getenv(const char *name);
int fstat(int descriptor, struct stat *status);
int mkdir(const char *path, mode_t mode);

#endif
