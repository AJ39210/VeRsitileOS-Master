
#include <stdint.h>

#include "gdt.h"

#define GDT_ENTRIES 7
#define IST_STACK_SIZE 16384

typedef struct __attribute__((packed))
{
    uint16_t limit;
    uint64_t base;
} gdt_pointer_t;

static uint64_t gdt[GDT_ENTRIES];

static tss_t tss;

/* Dedicated exception stacks. */
static uint8_t double_fault_stack[IST_STACK_SIZE]
    __attribute__((aligned(16)));

static uint8_t nmi_stack[IST_STACK_SIZE]
    __attribute__((aligned(16)));

static uint8_t machine_check_stack[IST_STACK_SIZE]
    __attribute__((aligned(16)));

/* Assembly functions. */
extern void gdt_load(gdt_pointer_t *pointer);
extern void tss_load(uint16_t selector);

static void gdt_set_tss(uint64_t base, uint32_t limit)
{
    uint64_t low = 0;

    low |= (uint64_t)(limit & 0xFFFF);
    low |= (base & 0xFFFFFF) << 16;
    low |= (uint64_t)0x89 << 40;
    low |= (uint64_t)((limit >> 16) & 0x0F) << 48;
    low |= ((base >> 24) & 0xFF) << 56;

    gdt[5] = low;
    gdt[6] = base >> 32;
}

void gdt_set_kernel_stack(uint64_t stack)
{
    tss.rsp0 = stack;
}

void gdt_set_ist_stack(uint8_t ist, uint64_t stack)
{
    switch (ist)
    {
        case 1: tss.ist1 = stack; break;
        case 2: tss.ist2 = stack; break;
        case 3: tss.ist3 = stack; break;
        case 4: tss.ist4 = stack; break;
        case 5: tss.ist5 = stack; break;
        case 6: tss.ist6 = stack; break;
        case 7: tss.ist7 = stack; break;
        default: break;
    }
}

void gdt_init(void)
{
    for (uint32_t i = 0; i < GDT_ENTRIES; i++)
        gdt[i] = 0;

    gdt[1] = 0x00AF9A000000FFFFULL;
    gdt[2] = 0x00CF92000000FFFFULL;
    gdt[3] = 0x00CFF2000000FFFFULL;
    gdt[4] = 0x00AFFA000000FFFFULL;

    tss = (tss_t){0};

    /* Deny user-mode I/O through the TSS I/O bitmap. */
    tss.iomap_base = sizeof(tss_t);

    /* Configure dedicated exception stacks. */
    gdt_set_ist_stack(
        TSS_IST_DOUBLE_FAULT,
        (uint64_t)(double_fault_stack + sizeof(double_fault_stack))
    );

    gdt_set_ist_stack(
        TSS_IST_NMI,
        (uint64_t)(nmi_stack + sizeof(nmi_stack))
    );

    gdt_set_ist_stack(
        TSS_IST_MACHINE_CHECK,
        (uint64_t)(machine_check_stack + sizeof(machine_check_stack))
    );

    gdt_set_tss(
        (uint64_t)&tss,
        sizeof(tss_t) - 1
    );

    gdt_pointer_t pointer = {
        .limit = sizeof(gdt) - 1,
        .base = (uint64_t)gdt
    };

    gdt_load(&pointer);

    tss_load(GDT_TSS);
}
