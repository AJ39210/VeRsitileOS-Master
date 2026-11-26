
#ifndef GDT_H
#define GDT_H

#include <stdint.h>

#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_DATA   0x18
#define GDT_USER_CODE   0x20
#define GDT_TSS         0x28

#define TSS_IST_DOUBLE_FAULT 1
#define TSS_IST_NMI          2
#define TSS_IST_MACHINE_CHECK 3

typedef struct __attribute__((packed))
{
    uint32_t reserved0;

    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;

    uint64_t reserved1;

    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;

    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;

} tss_t;

void gdt_init(void);

void gdt_set_kernel_stack(uint64_t stack);

void gdt_set_ist_stack(uint8_t ist, uint64_t stack);

#endif
