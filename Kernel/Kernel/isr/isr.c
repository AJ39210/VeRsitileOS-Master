
#include <stdint.h>

#include "isr.h"
#include "idt.h"
#include "gdt.h"
#include "panic.h"
#include "security.h"

extern void (*isr_stub_table[ISR_COUNT])(void);

void isr_init(void)
{
    for (uint8_t i = 0; i < ISR_COUNT; i++)
    {
        uint8_t ist = 0;

        if (i == 8)
            ist = TSS_IST_DOUBLE_FAULT;

        if (i == 2)
            ist = TSS_IST_NMI;

        if (i == 18)
            ist = TSS_IST_MACHINE_CHECK;

        idt_set_gate_ist(
            i,
            isr_stub_table[i],
            ist
        );
    }
}

void isr_dispatch(isr_frame_t *frame)
{
    if (!frame)
        kernel_panic("CPU exception without a saved register frame");

    if (frame->vector == 14 &&
        security_is_write_protection_fault(frame->error_code))
    {
        uint64_t fault_address;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(fault_address));
        security_panic_write_protection_fault(
            fault_address,
            frame->error_code,
            frame->rip
        );
    }

    kernel_panic_exception(frame->vector, frame->error_code, frame->rip);
}
