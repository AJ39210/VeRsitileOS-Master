#include <stdint.h>
#include <stdbool.h>

#include "font.h"
#include "limine.h"
#include "vesa.h"
#include "virtio_gpu.h"

#define CONSOLE_BACKGROUND 0x000000
#define CONSOLE_FOREGROUND 0x00E6EDF3
#define FONT_WIDTH 8
#define FONT_HEIGHT 16
#define FONT_MAX_GLYPHS 4096
#define PSF2_MAGIC 0x864AB572U

typedef struct
{
    uint32_t magic;
    uint32_t version;
    uint32_t header_size;
    uint32_t flags;
    uint32_t glyph_count;
    uint32_t bytes_per_glyph;
    uint32_t height;
    uint32_t width;
} psf2_header_t;

static uint32_t *framebuffer = 0;
static uint64_t width = 0;
static uint64_t height = 0;
static uint64_t pitch = 0;
static uint64_t console_x = 8;
static uint64_t console_y = 8;
static uint32_t console_foreground = CONSOLE_FOREGROUND;
static bool console_batch_active;
static bool console_dirty;
static uint32_t draw_batch_depth;
static bool draw_batch_dirty;

static uint8_t red_size;
static uint8_t red_shift;

static uint8_t green_size;
static uint8_t green_shift;

static uint8_t blue_size;
static uint8_t blue_shift;
static uint8_t loaded_font[FONT_MAX_GLYPHS * FONT_HEIGHT];
static uint16_t unicode_glyphs[65536];
static uint32_t loaded_glyph_count;
static bool font_loaded;

static uint32_t channel(uint8_t value, uint8_t size, uint8_t shift)
{
    uint32_t mask = (1U << size) - 1;
    return ((value * mask / 255) & mask) << shift;
}

static uint32_t pack_color(uint32_t color)
{
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;

    return channel(r, red_size, red_shift) |
           channel(g, green_size, green_shift) |
           channel(b, blue_size, blue_shift);
}

static void present_draw_batch(void)
{
    if (draw_batch_depth)
        draw_batch_dirty = true;
    else
        (void)virtio_gpu_present();
}

static uint64_t console_left_margin(void)
{
    return width >= 3 * FONT_WIDTH ? FONT_WIDTH : 0;
}

int vesa_init(struct limine_framebuffer *fb)
{
    if (!fb || !fb->address)
        return 0;

    if (fb->bpp != 32 || fb->memory_model != LIMINE_FRAMEBUFFER_RGB)
        return 0;

    framebuffer = (uint32_t *)fb->address;

    width = fb->width;
    height = fb->height;
    pitch = fb->pitch / 4;

    red_size = fb->red_mask_size;
    red_shift = fb->red_mask_shift;

    green_size = fb->green_mask_size;
    green_shift = fb->green_mask_shift;

    blue_size = fb->blue_mask_size;
    blue_shift = fb->blue_mask_shift;

    console_x = console_left_margin();
    console_y = 8;

    return 1;
}

