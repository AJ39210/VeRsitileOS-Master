#ifndef VERSITILE_LIBC_ERRNO_H
#define VERSITILE_LIBC_ERRNO_H

extern int errno;

#define ENOMEM 12
#define EFAULT 14
#define EINVAL 22
#define EPERM 1
#define ENOENT 2
#define EEXIST 17
#define ENOSPC 28
#define EIO 5
#define EPIPE 32
#define ENOEXEC 8
#define EBADF 9
#define EAGAIN 11
#define EWOULDBLOCK EAGAIN
#define EACCES 13
#define EPROTONOSUPPORT 93
#define ENODEV 19
#define EAFNOSUPPORT 97
#define EISDIR 21
#define ENOTDIR 20
#define ESPIPE 29
#define EROFS 30
#define ERANGE 34
#define ENAMETOOLONG 36
#define ENOSYS 38
#define ENOTSUP 95
#define EOVERFLOW 75
#define ENOTEMPTY 39
#define ETIMEDOUT 110

#endif
