
#include <stdint.h>
#include <stdbool.h>

#include "irq.h"
#include "apic.h"
#include "idt.h"
#include "task.h"

static irq_handler_t handlers[IRQ_COUNT];
static volatile uint64_t irq_counts[IRQ_COUNT];

/* Assembly IRQ entry points. */

extern void irq0_stub(void);
extern void irq1_stub(void);
extern void irq2_stub(void);
extern void irq3_stub(void);
extern void irq4_stub(void);
extern void irq5_stub(void);
extern void irq6_stub(void);
extern void irq7_stub(void);
extern void irq8_stub(void);
extern void irq9_stub(void);
extern void irq10_stub(void);
extern void irq11_stub(void);
extern void irq12_stub(void);
extern void irq13_stub(void);
extern void irq14_stub(void);
extern void irq15_stub(void);

static void (*irq_stubs[IRQ_COUNT])(void) =
{
    irq0_stub,
    irq1_stub,
    irq2_stub,
    irq3_stub,
    irq4_stub,
    irq5_stub,
    irq6_stub,
    irq7_stub,
    irq8_stub,
    irq9_stub,
    irq10_stub,
    irq11_stub,
    irq12_stub,
    irq13_stub,
    irq14_stub,
    irq15_stub
};

void irq_init(void)
{
    for (uint8_t i = 0; i < IRQ_COUNT; i++)
    {
        handlers[i] = 0;
        irq_counts[i] = 0;

        idt_set_gate(
            IRQ_BASE + i,
            irq_stubs[i]
        );
    }
}

bool irq_register_handler(uint8_t irq, irq_handler_t handler)
{
    if (irq >= IRQ_COUNT)
        return false;

    if (handler)
    {
        /* Register the handler before unmasking the IRQ. */

        handlers[irq] = handler;

        if (!apic_irq_enable(irq))
        {
            handlers[irq] = 0;
            return false;
        }
    }
    else
    {
        /* Mask first, then remove the handler. */

        apic_irq_disable(irq);
        handlers[irq] = 0;
    }

    return true;
}

irq_frame_t *irq_dispatch(irq_frame_t *frame)
{
    if (!frame)
        return frame;

    uint64_t vector = frame->vector;

    if (vector < IRQ_BASE ||
        vector >= IRQ_BASE + IRQ_COUNT)
        return frame;

    uint8_t irq = (uint8_t)(vector - IRQ_BASE);

    irq_counts[irq]++;

    if (handlers[irq])
        handlers[irq]();

    apic_eoi();

    if (irq == 0)
        return task_schedule_from_interrupt(frame);

    return frame;
}

uint64_t irq_get_count(uint8_t irq)
{
    if (irq >= IRQ_COUNT)
        return 0;

    return irq_counts[irq];
}