bool vesa_load_psf2(const void *font_data, uint64_t size)
{
    if (!font_data || size < sizeof(psf2_header_t))
        return false;

    const uint8_t *bytes = font_data;
    const psf2_header_t *header = font_data;
    if (header->magic != PSF2_MAGIC ||
        header->version != 0 ||
        header->header_size < sizeof(*header) ||
        header->header_size > size ||
        header->width != FONT_WIDTH ||
        header->height != FONT_HEIGHT ||
        header->bytes_per_glyph != FONT_HEIGHT ||
        header->glyph_count == 0 ||
        header->glyph_count > FONT_MAX_GLYPHS ||
        header->glyph_count >
            (size - header->header_size) / header->bytes_per_glyph)
        return false;

    uint64_t glyph_bytes =
        (uint64_t)header->glyph_count * header->bytes_per_glyph;
    for (uint64_t i = 0; i < glyph_bytes; i++)
        loaded_font[i] = bytes[header->header_size + i];

    loaded_glyph_count = header->glyph_count;
    for (uint32_t i = 0; i < 65536; i++)
        unicode_glyphs[i] = UINT16_MAX;

    if (header->flags & 1U)
    {
        uint64_t position = header->header_size + glyph_bytes;
        uint32_t glyph = 0;
        while (position < size && glyph < loaded_glyph_count)
        {
            uint8_t value = bytes[position++];
            if (value == 0xFF)
            {
                glyph++;
                continue;
            }
            if (value == 0xFE)
            {
                while (position < size &&
                       bytes[position] != 0xFE &&
                       bytes[position] != 0xFF)
                    position++;
                continue;
            }

            uint32_t codepoint;
            if (value < 0x80)
            {
                codepoint = value;
            }
            else if ((value & 0xE0) == 0xC0 && position < size)
            {
                codepoint = ((uint32_t)(value & 0x1F) << 6) |
                    (bytes[position++] & 0x3F);
            }
            else if ((value & 0xF0) == 0xE0 && size - position >= 2)
            {
                codepoint = ((uint32_t)(value & 0x0F) << 12) |
                    ((uint32_t)(bytes[position] & 0x3F) << 6) |
                    (bytes[position + 1] & 0x3F);
                position += 2;
            }
            else if ((value & 0xF8) == 0xF0 && size - position >= 3)
            {
                codepoint = ((uint32_t)(value & 0x07) << 18) |
                    ((uint32_t)(bytes[position] & 0x3F) << 12) |
                    ((uint32_t)(bytes[position + 1] & 0x3F) << 6) |
                    (bytes[position + 2] & 0x3F);
                position += 3;
            }
            else
            {
                continue;
            }

            if (codepoint < 65536)
                unicode_glyphs[codepoint] = (uint16_t)glyph;
        }
    }

    font_loaded = true;
    return true;
}

void vesa_put_pixel(uint64_t x, uint64_t y, uint32_t color)
{
    if (!framebuffer || x >= width || y >= height)
        return;

    framebuffer[y * pitch + x] = pack_color(color);
}

void vesa_fill_rect(
    uint64_t x,
    uint64_t y,
    uint64_t rect_width,
    uint64_t rect_height,
    uint32_t color
)
{
    if (!framebuffer || x >= width || y >= height)
        return;
    if (rect_width > width - x)
        rect_width = width - x;
    if (rect_height > height - y)
        rect_height = height - y;

    uint32_t pixel = pack_color(color);
    for (uint64_t row = 0; row < rect_height; row++)
    {
        uint32_t *destination = framebuffer + (y + row) * pitch + x;
        for (uint64_t column = 0; column < rect_width; column++)
            destination[column] = pixel;
    }
    present_draw_batch();
}

bool vesa_blit_rgb(
    uint64_t x,
    uint64_t y,
    uint64_t rect_width,
    uint64_t rect_height,
    const uint32_t *pixels,
    uint64_t source_stride
)
{
    if (!framebuffer || !pixels || x > width || y > height ||
        rect_width > width - x || rect_height > height - y ||
        source_stride < rect_width)
        return false;

    for (uint64_t row = 0; row < rect_height; row++)
    {
        uint32_t *destination = framebuffer + (y + row) * pitch + x;
        const uint32_t *source = pixels + row * source_stride;
        for (uint64_t column = 0; column < rect_width; column++)
            destination[column] = pack_color(source[column]);
    }
    present_draw_batch();
    return true;
}

bool vesa_blit_rgb_scaled(
    const uint8_t *pixels,
    uint64_t source_width,
    uint64_t source_height
)
{
    if (!framebuffer || !pixels || !source_width || !source_height ||
        !width || !height)
        return false;

    for (uint64_t y = 0; y < height; y++)
    {
        uint64_t source_y = y * source_height / height;
        uint32_t *destination = framebuffer + y * pitch;
        for (uint64_t x = 0; x < width; x++)
        {
            uint64_t source_x = x * source_width / width;
            const uint8_t *source =
                pixels + (source_y * source_width + source_x) * 3;
            uint32_t color =
                ((uint32_t)source[0] << 16) |
                ((uint32_t)source[1] << 8) |
                source[2];
            destination[x] = pack_color(color);
        }
    }

    present_draw_batch();
    return true;
}

