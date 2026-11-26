
#include <stdint.h>
#include <stdbool.h>

#include "acpi.h"
#include "apic.h"
#include "idt.h"
#include "paging.h"

#define IA32_APIC_BASE_MSR 0x1B

#define APIC_BASE_ENABLE   (1ULL << 11)
#define APIC_BASE_X2APIC   (1ULL << 10)
#define APIC_BASE_ADDRESS  0x0000000FFFFFF000ULL

#define LAPIC_MMIO_SIZE    4096

#define LAPIC_ID           0x020
#define LAPIC_TPR          0x080
#define LAPIC_EOI          0x0B0
#define LAPIC_SVR          0x0F0
#define LAPIC_LVT_TIMER    0x320
#define LAPIC_LVT_THERMAL  0x330
#define LAPIC_LVT_PERF     0x340
#define LAPIC_LVT_LINT0    0x350
#define LAPIC_LVT_LINT1    0x360
#define LAPIC_LVT_ERROR    0x370

#define IOAPIC_MMIO_SIZE   4096

#define IOAPIC_REG_SELECT  0x00
#define IOAPIC_REG_WINDOW  0x10
#define IOAPIC_REG_VERSION 0x01
#define IOAPIC_REG_REDIR   0x10

#define IOAPIC_MASK        (1U << 16)
#define IOAPIC_ACTIVE_LOW  (1U << 13)
#define IOAPIC_LEVEL       (1U << 15)

#define IOAPIC_MAX_REDIR_ENTRIES 120

#define APIC_SPURIOUS_VECTOR 0xFF
#define APIC_IRQ_VECTOR_BASE 32

extern void apic_spurious_stub(void);

typedef struct
{
    volatile uint32_t *registers;
    uint32_t gsi_base;
    uint32_t redirection_count;
} ioapic_t;

static volatile uint32_t *lapic;

static ioapic_t ioapics[ACPI_MAX_IOAPICS];

static uint32_t ioapic_count;
static uint8_t bsp_apic_id;


/* ---------------------------------------------------------
   MSR ACCESS
   --------------------------------------------------------- */

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t low;
    uint32_t high;

    __asm__ volatile (
        "rdmsr"
        : "=a"(low), "=d"(high)
        : "c"(msr)
    );

    return ((uint64_t)high << 32) | low;
}

static inline void wrmsr(uint32_t msr, uint64_t value)
{
    uint32_t low = (uint32_t)value;
    uint32_t high = (uint32_t)(value >> 32);

    __asm__ volatile (
        "wrmsr"
        :
        : "c"(msr), "a"(low), "d"(high)
        : "memory"
    );
}


/* ---------------------------------------------------------
   CPU FEATURES
   --------------------------------------------------------- */

static bool cpu_has_apic(void)
{
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;

    __asm__ volatile (
        "cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(1), "c"(0)
    );

    return (edx & (1U << 9)) != 0;
}


/* ---------------------------------------------------------
   LOCAL APIC
   --------------------------------------------------------- */

static inline void lapic_write(uint32_t offset, uint32_t value)
{
    lapic[offset / 4] = value;
}

static inline uint32_t lapic_read(uint32_t offset)
{
    return lapic[offset / 4];
}


/* ---------------------------------------------------------
   I/O APIC
   --------------------------------------------------------- */

static uint32_t ioapic_read(ioapic_t *io, uint8_t reg)
{
    io->registers[IOAPIC_REG_SELECT / 4] = reg;

    return io->registers[IOAPIC_REG_WINDOW / 4];
}

static void ioapic_write(ioapic_t *io, uint8_t reg, uint32_t value)
{
    io->registers[IOAPIC_REG_SELECT / 4] = reg;
    io->registers[IOAPIC_REG_WINDOW / 4] = value;
}


/* ---------------------------------------------------------
   FIND I/O APIC FOR A GSI
   --------------------------------------------------------- */

