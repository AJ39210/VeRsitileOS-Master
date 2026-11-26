#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool virtio_net_init(void);
bool virtio_net_is_initialized(void);
bool virtio_net_link_is_up(void);
const uint8_t *virtio_net_mac_address(void);
bool virtio_net_send(const void *packet, size_t length);
size_t virtio_net_receive(void *packet, size_t capacity);

#endif
