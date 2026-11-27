# Memory management

[`mm.c`](../../../../Kernel/Kernel/mm/mm.c) coordinates initialization of the memory-management components from the Limine memory map and HHDM offset. Set `MM_IMPLEMENTATION` in `Kernel/config.cfg` to `1` for the existing C implementation or `2` for the Rust implementation. Both implementations export the same C ABI declared in `Kernel/Include/`.

- [`pmm.c`](../../../../Kernel/Kernel/mm/pmm.c) — physical page allocation/free and memory/page statistics.
- [`paging.c`](../../../../Kernel/Kernel/mm/paging.c) — initial paging support, HHDM access, page invalidation, and NX capability query.
- [`vmm.c`](../../../../Kernel/Kernel/mm/vmm.c) — address-space creation/cloning/switching, mappings, protection, translation, and user-range checks.
- [`region/region.c`](../../../../Kernel/Kernel/mm/region/region.c) — virtual address-region reservation/allocation helpers.
- [`heap/heap.c`](heap/main.md) — kernel heap allocation and statistics.
- [`sync/spinlock.c`](sync/main.md) — IRQ-safe spinlock helpers.

The matching Rust implementation lives in [`Kernel/rust/src/mm/`](../../../../Kernel/rust/src/mm/), with corresponding paging, PMM, VMM, spinlock, region, heap, and initialization modules. The selected implementation is linked exclusively; C and Rust versions are not linked together.

`kmain()` treats `mm_init()` failure as fatal and logs PMM statistics after successful initialization. Memory-management APIs are declared in [`Include/`](../../../../Kernel/Include/): `mm.h`, `pmm.h`, `paging.h`, `vmm.h`, `region.h`, `heap.h`, and `spinlock.h`.
