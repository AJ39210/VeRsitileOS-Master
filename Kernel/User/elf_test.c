#include <ctype.h>
#include <errno.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <endian.h>
#include <os/gui.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <png.h>
#include <zlib.h>

static volatile unsigned long initialized_data = 40;
static volatile unsigned long zero_initialized;

typedef struct
{
    unsigned char *data;
    size_t size;
    size_t capacity;
    size_t offset;
} png_memory_buffer_t;

static png_memory_buffer_t png_test_buffer;

static void png_memory_write(
    png_structp png,
    png_bytep data,
    png_size_t length
)
{
    png_memory_buffer_t *buffer = png_get_io_ptr(png);
    if (length > (size_t)-1 - buffer->size)
        png_error(png, "PNG test buffer overflow");

    size_t required = buffer->size + length;
    if (required > buffer->capacity)
    {
        size_t capacity = buffer->capacity ? buffer->capacity : 64;
        while (capacity < required)
        {
            if (capacity > (size_t)-1 / 2)
            {
                capacity = required;
                break;
            }
            capacity *= 2;
        }

        unsigned char *resized = realloc(buffer->data, capacity);
        if (!resized)
            png_error(png, "PNG test allocation failed");
        buffer->data = resized;
        buffer->capacity = capacity;
    }

    memcpy(buffer->data + buffer->size, data, length);
    buffer->size = required;
}

static void png_memory_read(
    png_structp png,
    png_bytep data,
    png_size_t length
)
{
    png_memory_buffer_t *buffer = png_get_io_ptr(png);
    if (buffer->offset > buffer->size ||
        length > buffer->size - buffer->offset)
        png_error(png, "PNG test input exhausted");

    memcpy(data, buffer->data + buffer->offset, length);
    buffer->offset += length;
}

static void png_memory_flush(png_structp png)
{
    (void)png;
}

static int compare_ints(const void *left, const void *right)
{
    int a = *(const int *)left;
    int b = *(const int *)right;
    return (a > b) - (a < b);
}

