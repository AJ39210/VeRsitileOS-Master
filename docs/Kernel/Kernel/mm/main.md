# Memory management

[`mm.c`](../../../../Kernel/Kernel/mm/mm.c) coordinates initialization of the memory-management components from the Limine memory map and HHDM offset.

- [`pmm.c`](../../../../Kernel/Kernel/mm/pmm.c) — physical page allocation/free and memory/page statistics.
- [`paging.c`](../../../../Kernel/Kernel/mm/paging.c) — initial paging support, HHDM access, page invalidation, and NX capability query.
- [`vmm.c`](../../../../Kernel/Kernel/mm/vmm.c) — address-space creation/cloning/switching, mappings, protection, translation, and user-range checks.
- [`region/region.c`](../../../../Kernel/Kernel/mm/region/region.c) — virtual address-region reservation/allocation helpers.
- [`heap/heap.c`](heap/main.md) — kernel heap allocation and statistics.
- [`sync/spinlock.c`](sync/main.md) — IRQ-safe spinlock helpers.

`kmain()` treats `mm_init()` failure as fatal and logs PMM statistics after successful initialization. Memory-management APIs are declared in [`Include/`](../../../../Kernel/Include/): `mm.h`, `pmm.h`, `paging.h`, `vmm.h`, `region.h`, `heap.h`, and `spinlock.h`.
