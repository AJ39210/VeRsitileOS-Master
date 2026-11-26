#ifndef VERSITILE_LIBC_ENDIAN_H
#define VERSITILE_LIBC_ENDIAN_H

#include <stdint.h>

#define LITTLE_ENDIAN 1234
#define BIG_ENDIAN 4321
#define PDP_ENDIAN 3412

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define BYTE_ORDER BIG_ENDIAN
#else
#define BYTE_ORDER LITTLE_ENDIAN
#endif

#define htobe16(value) __builtin_bswap16((uint16_t)(value))
#define htole16(value) ((uint16_t)(value))
#define be16toh(value) __builtin_bswap16((uint16_t)(value))
#define le16toh(value) ((uint16_t)(value))

#define htobe32(value) __builtin_bswap32((uint32_t)(value))
#define htole32(value) ((uint32_t)(value))
#define be32toh(value) __builtin_bswap32((uint32_t)(value))
#define le32toh(value) ((uint32_t)(value))

#define htobe64(value) __builtin_bswap64((uint64_t)(value))
#define htole64(value) ((uint64_t)(value))
#define be64toh(value) __builtin_bswap64((uint64_t)(value))
#define le64toh(value) ((uint64_t)(value))

#endif