static int test_png_decode(void)
{
    png_test_buffer = (png_memory_buffer_t){0};
    const unsigned char expected_pixel[] = {0x26, 0x80, 0xD4, 0xFF};

    png_structp writer = png_create_write_struct(
        PNG_LIBPNG_VER_STRING, 0, 0, 0
    );
    if (!writer)
        return 1;
    png_infop writer_info = png_create_info_struct(writer);
    if (!writer_info)
    {
        png_destroy_write_struct(&writer, 0);
        return 2;
    }

    if (setjmp(png_jmpbuf(writer)))
    {
        png_destroy_write_struct(&writer, &writer_info);
        free(png_test_buffer.data);
        png_test_buffer = (png_memory_buffer_t){0};
        return 3;
    }

    png_set_write_fn(
        writer, &png_test_buffer, png_memory_write, png_memory_flush
    );
    png_set_IHDR(
        writer,
        writer_info,
        1,
        1,
        8,
        PNG_COLOR_TYPE_RGBA,
        PNG_INTERLACE_NONE,
        PNG_COMPRESSION_TYPE_BASE,
        PNG_FILTER_TYPE_BASE
    );
    png_write_info(writer, writer_info);
    png_write_row(writer, expected_pixel);
    png_write_end(writer, writer_info);
    png_destroy_write_struct(&writer, &writer_info);

    png_structp reader = png_create_read_struct(
        PNG_LIBPNG_VER_STRING, 0, 0, 0
    );
    if (!reader)
    {
        free(png_test_buffer.data);
        return 4;
    }
    png_infop reader_info = png_create_info_struct(reader);
    if (!reader_info)
    {
        png_destroy_read_struct(&reader, 0, 0);
        free(png_test_buffer.data);
        return 5;
    }

    if (setjmp(png_jmpbuf(reader)))
    {
        png_destroy_read_struct(&reader, &reader_info, 0);
        free(png_test_buffer.data);
        png_test_buffer = (png_memory_buffer_t){0};
        return 6;
    }

    png_test_buffer.offset = 0;
    png_set_read_fn(reader, &png_test_buffer, png_memory_read);
    png_read_info(reader, reader_info);
    png_uint_32 width;
    png_uint_32 height;
    int bit_depth;
    int color_type;
    if (!png_get_IHDR(
            reader, reader_info, &width, &height, &bit_depth, &color_type,
            0, 0, 0) ||
        width != 1 || height != 1 ||
        bit_depth != 8 || color_type != PNG_COLOR_TYPE_RGBA)
    {
        png_destroy_read_struct(&reader, &reader_info, 0);
        free(png_test_buffer.data);
        return 7;
    }

    png_read_update_info(reader, reader_info);
    unsigned char pixel[4];
    png_read_row(reader, pixel, 0);
    png_read_end(reader, reader_info);
    png_destroy_read_struct(&reader, &reader_info, 0);
    free(png_test_buffer.data);
    png_test_buffer = (png_memory_buffer_t){0};

    return memcmp(pixel, expected_pixel, sizeof(expected_pixel)) == 0
        ? 0
        : 8;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    initialized_data += 2;
    if (initialized_data != 42 || zero_initialized != 0)
    {
        puts("libc ELF data/BSS check failed");
        return 1;
    }

    os_keyboard_event_t keyboard_event;
    if (os_keyboard_read(&keyboard_event, 1) != -EINVAL)
    {
        puts("keyboard event syscall validation failed");
        return 2;
    }

    unsigned char *mapping = mmap(
        0,
        3 * 4096,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0);
    if (mapping == MAP_FAILED ||
        (uintptr_t)mapping % 4096 != 0)
    {
        puts("POSIX anonymous mmap failed");
        return 2;
    }
    mapping[0] = 0xA5;
    mapping[2 * 4096] = 0x5A;
    if (mapping[0] != 0xA5 || mapping[2 * 4096] != 0x5A)
    {
        puts("POSIX mmap read/write access failed");
        return 2;
    }
    if (mprotect(mapping, 4096, PROT_READ) != 0)
    {
        printf("POSIX mprotect read-only failed: errno=%d\n", errno);
        return 2;
    }
    if (mprotect(mapping, 4096, PROT_NONE) != 0)
    {
        printf("POSIX mprotect no-access failed: errno=%d\n", errno);
        return 2;
    }
    if (mprotect(mapping, 4096, PROT_READ | PROT_WRITE) != 0)
    {
        printf("POSIX mprotect restore failed: errno=%d\n", errno);
        return 2;
    }
    if (mapping[0] != 0xA5)
    {
        puts("POSIX mprotect did not preserve mapped data");
        return 2;
    }
    if (mprotect(mapping + 3 * 4096, 4096, PROT_READ) != -1)
    {
        puts("POSIX mprotect accepted an unmapped range");
        return 2;
    }
    if (munmap(mapping + 4096, 4096) != 0)
    {
        puts("POSIX mmap partial munmap failed");
        return 2;
    }
    unsigned char *reused_mapping = mmap(
        mapping + 4096,
        4096,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0);
    if (reused_mapping == MAP_FAILED ||
        reused_mapping != mapping + 4096 ||
        munmap(reused_mapping, 4096) != 0 ||
        munmap(mapping, 4096) != 0 ||
        munmap(mapping + 2 * 4096, 4096) != 0)
    {
        puts("POSIX mmap address reuse or split cleanup failed");
        return 2;
    }

    int socket_descriptors[2];
    if (socketpair(
            AF_UNIX,
            SOCK_STREAM,
            0,
            socket_descriptors) != 0)
    {
        printf("POSIX socketpair failed: errno=%d\n", errno);
        return 2;
    }

    static const char socket_message[] = "local stream works";
    char socket_received[sizeof(socket_message)] = {0};
    struct pollfd socket_poll = {
        .fd = socket_descriptors[1],
        .events = POLLIN
    };
    if (poll(&socket_poll, 1, 1) != 0 ||
        write(
            socket_descriptors[0],
            socket_message,
            sizeof(socket_message)) != sizeof(socket_message) ||
        poll(&socket_poll, 1, 100) != 1 ||
        !(socket_poll.revents & POLLIN) ||
        read(
            socket_descriptors[1],
            socket_received,
            sizeof(socket_received)) != sizeof(socket_received) ||
        memcmp(
            socket_message,
            socket_received,
            sizeof(socket_message)) != 0 ||
        close(socket_descriptors[1]) != 0 ||
        write(
            socket_descriptors[0],
            socket_message,
            sizeof(socket_message)) != -1 ||
        errno != EPIPE)
    {
        puts("POSIX local stream/poll/close check failed");
        return 2;
    }
    socket_poll.fd = socket_descriptors[0];
    socket_poll.revents = 0;
    if (poll(&socket_poll, 1, 0) != 1 ||
        !(socket_poll.revents & (POLLHUP | POLLIN)) ||
        read(socket_descriptors[0], socket_received, 1) != 0 ||
        close(socket_descriptors[0]) != 0)
    {
        puts("POSIX local stream EOF/readiness check failed");
        return 2;
    }

    char source[] = "dynamic libc works";
    char *copy = calloc(sizeof(source), 1);
    if (!copy)
    {
        puts("libc malloc failed");
        return 3;
    }

    memcpy(copy, source, sizeof(source));
    if (strcmp(copy, source) != 0)
    {
        puts("libc string functions failed");
        free(copy);
        return 4;
    }

    char *larger_copy = realloc(copy, sizeof(source) + 8);
    if (!larger_copy)
    {
        free(copy);
        puts("libc realloc failed");
        return 5;
    }
    copy = larger_copy;

    char formatted[32];
    if (snprintf(formatted, sizeof(formatted), "size=%zu", sizeof(source)) < 0 ||
        strcmp(formatted, "size=19") != 0 ||
        !isalnum('V') ||
        tolower('A') != 'a' ||
        strcasecmp("PNG", "png") != 0 ||
        be32toh(htobe32(0x12345678U)) != 0x12345678U ||
        memchr(source, 'l', sizeof(source)) == 0)
    {
        puts("libc formatting/ctype check failed");
        free(copy);
        return 6;
    }

    int values[] = {4, 1, 3, 2};
    qsort(values, 4, sizeof(values[0]), compare_ints);
    int key = 3;
    if (values[0] != 1 || values[3] != 4 ||
        !bsearch(&key, values, 4, sizeof(values[0]), compare_ints))
    {
        puts("libc sorting/search check failed");
        free(copy);
        return 7;
    }

    jmp_buf jump_environment;
    int jump_result = setjmp(jump_environment);
    if (jump_result == 0)
        longjmp(jump_environment, 7);
    if (jump_result != 7)
    {
        puts("libc setjmp/longjmp check failed");
        free(copy);
        return 8;
    }

    void *small_allocations[128];
    for (unsigned i = 0; i < 128; i++)
    {
        small_allocations[i] = malloc(24);
        if (!small_allocations[i])
        {
            puts("libc heap arena allocation failed");
            free(copy);
            return 9;
        }
        memset(small_allocations[i], (int)i, 24);
    }
    for (unsigned i = 0; i < 128; i++)
        free(small_allocations[i]);

    FILE *library_file = fopen("/lib/libc.so", "rb");
    unsigned char elf_magic[4];
    if (!library_file)
    {
        puts("libc stdio/file seek check failed");
        free(copy);
        return 10;
    }
    bool file_ok =
        fread(elf_magic, 1, sizeof(elf_magic), library_file) ==
            sizeof(elf_magic) &&
        elf_magic[0] == 0x7F &&
        elf_magic[1] == 'E' &&
        elf_magic[2] == 'L' &&
        elf_magic[3] == 'F' &&
        ftell(library_file) == (long)sizeof(elf_magic) &&
        fseek(library_file, 0, SEEK_SET) == 0 &&
        fgetc(library_file) == 0x7F;
    if (fclose(library_file) != 0)
        file_ok = false;
    if (!file_ok)
    {
        puts("libc stdio/file seek check failed");
        free(copy);
        return 10;
    }

    FT_Library font_library = 0;
    if (FT_Init_FreeType(&font_library) != 0)
    {
        puts("FreeType initialization failed");
        free(copy);
        return 11;
    }
    FT_Done_FreeType(font_library);

    int png_result = test_png_decode();
    if (png_result != 0)
    {
        printf("PNG decode failed at stage %d\n", png_result);
        free(copy);
        return 12;
    }

    z_stream decompressor = {0};
    int inflate_result = inflateInit(&decompressor);
    if (inflate_result != Z_OK)
    {
        printf("zlib inflateInit failed: %d\n", inflate_result);
        free(copy);
        return 13;
    }
    if (inflateEnd(&decompressor) != Z_OK)
    {
        puts("zlib inflateEnd failed");
        free(copy);
        return 12;
    }

    printf(
        "C ELF via libc/png/zlib/freetype: %s; pid=%d; value=%lu; size=%zu\n",
        copy,
        getpid(),
        initialized_data,
        sizeof(source)
    );
    free(copy);
    return 0;
}
