#include <stdbool.h>
#include <stdint.h>

#include "errno.h"
#include "internal.h"
#include "limits.h"
#include "stdlib.h"
#include "syscall.h"
#include "string.h"
#include "unistd.h"

#define MALLOC_MAX_ARENAS 64
#define MALLOC_ARENA_MIN_SIZE 65536
#define MALLOC_ALIGNMENT 16
#define MALLOC_BLOCK_MAGIC 0x564F534D414C4C4FULL

typedef struct malloc_block malloc_block_t;

struct __attribute__((aligned(MALLOC_ALIGNMENT))) malloc_block
{
    size_t size;
    size_t requested;
    uint64_t magic;
    bool free;
    malloc_block_t *previous;
    malloc_block_t *next;
};

typedef struct
{
    uint8_t *base;
    size_t size;
    malloc_block_t *first;
} malloc_arena_t;

static malloc_arena_t arenas[MALLOC_MAX_ARENAS];

static size_t align_allocation(size_t size)
{
    if (size > SIZE_MAX - (MALLOC_ALIGNMENT - 1))
        return 0;
    return (size + MALLOC_ALIGNMENT - 1) & ~(MALLOC_ALIGNMENT - 1);
}

static malloc_arena_t *create_arena(size_t size)
{
    if (size > SIZE_MAX - sizeof(malloc_block_t))
    {
        errno = ENOMEM;
        return 0;
    }

    size_t arena_size = size + sizeof(malloc_block_t);
    if (arena_size < MALLOC_ARENA_MIN_SIZE)
        arena_size = MALLOC_ARENA_MIN_SIZE;

    size_t slot;
    for (slot = 0; slot < MALLOC_MAX_ARENAS; slot++)
    {
        if (!arenas[slot].base)
            break;
    }
    if (slot == MALLOC_MAX_ARENAS)
    {
        errno = ENOMEM;
        return 0;
    }

    int64_t result = libc_syscall1(SYS_ALLOC, arena_size);
    if (result < 0)
    {
        libc_set_errno_from_result(result);
        return 0;
    }

    malloc_arena_t *arena = &arenas[slot];
    arena->base = (uint8_t *)(uintptr_t)result;
    arena->size = arena_size;
    arena->first = (malloc_block_t *)arena->base;
    *arena->first = (malloc_block_t){
        .size = arena_size - sizeof(malloc_block_t),
        .requested = 0,
        .magic = MALLOC_BLOCK_MAGIC,
        .free = true,
        .previous = 0,
        .next = 0
    };
    return arena;
}

static malloc_block_t *find_free_block(size_t size)
{
    for (size_t i = 0; i < MALLOC_MAX_ARENAS; i++)
    {
        for (malloc_block_t *block = arenas[i].first;
             block;
             block = block->next)
        {
            if (block->free && block->size >= size)
                return block;
        }
    }
    return 0;
}

static void split_block(malloc_block_t *block, size_t size)
{
    if (block->size - size <
        sizeof(malloc_block_t) + MALLOC_ALIGNMENT)
        return;

    malloc_block_t *next = (malloc_block_t *)(
        (uint8_t *)(block + 1) + size
    );
    *next = (malloc_block_t){
        .size = block->size - size - sizeof(malloc_block_t),
        .requested = 0,
        .magic = MALLOC_BLOCK_MAGIC,
        .free = true,
        .previous = block,
        .next = block->next
    };
    if (next->next)
        next->next->previous = next;
    block->next = next;
    block->size = size;
}

static malloc_arena_t *arena_for_block(const malloc_block_t *block)
{
    uintptr_t address = (uintptr_t)block;
    for (size_t i = 0; i < MALLOC_MAX_ARENAS; i++)
    {
        uintptr_t start = (uintptr_t)arenas[i].base;
        if (start && address >= start &&
            address - start < arenas[i].size)
            return &arenas[i];
    }
    return 0;
}

