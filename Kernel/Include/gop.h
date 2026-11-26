#ifndef GOP_H
#define GOP_H

#include <stdbool.h>

struct limine_framebuffer;

/*
 * Initializes the Limine-provided framebuffer selected by UEFI GOP.
 * UEFI boot services are no longer available once Limine transfers control.
 */
bool gop_init(struct limine_framebuffer *framebuffer);
bool gop_is_initialized(void);

#endif
