#ifndef VIRTIO_GPU_H
#define VIRTIO_GPU_H

#include <stdbool.h>

bool virtio_gpu_init(void);
bool virtio_gpu_is_initialized(void);
bool virtio_gpu_present(void);
bool virtio_gpu_driver_init(void);
bool virtio_gpu_driver_is_initialized(void);
bool virtio_gpu_driver_present(void);

#endif
