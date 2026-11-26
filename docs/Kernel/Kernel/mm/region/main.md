# Virtual memory regions

[`region.c`](../../../../../Kernel/Kernel/mm/region/region.c) tracks reserved virtual regions and provides aligned allocation, freeing, and reservation queries. It works alongside the page-table and VMM layers; reserving a region is not the same operation as mapping physical pages.

API: [`Include/region.h`](../../../../../Kernel/Include/region.h).