void *malloc(size_t size)
{
    if (size == 0)
        size = 1;
    size_t aligned = align_allocation(size);
    if (!aligned)
    {
        errno = ENOMEM;
        return 0;
    }

    malloc_block_t *block = find_free_block(aligned);
    if (!block)
    {
        if (!create_arena(aligned))
            return 0;
        block = find_free_block(aligned);
    }
    if (!block)
    {
        errno = ENOMEM;
        return 0;
    }

    split_block(block, aligned);
    block->free = false;
    block->requested = size;
    return block + 1;
}

void *calloc(size_t count, size_t size)
{
    if (size && count > SIZE_MAX / size)
    {
        errno = ENOMEM;
        return 0;
    }
    size_t total = count * size;
    void *pointer = malloc(total);
    if (pointer)
        memset(pointer, 0, total);
    return pointer;
}

void free(void *pointer)
{
    if (!pointer)
        return;

    malloc_block_t *block = (malloc_block_t *)pointer - 1;
    if (block->magic != MALLOC_BLOCK_MAGIC || block->free)
    {
        errno = EINVAL;
        return;
    }

    block->free = true;
    block->requested = 0;
    if (block->next && block->next->free)
    {
        malloc_block_t *next = block->next;
        block->size += sizeof(malloc_block_t) + next->size;
        block->next = next->next;
        if (block->next)
            block->next->previous = block;
    }
    if (block->previous && block->previous->free)
    {
        block = block->previous;
        block->size += sizeof(malloc_block_t) + block->next->size;
        block->next = block->next->next;
        if (block->next)
            block->next->previous = block;
    }

    malloc_arena_t *arena = arena_for_block(block);
    if (!arena || block != arena->first ||
        block->next || block->size != arena->size - sizeof(*block))
        return;

    size_t active_arenas = 0;
    for (size_t i = 0; i < MALLOC_MAX_ARENAS; i++)
        active_arenas += arenas[i].base != 0;
    if (active_arenas <= 1)
        return;

    int64_t result = libc_syscall1(
        SYS_FREE,
        (uint64_t)(uintptr_t)arena->base
    );
    if (result < 0)
    {
        libc_set_errno_from_result(result);
        return;
    }
    *arena = (malloc_arena_t){0};
}

void *realloc(void *pointer, size_t size)
{
    if (!pointer)
        return malloc(size);
    if (size == 0)
    {
        free(pointer);
        return 0;
    }

    malloc_block_t *block = (malloc_block_t *)pointer - 1;
    if (block->magic != MALLOC_BLOCK_MAGIC || block->free)
    {
        errno = EINVAL;
        return 0;
    }
    size_t old_size = block->requested;
    size_t aligned = align_allocation(size);
    if (!aligned)
    {
        errno = ENOMEM;
        return 0;
    }
    if (aligned <= block->size)
    {
        block->requested = size;
        return pointer;
    }

    void *replacement = malloc(size);
    if (!replacement)
        return 0;
    memcpy(replacement, pointer, old_size < size ? old_size : size);
    free(pointer);
    return replacement;
}

_Noreturn void exit(int status)
{
    _exit(status);
}

long strtol(const char *string, char **end, int base)
{
    if (!string)
    {
        errno = EINVAL;
        return 0;
    }

    const char *cursor = string;
    while (*cursor == ' ' || (*cursor >= '\t' && *cursor <= '\r'))
        cursor++;
    bool negative = *cursor == '-';

    int saved_errno = errno;
    errno = 0;
    char *parse_end;
    unsigned long value = strtoul(string, &parse_end, base);
    int parse_errno = errno;
    bool overflow = parse_errno == ERANGE;
    if (end)
        *end = parse_end;
    if (parse_errno == 0)
        errno = saved_errno;

    if (parse_end == string)
        return 0;
    if (overflow)
    {
        errno = ERANGE;
        return negative ? LONG_MIN : LONG_MAX;
    }

    if (negative)
    {
        value = 0UL - value;
        unsigned long negative_limit = (unsigned long)LONG_MAX + 1UL;
        if (value > negative_limit)
        {
            errno = ERANGE;
            return LONG_MIN;
        }
        if (value == negative_limit)
            return LONG_MIN;
        return -(long)value;
    }
    if (value > (unsigned long)LONG_MAX)
    {
        errno = ERANGE;
        return LONG_MAX;
    }
    return (long)value;
}

