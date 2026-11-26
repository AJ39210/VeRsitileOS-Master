#ifndef NETWORK_H
#define NETWORK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NETWORK_HOSTNAME_MAX 255

typedef struct
{
    uint8_t mac[6];
    uint8_t address[4];
    uint8_t netmask[4];
    uint8_t gateway[4];
    uint8_t dns[2][4];
    uint8_t link_up;
    uint8_t configured;
    uint8_t reserved[2];
} network_info_t;

typedef struct
{
    uint8_t address[4];
    uint8_t reserved[4];
    uint64_t round_trip_ms;
} network_ping_request_t;

typedef struct
{
    char hostname[NETWORK_HOSTNAME_MAX + 1];
    uint8_t address[4];
} network_resolve_request_t;

bool network_init(void);
void network_poll(void);
void network_get_info(network_info_t *info);
bool network_ping(const uint8_t address[4], uint64_t *round_trip_ms);
bool network_resolve(const char *hostname, uint8_t address[4]);

#endif
