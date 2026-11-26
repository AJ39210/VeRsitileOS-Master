#ifndef VERSITILE_DOOM_SYS_STAT_H
#define VERSITILE_DOOM_SYS_STAT_H

#include <sys/types.h>

#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

struct stat
{
    off_t st_size;
};

int fstat(int descriptor, struct stat *status);
int mkdir(const char *path, mode_t mode);

#endif
