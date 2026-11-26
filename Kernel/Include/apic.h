#ifndef APIC_H
#define APIC_H

#include <stdint.h>
#include <stdbool.h>

bool apic_init(uint64_t rsdp_physical, uint64_t hhdm_offset);

void apic_eoi(void);

bool apic_irq_enable(uint8_t irq);
void apic_irq_disable(uint8_t irq);

#endif