bool vesa_read_rgb(
    uint64_t x,
    uint64_t y,
    uint64_t rect_width,
    uint64_t rect_height,
    uint32_t *pixels,
    uint64_t destination_stride
)
{
    if (!framebuffer || !pixels || x > width || y > height ||
        rect_width > width - x || rect_height > height - y ||
        destination_stride < rect_width)
        return false;

    for (uint64_t row = 0; row < rect_height; row++)
    {
        const uint32_t *source = framebuffer + (y + row) * pitch + x;
        uint32_t *destination = pixels + row * destination_stride;
        for (uint64_t column = 0; column < rect_width; column++)
        {
            uint32_t pixel = source[column];
            uint8_t red = (pixel >> red_shift) &
                ((1U << red_size) - 1);
            uint8_t green = (pixel >> green_shift) &
                ((1U << green_size) - 1);
            uint8_t blue = (pixel >> blue_shift) &
                ((1U << blue_size) - 1);
            destination[column] =
                ((uint32_t)(red * 255 / ((1U << red_size) - 1)) << 16) |
                ((uint32_t)(green * 255 / ((1U << green_size) - 1)) << 8) |
                (uint32_t)(blue * 255 / ((1U << blue_size) - 1));
        }
    }
    return true;
}

static uint32_t decode_utf8(const uint8_t **text, const uint8_t *end)
{
    uint8_t first = *(*text)++;
    if (first < 0x80)
        return first;
    if ((first & 0xE0) == 0xC0 && *text < end)
        return ((uint32_t)(first & 0x1F) << 6) |
            (*(*text)++ & 0x3F);
    if ((first & 0xF0) == 0xE0 && end - *text >= 2)
    {
        uint32_t codepoint = ((uint32_t)(first & 0x0F) << 12) |
            ((uint32_t)(*text)[0] & 0x3F) << 6 |
            ((uint32_t)(*text)[1] & 0x3F);
        *text += 2;
        return codepoint;
    }
    if ((first & 0xF8) == 0xF0 && end - *text >= 3)
    {
        uint32_t codepoint = ((uint32_t)(first & 0x07) << 18) |
            ((uint32_t)(*text)[0] & 0x3F) << 12 |
            ((uint32_t)(*text)[1] & 0x3F) << 6 |
            ((uint32_t)(*text)[2] & 0x3F);
        *text += 3;
        return codepoint;
    }
    return '?';
}

static const uint8_t *font_glyph(uint32_t codepoint)
{
    if (font_loaded)
    {
        uint32_t glyph = codepoint < 65536
            ? unicode_glyphs[codepoint]
            : UINT16_MAX;
        if (glyph == UINT16_MAX && codepoint < loaded_glyph_count)
            glyph = codepoint;
        if (glyph == UINT16_MAX && '?' < loaded_glyph_count)
            glyph = '?';
        if (glyph < loaded_glyph_count)
            return loaded_font + glyph * FONT_HEIGHT;
    }

    if (codepoint < 32 || codepoint > 122)
        codepoint = '?';
    return kernel_font_data + (codepoint - 32) * FONT_HEIGHT;
}

static void draw_glyph(
    uint64_t x,
    uint64_t y,
    uint32_t foreground,
    uint32_t background,
    uint32_t codepoint,
    bool opaque
)
{
    const uint8_t *glyph = font_glyph(codepoint);
    for (uint64_t row = 0; row < FONT_HEIGHT; row++)
    {
        uint64_t py = y + row;
        if (py >= height)
            break;
        for (uint64_t column = 0; column < FONT_WIDTH; column++)
        {
            uint64_t px = x + column;
            if (px >= width)
                break;
            bool set = (glyph[row] & (0x80 >> column)) != 0;
            if (set || opaque)
                framebuffer[py * pitch + px] =
                    set ? foreground : background;
        }
    }
}

void vesa_draw_text(
    uint64_t x,
    uint64_t y,
    uint32_t color,
    const char *text,
    uint64_t length
)
{
    if (!framebuffer || !text)
        return;

    uint32_t foreground = pack_color(color);
    const uint8_t *cursor = (const uint8_t *)text;
    const uint8_t *end = cursor + length;
    while (cursor < end)
    {
        uint32_t codepoint = decode_utf8(&cursor, end);
        if (codepoint == '\n')
        {
            x = 0;
            y += FONT_HEIGHT;
        }
        else if (codepoint == '\r')
        {
            x = 0;
        }
        else
        {
            draw_glyph(x, y, foreground, 0, codepoint, false);
            x += FONT_WIDTH;
        }

        if (y >= height)
            break;
    }
    present_draw_batch();
}