static ioapic_t *find_ioapic(uint32_t gsi)
{
    for (uint32_t i = 0; i < ioapic_count; i++)
    {
        ioapic_t *io = &ioapics[i];

        if (gsi >= io->gsi_base &&
            gsi < io->gsi_base + io->redirection_count)
        {
            return io;
        }
    }

    return 0;
}


/* ---------------------------------------------------------
   ISA IRQ ROUTING
   --------------------------------------------------------- */

static bool get_isa_route(
    uint8_t irq,
    uint32_t *gsi,
    uint16_t *flags
)
{
    const acpi_madt_info_t *madt = acpi_get_madt();

    if (!madt || !gsi || !flags || irq >= 16)
        return false;

    /* Default ISA routing:
       Active-high, edge-triggered. */

    *gsi = irq;
    *flags = 0;

    for (uint32_t i = 0; i < madt->iso_count; i++)
    {
        if (madt->isos[i].source == irq)
        {
            *gsi = madt->isos[i].gsi;
            *flags = madt->isos[i].flags;
            break;
        }
    }

    return true;
}


/* ---------------------------------------------------------
   INITIALIZE I/O APICs
   --------------------------------------------------------- */

static bool configure_ioapics(
    const acpi_madt_info_t *madt
)
{
    if (!madt)
        return false;

    if (madt->ioapic_count > ACPI_MAX_IOAPICS)
        return false;

    ioapic_count = madt->ioapic_count;

    for (uint32_t i = 0; i < ioapic_count; i++)
    {
        ioapic_t *io = &ioapics[i];

        uint64_t physical_address =
            madt->ioapics[i].address;

        if (!physical_address)
            return false;

        /*
         * Map the physical I/O APIC MMIO region.
         * ioremap must create an uncached mapping.
         */

        io->registers = (volatile uint32_t *)
            ioremap(physical_address, IOAPIC_MMIO_SIZE);

        if (!io->registers)
            return false;

        io->gsi_base = madt->ioapics[i].gsi_base;

        /* Read I/O APIC version. */

        uint32_t version =
            ioapic_read(io, IOAPIC_REG_VERSION);

        io->redirection_count =
            ((version >> 16) & 0xFF) + 1;

        if (io->redirection_count == 0 ||
            io->redirection_count > IOAPIC_MAX_REDIR_ENTRIES)
        {
            return false;
        }

        /*
         * Mask all redirection entries.
         * No external IRQ should be enabled yet.
         */

        for (uint32_t pin = 0;
             pin < io->redirection_count;
             pin++)
        {
            uint8_t low_reg =
                IOAPIC_REG_REDIR + (uint8_t)(pin * 2);

            uint8_t high_reg = low_reg + 1;

            ioapic_write(io, low_reg, IOAPIC_MASK);
            ioapic_write(io, high_reg, 0);
        }
    }

    return true;
}


/* ---------------------------------------------------------
   APIC INITIALIZATION
   --------------------------------------------------------- */

