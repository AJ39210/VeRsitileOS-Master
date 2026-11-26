#include <stdint.h>

#include "spinlock.h"

void spinlock_init(spinlock_t *lock)
{
    __atomic_store_n(&lock->value, 0, __ATOMIC_RELAXED);
}

uint64_t spinlock_lock_irqsave(spinlock_t *lock)
{
    uint64_t flags;

    __asm__ volatile (
        "pushfq\n"
        "popq %0\n"
        "cli"
        : "=r"(flags)
        :
        : "memory"
    );

    while (__atomic_exchange_n(
        &lock->value,
        1,
        __ATOMIC_ACQUIRE
    ))
    {
        while (__atomic_load_n(
            &lock->value,
            __ATOMIC_RELAXED
        ))
        {
            __asm__ volatile ("pause");
        }
    }

    return flags;
}

void spinlock_unlock_irqrestore(
    spinlock_t *lock,
    uint64_t flags
)
{
    __atomic_store_n(
        &lock->value,
        0,
        __ATOMIC_RELEASE
    );

    if (flags & (1ULL << 9))
    {
        __asm__ volatile ("sti" ::: "memory");
    }
}