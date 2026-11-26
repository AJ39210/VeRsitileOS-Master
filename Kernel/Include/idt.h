
#ifndef IDT_H
#define IDT_H

#include <stdint.h>

#define IDT_ENTRIES 256

void idt_init(void);

void idt_set_gate(uint8_t vector, void (*handler)(void));
void idt_set_user_gate(uint8_t vector, void (*handler)(void));

void idt_set_gate_ist(
    uint8_t vector,
    void (*handler)(void),
    uint8_t ist
);

#endif
