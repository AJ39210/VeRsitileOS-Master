#ifndef VIRTIO_BLK_H
#define VIRTIO_BLK_H

#include <stdbool.h>
#include <stdint.h>

bool virtio_blk_init(void);
uint32_t virtio_blk_device_count(void);

#endif
