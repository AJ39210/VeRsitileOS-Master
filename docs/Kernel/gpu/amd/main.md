# AMD GPU support

The `amd/` directory currently contains the [Radeon HD driver](RadeonHD/main.md). It classifies supported PCI display devices and can attach the generic software DRM framebuffer to a matching firmware scanout. It does not initialize a new display mode or accelerate rendering.
