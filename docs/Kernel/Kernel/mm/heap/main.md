# Kernel heap

[`heap.c`](../../../../../Kernel/Kernel/mm/heap/heap.c) implements kernel dynamic allocation on top of the lower-level memory facilities. It initializes the heap, frees allocations, and reports used, free, and committed memory totals.

Framebuffer software surfaces and other kernel components can use heap allocation after memory-manager initialization. The heap is distinct from user-process allocation/mapping.

API: [`Include/heap.h`](../../../../../Kernel/Include/heap.h).
