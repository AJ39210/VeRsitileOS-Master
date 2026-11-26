#ifndef VERSITILE_LIBC_SYS_SOCKET_H
#define VERSITILE_LIBC_SYS_SOCKET_H

#include <stddef.h>
#include <sys/types.h>

typedef unsigned short sa_family_t;
typedef unsigned int socklen_t;

struct sockaddr
{
    sa_family_t sa_family;
    char sa_data[14];
};

struct sockaddr_un
{
    sa_family_t sun_family;
    char sun_path[108];
};

#define AF_UNIX 1
#define AF_LOCAL AF_UNIX

#define SOCK_STREAM 1
#define SOCK_DGRAM 2

#define SOL_SOCKET 1
#define SO_ERROR 4
#define SO_SNDBUF 7
#define SO_RCVBUF 8

int socketpair(int domain, int type, int protocol, int descriptors[2]);

#endif
