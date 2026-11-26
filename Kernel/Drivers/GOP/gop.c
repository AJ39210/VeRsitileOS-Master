#include "gop.h"

#include "limine.h"
#include "vesa.h"

static bool initialized;

bool gop_init(struct limine_framebuffer *framebuffer)
{
    initialized = false;
    if (!framebuffer ||
        framebuffer->memory_model != LIMINE_FRAMEBUFFER_RGB ||
        framebuffer->bpp != 32 ||
        !framebuffer->width ||
        !framebuffer->height ||
        !framebuffer->pitch)
        return false;

    initialized = vesa_init(framebuffer) != 0;
    return initialized;
}

bool gop_is_initialized(void)
{
    return initialized;
}