int atoi(const char *string)
{
    return (int)strtol(string, 0, 10);
}

unsigned long strtoul(const char *string, char **end, int base)
{
    const char *cursor = string;
    while (*cursor == ' ' || (*cursor >= '\t' && *cursor <= '\r'))
        cursor++;

    bool negative = false;
    if (*cursor == '+' || *cursor == '-')
    {
        negative = *cursor == '-';
        cursor++;
    }

    if (base == 0)
    {
        if (cursor[0] == '0' && (cursor[1] == 'x' || cursor[1] == 'X'))
        {
            base = 16;
            cursor += 2;
        }
        else
            base = cursor[0] == '0' ? 8 : 10;
    }
    else if (base == 16 && cursor[0] == '0' &&
             (cursor[1] == 'x' || cursor[1] == 'X'))
        cursor += 2;

    if (base < 2 || base > 36)
    {
        errno = EINVAL;
        if (end)
            *end = (char *)string;
        return 0;
    }

    unsigned long value = 0;
    bool any = false;
    bool overflow = false;
    for (;; cursor++)
    {
        unsigned char character = (unsigned char)*cursor;
        unsigned digit;
        if (character >= '0' && character <= '9')
            digit = character - '0';
        else if (character >= 'a' && character <= 'z')
            digit = character - 'a' + 10;
        else if (character >= 'A' && character <= 'Z')
            digit = character - 'A' + 10;
        else
            break;
        if (digit >= (unsigned)base)
            break;
        any = true;
        if (value > (ULONG_MAX - digit) / (unsigned)base)
        {
            errno = ERANGE;
            overflow = true;
            value = ULONG_MAX;
            while (true)
            {
                character = (unsigned char)*cursor;
                if (character >= '0' && character <= '9')
                    digit = character - '0';
                else if (character >= 'a' && character <= 'z')
                    digit = character - 'a' + 10;
                else if (character >= 'A' && character <= 'Z')
                    digit = character - 'A' + 10;
                else
                    break;
                if (digit >= (unsigned)base)
                    break;
                cursor++;
            }
            break;
        }
        value = value * (unsigned)base + digit;
    }

    if (!any)
    {
        if (end)
            *end = (char *)string;
        return 0;
    }
    if (end)
        *end = (char *)cursor;
    if (overflow)
        return ULONG_MAX;
    return negative ? 0UL - value : value;
}

int abs(int value)
{
    return value < 0 ? -value : value;
}

long labs(long value)
{
    return value < 0 ? -value : value;
}

_Noreturn void abort(void)
{
    _exit(127);
}

void qsort(
    void *base,
    size_t count,
    size_t size,
    int (*compare)(const void *, const void *)
)
{
    if (!base || !compare || !size)
        return;
    unsigned char *bytes = base;
    for (size_t i = 1; i < count; i++)
    {
        size_t j = i;
        while (j && compare(bytes + (j - 1) * size,
                            bytes + j * size) > 0)
        {
            for (size_t k = 0; k < size; k++)
            {
                unsigned char value = bytes[(j - 1) * size + k];
                bytes[(j - 1) * size + k] = bytes[j * size + k];
                bytes[j * size + k] = value;
            }
            j--;
        }
    }
}

void *bsearch(
    const void *key,
    const void *base,
    size_t count,
    size_t size,
    int (*compare)(const void *, const void *)
)
{
    if (!key || !base || !compare || !size)
        return 0;
    const unsigned char *bytes = base;
    size_t low = 0;
    size_t high = count;
    while (low < high)
    {
        size_t middle = low + (high - low) / 2;
        const void *element = bytes + middle * size;
        int order = compare(key, element);
        if (order < 0)
            high = middle;
        else if (order > 0)
            low = middle + 1;
        else
            return (void *)element;
    }
    return 0;
}

static unsigned random_state = 1;

int rand(void)
{
    random_state = random_state * 1103515245U + 12345U;
    return (int)((random_state >> 16) & 0x7FFF);
}

void srand(unsigned seed)
{
    random_state = seed;
}