void vesa_draw_batch_begin(void)
{
    draw_batch_depth++;
}

void vesa_draw_batch_end(void)
{
    if (!draw_batch_depth)
        return;
    draw_batch_depth--;
    if (!draw_batch_depth && draw_batch_dirty)
    {
        draw_batch_dirty = false;
        (void)virtio_gpu_present();
    }
}

void vesa_clear(uint32_t color)
{
    for (uint64_t y = 0; y < height; y++)
        for (uint64_t x = 0; x < width; x++)
            vesa_put_pixel(x, y, color);
    present_draw_batch();
}

void vesa_console_reset(void)
{
    console_x = console_left_margin();
    console_y = 8;
}

void vesa_console_clear(void)
{
    if (!framebuffer)
        return;

    uint32_t background = pack_color(CONSOLE_BACKGROUND);
    for (uint64_t y = 0; y < height; y++)
    {
        uint32_t *row = framebuffer + y * pitch;
        for (uint64_t x = 0; x < width; x++)
            row[x] = background;
    }
    vesa_console_reset();
    console_dirty = true;
}

void vesa_console_set_color(uint32_t color)
{
    console_foreground = color & 0x00FFFFFF;
}

void vesa_console_batch_begin(void)
{
    console_batch_active = true;
}

void vesa_console_batch_end(void)
{
    console_batch_active = false;
    if (console_dirty)
    {
        console_dirty = false;
        (void)virtio_gpu_present();
    }
}

uint64_t vesa_get_width(void)
{
    return width;
}

uint64_t vesa_get_height(void)
{
    return height;
}

uint64_t vesa_get_pitch_bytes(void)
{
    return pitch * sizeof(uint32_t);
}

static void console_scroll(void)
{
    if (height <= FONT_HEIGHT)
    {
        console_y = 0;
        return;
    }

    uint32_t *destination = framebuffer;
    const uint32_t *source = framebuffer + FONT_HEIGHT * pitch;
    uint64_t pixel_count = (height - FONT_HEIGHT) * pitch;
    __asm__ volatile (
        "cld\n\t"
        "rep movsl"
        : "+D"(destination), "+S"(source), "+c"(pixel_count)
        :
        : "memory"
    );

    uint32_t background = pack_color(CONSOLE_BACKGROUND);
    for (uint64_t y = height - FONT_HEIGHT; y < height; y++)
    {
        for (uint64_t x = 0; x < width; x++)
            framebuffer[y * pitch + x] = background;
    }

    console_y = height - FONT_HEIGHT;
}

static void console_newline(void)
{
    console_x = console_left_margin();
    console_y += FONT_HEIGHT;

    if (console_y + FONT_HEIGHT > height)
        console_scroll();
}

static void console_draw_glyph(unsigned char character)
{
    uint32_t foreground = pack_color(console_foreground);
    uint32_t background = pack_color(CONSOLE_BACKGROUND);
    draw_glyph(
        console_x,
        console_y,
        foreground,
        background,
        character,
        true
    );
    console_x += FONT_WIDTH;
}

void vesa_console_putc(char c)
{
    if (!framebuffer || width < FONT_WIDTH || height < FONT_HEIGHT)
        return;

    if (c == '\r')
        return;

    if (c == '\b')
    {
        uint64_t left_margin = console_left_margin();
        if (console_x >= left_margin + FONT_WIDTH)
        {
            console_x -= FONT_WIDTH;
            console_draw_glyph(' ');
            console_x -= FONT_WIDTH;
            console_dirty = true;
        }
        if (!console_batch_active && console_dirty)
        {
            console_dirty = false;
            (void)virtio_gpu_present();
        }
        return;
    }

    if (c == '\n')
    {
        console_newline();
        if (!console_batch_active && console_dirty)
        {
            console_dirty = false;
            (void)virtio_gpu_present();
        }
        return;
    }

    if (c == '\t')
    {
        for (uint8_t i = 0; i < 4; i++)
            vesa_console_putc(' ');
        return;
    }

    if (console_x + FONT_WIDTH > width)
        console_newline();

    if (console_y + FONT_HEIGHT > height)
        console_scroll();

    console_draw_glyph((unsigned char)c);
    console_dirty = true;
}