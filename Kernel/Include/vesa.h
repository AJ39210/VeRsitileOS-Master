#ifndef VESA_H
#define VESA_H

#include <stdint.h>
#include <stdbool.h>

struct limine_framebuffer;

int vesa_init(struct limine_framebuffer *fb);

void vesa_put_pixel(uint64_t x, uint64_t y, uint32_t color);
void vesa_fill_rect(
    uint64_t x,
    uint64_t y,
    uint64_t width,
    uint64_t height,
    uint32_t color
);
bool vesa_blit_rgb(
    uint64_t x,
    uint64_t y,
    uint64_t width,
    uint64_t height,
    const uint32_t *pixels,
    uint64_t stride
);
bool vesa_blit_rgb_scaled(
    const uint8_t *pixels,
    uint64_t source_width,
    uint64_t source_height
);
bool vesa_read_rgb(
    uint64_t x,
    uint64_t y,
    uint64_t width,
    uint64_t height,
    uint32_t *pixels,
    uint64_t stride
);
void vesa_draw_text(
    uint64_t x,
    uint64_t y,
    uint32_t color,
    const char *text,
    uint64_t length
);
void vesa_draw_batch_begin(void);
void vesa_draw_batch_end(void);
bool vesa_load_psf2(const void *font_data, uint64_t size);
void vesa_clear(uint32_t color);
void vesa_console_reset(void);
void vesa_console_clear(void);
void vesa_console_set_color(uint32_t color);
void vesa_console_batch_begin(void);
void vesa_console_batch_end(void);
void vesa_console_putc(char c);

uint64_t vesa_get_width(void);
uint64_t vesa_get_height(void);
uint64_t vesa_get_pitch_bytes(void);

#endif