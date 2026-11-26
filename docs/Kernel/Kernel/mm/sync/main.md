# Memory synchronization

[`spinlock.c`](../../../../../Kernel/Kernel/mm/sync/spinlock.c) implements spinlocks and IRQ-save/restore lock operations. The IRQ-save form preserves interrupt state while holding a lock so an interrupt handler cannot deadlock against code interrupted while holding that same lock.

Use the public lock type and helpers from [`Include/spinlock.h`](../../../../../Kernel/Include/spinlock.h).
