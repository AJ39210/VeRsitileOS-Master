#ifndef VIRTIO_INPUT_H
#define VIRTIO_INPUT_H

#include <stdbool.h>

bool virtio_input_init(void);
void virtio_input_poll(void);
uint32_t virtio_input_device_count(void);

#endif
