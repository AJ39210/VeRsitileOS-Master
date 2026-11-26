
#include <stdint.h>
#include "pic.h"

#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1

#define PIC_EOI      0x20

static uint8_t master_mask = 0xFF;
static uint8_t slave_mask  = 0xFF;

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;

    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));

    return value;
}

static void io_wait(void)
{
    outb(0x80, 0);
}

void pic_remap(uint8_t master_offset, uint8_t slave_offset)
{
    uint8_t master_saved = inb(PIC1_DATA);
    uint8_t slave_saved  = inb(PIC2_DATA);

    outb(PIC1_COMMAND, 0x11);
    io_wait();

    outb(PIC2_COMMAND, 0x11);
    io_wait();

    outb(PIC1_DATA, master_offset);
    io_wait();

    outb(PIC2_DATA, slave_offset);
    io_wait();

    outb(PIC1_DATA, 0x04);
    io_wait();

    outb(PIC2_DATA, 0x02);
    io_wait();

    outb(PIC1_DATA, 0x01);
    io_wait();

    outb(PIC2_DATA, 0x01);
    io_wait();

    master_mask = master_saved;
    slave_mask  = slave_saved;

    outb(PIC1_DATA, master_mask);
    outb(PIC2_DATA, slave_mask);
}

void pic_mask_irq(uint8_t irq)
{
    if (irq >= 16)
        return;

    if (irq < 8)
    {
        master_mask |= (1u << irq);
        outb(PIC1_DATA, master_mask);
    }
    else
    {
        irq -= 8;

        slave_mask |= (1u << irq);
        outb(PIC2_DATA, slave_mask);
    }
}

void pic_unmask_irq(uint8_t irq)
{
    if (irq >= 16)
        return;

    if (irq < 8)
    {
        master_mask &= ~(1u << irq);
        outb(PIC1_DATA, master_mask);
    }
    else
    {
        irq -= 8;

        slave_mask &= ~(1u << irq);
        outb(PIC2_DATA, slave_mask);

        master_mask &= ~(1u << 2);
        outb(PIC1_DATA, master_mask);
    }
}

void pic_send_eoi(uint8_t irq)
{
    if (irq >= 8)
        outb(PIC2_COMMAND, PIC_EOI);

    outb(PIC1_COMMAND, PIC_EOI);
}

void pic_disable(void)
{
    master_mask = 0xFF;
    slave_mask  = 0xFF;

    outb(PIC1_DATA, master_mask);
    outb(PIC2_DATA, slave_mask);

    /* Switch a PC-compatible IMCR to APIC mode. */
    outb(0x22, 0x70);
    outb(0x23, 0x01);
}