bool apic_init(uint64_t rsdp_physical, uint64_t hhdm_offset)
{
    (void)hhdm_offset;

    /* Check CPU APIC support. */

    if (!cpu_has_apic())
        return false;

    /* Initialize ACPI and parse the MADT. */

    if (!acpi_init(rsdp_physical, hhdm_offset))
        return false;

    const acpi_madt_info_t *madt = acpi_get_madt();

    if (!madt)
        return false;

    /* Read the APIC base MSR. */

    uint64_t apic_base = rdmsr(IA32_APIC_BASE_MSR);

    /* This driver currently supports xAPIC only. */

    if (apic_base & APIC_BASE_X2APIC)
        return false;

    uint64_t physical_base = madt->lapic_address;

    if (!physical_base)
        return false;

    /*
     * Configure the Local APIC physical address.
     * Preserve unrelated MSR bits.
     */

    apic_base &= ~APIC_BASE_ADDRESS;
    apic_base |= physical_base & APIC_BASE_ADDRESS;
    apic_base |= APIC_BASE_ENABLE;

    wrmsr(IA32_APIC_BASE_MSR, apic_base);

    /*
     * Map Local APIC MMIO.
     * Do NOT access it through the HHDM directly.
     */

    lapic = (volatile uint32_t *)
        ioremap(physical_base, LAPIC_MMIO_SIZE);

    if (!lapic)
        return false;

    /* Set task priority to zero. */

    lapic_write(LAPIC_TPR, 0);

    /* Get the bootstrap processor's APIC ID. */

    bsp_apic_id =
        (lapic_read(LAPIC_ID) >> 24) & 0xFF;

    /*
     * Mask local interrupt sources until their
     * corresponding drivers are ready.
     */

    lapic_write(LAPIC_LVT_TIMER,   IOAPIC_MASK);
    lapic_write(LAPIC_LVT_THERMAL, IOAPIC_MASK);
    lapic_write(LAPIC_LVT_PERF,    IOAPIC_MASK);
    lapic_write(LAPIC_LVT_LINT0,   IOAPIC_MASK);
    lapic_write(LAPIC_LVT_LINT1,   IOAPIC_MASK);
    lapic_write(LAPIC_LVT_ERROR,   IOAPIC_MASK);

    /* Install the spurious interrupt handler. */

    idt_set_gate(
        APIC_SPURIOUS_VECTOR,
        apic_spurious_stub
    );

    /* Enable the Local APIC with vector 0xFF. */

    lapic_write(
        LAPIC_SVR,
        APIC_SPURIOUS_VECTOR | (1U << 8)
    );

    /* Initialize and mask all I/O APIC inputs. */

    if (!configure_ioapics(madt))
        return false;

    return true;
}


/* ---------------------------------------------------------
   END OF INTERRUPT
   --------------------------------------------------------- */

void apic_eoi(void)
{
    if (lapic)
    {
        lapic_write(LAPIC_EOI, 0);
    }
}


/* ---------------------------------------------------------
   ENABLE IRQ
   --------------------------------------------------------- */

bool apic_irq_enable(uint8_t irq)
{
    if (!lapic || irq >= 16)
        return false;

    uint32_t gsi;
    uint16_t flags;

    if (!get_isa_route(irq, &gsi, &flags))
        return false;

    uint8_t polarity = flags & 0x3;
    uint8_t trigger = (flags >> 2) & 0x3;

    /* ACPI encoding 2 is reserved. */

    if (polarity == 2 || trigger == 2)
        return false;

    ioapic_t *io = find_ioapic(gsi);

    if (!io)
        return false;

    uint32_t pin = gsi - io->gsi_base;

    uint8_t low_reg =
        IOAPIC_REG_REDIR + (uint8_t)(pin * 2);

    uint32_t low = APIC_IRQ_VECTOR_BASE + irq;

    /* Active-low polarity. */

    if (polarity == 3)
        low |= IOAPIC_ACTIVE_LOW;

    /* Level-triggered interrupt. */

    if (trigger == 3)
        low |= IOAPIC_LEVEL;

    uint32_t high =
        (uint32_t)bsp_apic_id << 24;

    /*
     * Keep the entry masked while updating
     * both halves of the redirection entry.
     */

    ioapic_write(io, low_reg, low | IOAPIC_MASK);
    ioapic_write(io, low_reg + 1, high);

    /* Unmask the interrupt. */

    ioapic_write(io, low_reg, low);

    return true;
}


/* ---------------------------------------------------------
   DISABLE IRQ
   --------------------------------------------------------- */

void apic_irq_disable(uint8_t irq)
{
    if (!lapic || irq >= 16)
        return;

    uint32_t gsi;
    uint16_t flags;

    if (!get_isa_route(irq, &gsi, &flags))
        return;

    ioapic_t *io = find_ioapic(gsi);

    if (!io)
        return;

    uint32_t pin = gsi - io->gsi_base;

    uint8_t low_reg =
        IOAPIC_REG_REDIR + (uint8_t)(pin * 2);

    uint32_t low = ioapic_read(io, low_reg);

    ioapic_write(io, low_reg, low | IOAPIC_MASK);
}
