
#include <stdint.h>

#include "idt.h"
#include "gdt.h"

#define IDT_INTERRUPT_GATE        0x8E
#define IDT_USER_INTERRUPT_GATE   0xEE

typedef struct __attribute__((packed))
{
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attributes;
    uint16_t offset_middle;
    uint32_t offset_high;
    uint32_t reserved;
} idt_entry_t;

typedef struct __attribute__((packed))
{
    uint16_t limit;
    uint64_t base;
} idt_pointer_t;

static idt_entry_t idt[IDT_ENTRIES];

extern void idt_load(idt_pointer_t *pointer);
extern void idt_default_handler(void);

void idt_set_gate_ist(
    uint8_t vector,
    void (*handler)(void),
    uint8_t ist
)
{
    if (ist > 7)
        return;

    uint64_t address = (uint64_t)handler;

    idt[vector].offset_low = address & 0xFFFF;
    idt[vector].selector = GDT_KERNEL_CODE;

    idt[vector].ist = ist & 0x07;

    idt[vector].type_attributes = IDT_INTERRUPT_GATE;

    idt[vector].offset_middle = (address >> 16) & 0xFFFF;
    idt[vector].offset_high = (address >> 32) & 0xFFFFFFFF;

    idt[vector].reserved = 0;
}

void idt_set_gate(uint8_t vector, void (*handler)(void))
{
    idt_set_gate_ist(vector, handler, 0);
}

void idt_set_user_gate(uint8_t vector, void (*handler)(void))
{
    idt_set_gate_ist(vector, handler, 0);
    idt[vector].type_attributes = IDT_USER_INTERRUPT_GATE;
}

void idt_init(void)
{
    for (uint16_t i = 0; i < IDT_ENTRIES; i++)
    {
        idt_set_gate(
            i,
            idt_default_handler
        );
    }

    idt_pointer_t pointer =
    {
        .limit = sizeof(idt) - 1,
        .base = (uint64_t)idt
    };

    idt_load(&pointer);
}
