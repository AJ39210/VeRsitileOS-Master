#ifndef HEAP_H
#define HEAP_H

#include <stdint.h>
#include <stdbool.h>

bool kheap_init(void);

void *kmalloc(uint64_t size);

void *kcalloc(uint64_t count, uint64_t size);

void *kmemalign(uint64_t alignment, uint64_t size);

void *krealloc(void *pointer, uint64_t new_size);

void kfree(void *pointer);

uint64_t kheap_get_used(void);
uint64_t kheap_get_free(void);
uint64_t kheap_get_committed(void);

#endif