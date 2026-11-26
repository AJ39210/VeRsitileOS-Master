#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>

typedef struct
{
    volatile uint32_t value;
} spinlock_t;

void spinlock_init(spinlock_t *lock);

uint64_t spinlock_lock_irqsave(spinlock_t *lock);

void spinlock_unlock_irqrestore(
    spinlock_t *lock,
    uint64_t flags
);

#endif