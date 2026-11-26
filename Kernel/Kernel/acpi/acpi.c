#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "acpi.h"

#define ACPI_RSDP_SIGNATURE 0x2052545020445352ULL
#define ACPI_MAX_TABLE_SIZE (1024 * 1024)

typedef struct __attribute__((packed))
{
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;

    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
} rsdp_t;

typedef struct __attribute__((packed))
{
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} sdt_header_t;

typedef struct __attribute__((packed))
{
    sdt_header_t header;
    uint32_t entries[];
} rsdt_t;

typedef struct __attribute__((packed))
{
    sdt_header_t header;
    uint64_t entries[];
} xsdt_t;

typedef struct __attribute__((packed))
{
    sdt_header_t header;
    uint32_t lapic_address;
    uint32_t flags;
} madt_t;

typedef struct __attribute__((packed))
{
    uint8_t type;
    uint8_t length;
} madt_entry_t;

typedef struct __attribute__((packed))
{
    madt_entry_t header;
    uint8_t processor_id;
    uint8_t apic_id;
    uint32_t flags;
} madt_lapic_entry_t;

typedef struct __attribute__((packed))
{
    madt_entry_t header;
    uint8_t id;
    uint8_t reserved;
    uint32_t address;
    uint32_t gsi_base;
} madt_ioapic_entry_t;

typedef struct __attribute__((packed))
{
    madt_entry_t header;
    uint8_t bus;
    uint8_t source;
    uint32_t gsi;
    uint16_t flags;
} madt_iso_entry_t;

typedef struct __attribute__((packed))
{
    madt_entry_t header;
    uint16_t reserved;
    uint64_t address;
} madt_lapic_override_t;

static uint64_t hhdm_offset;
static acpi_madt_info_t madt_info;

/*
 * Physical ACPI table addresses are converted to HHDM
 * virtual addresses.
 *
 * The RSDP itself is NOT passed through this function
 * when using Limine base revision 4.
 */
static void *phys_to_virt(uint64_t physical)
{
    return (void *)(physical + hhdm_offset);
}

static uint8_t checksum(const void *data, uint32_t length)
{
    const uint8_t *bytes = data;
    uint8_t sum = 0;

    for (uint32_t i = 0; i < length; i++)
        sum = (uint8_t)(sum + bytes[i]);

    return sum;
}

static bool signature_is(const char *a, const char *b)
{
    for (uint32_t i = 0; i < 4; i++)
    {
        if (a[i] != b[i])
            return false;
    }

    return true;
}

static bool valid_sdt_header(const sdt_header_t *header)
{
    if (!header)
        return false;

    if (header->length < sizeof(sdt_header_t))
        return false;

    if (header->length > ACPI_MAX_TABLE_SIZE)
        return false;

    return true;
}

static madt_t *find_madt(uint64_t root_physical, bool xsdt_mode)
{
    if (!root_physical)
        return NULL;

    sdt_header_t *root = phys_to_virt(root_physical);

    if (!valid_sdt_header(root))
        return NULL;

    if (xsdt_mode)
    {
        if (!signature_is(root->signature, "XSDT"))
            return NULL;
    }
    else
    {
        if (!signature_is(root->signature, "RSDT"))
            return NULL;
    }

    if (checksum(root, root->length) != 0)
        return NULL;

    uint32_t entry_size = xsdt_mode ? 8 : 4;
    uint32_t entries_length =
        root->length - sizeof(sdt_header_t);

    if (entries_length % entry_size != 0)
        return NULL;

    uint32_t count = entries_length / entry_size;

    for (uint32_t i = 0; i < count; i++)
    {
        uint64_t address;

        if (xsdt_mode)
            address = ((xsdt_t *)root)->entries[i];
        else
            address = ((rsdt_t *)root)->entries[i];

        if (!address)
            continue;

        sdt_header_t *table = phys_to_virt(address);

        if (!valid_sdt_header(table))
            continue;

        if (!signature_is(table->signature, "APIC"))
            continue;

        if (checksum(table, table->length) != 0)
            return NULL;

        return (madt_t *)table;
    }

    return NULL;
}

static bool parse_madt(madt_t *madt)
{
    if (!madt)
        return false;

    if (madt->header.length < sizeof(madt_t))
        return false;

    madt_info.lapic_address = madt->lapic_address;

    uint32_t remaining =
        madt->header.length - sizeof(madt_t);

    uint8_t *position =
        (uint8_t *)madt + sizeof(madt_t);

    while (remaining >= sizeof(madt_entry_t))
    {
        madt_entry_t *entry = (madt_entry_t *)position;

        if (entry->length < sizeof(madt_entry_t))
            return false;

        if (entry->length > remaining)
            return false;

        switch (entry->type)
        {
            /* I/O APIC entry */
            case 1:
            {
                if (entry->length < sizeof(madt_ioapic_entry_t))
                    return false;

                if (madt_info.ioapic_count >= ACPI_MAX_IOAPICS)
                    return false;

                madt_ioapic_entry_t *io =
                    (madt_ioapic_entry_t *)entry;

                if (!io->address)
                    return false;

                acpi_ioapic_t *out =
                    &madt_info.ioapics[madt_info.ioapic_count++];

                out->id = io->id;
                out->address = io->address;
                out->gsi_base = io->gsi_base;

                break;
            }

            /* Interrupt source override */
            case 2:
            {
                if (entry->length < sizeof(madt_iso_entry_t))
                    return false;

                madt_iso_entry_t *iso =
                    (madt_iso_entry_t *)entry;

                /* Only ISA bus overrides are handled. */
                if (iso->bus != 0)
                    break;

                if (madt_info.iso_count >= ACPI_MAX_ISO)
                    return false;

                acpi_iso_t *out =
                    &madt_info.isos[madt_info.iso_count++];

                out->source = iso->source;
                out->gsi = iso->gsi;
                out->flags = iso->flags;

                break;
            }

            /* Local APIC address override */
            case 5:
            {
                if (entry->length < sizeof(madt_lapic_override_t))
                    return false;

                madt_lapic_override_t *override =
                    (madt_lapic_override_t *)entry;

                madt_info.lapic_address = override->address;

                break;
            }

            default:
                break;
        }

        position += entry->length;
        remaining -= entry->length;
    }

    if (remaining != 0)
        return false;

    return madt_info.lapic_address != 0 &&
           madt_info.ioapic_count != 0;
}

/*
 * Limine base revision 4:
 * rsdp_address is already a virtual HHDM address.
 *
 * RSDT/XSDT and MADT addresses inside ACPI tables
 * remain physical and are translated using phys_to_virt().
 */
bool acpi_init(uint64_t rsdp_address, uint64_t hhdm)
{
    if (!rsdp_address || !hhdm)
        return false;

    hhdm_offset = hhdm;

    madt_info = (acpi_madt_info_t){0};

    /* Do NOT add HHDM to the RSDP address here. */
    rsdp_t *rsdp = (rsdp_t *)rsdp_address;

    if (*(const uint64_t *)rsdp != ACPI_RSDP_SIGNATURE)
        return false;

    if (checksum(rsdp, 20) != 0)
        return false;

    uint64_t root_address = 0;
    bool xsdt_mode = false;

    if (rsdp->revision >= 2)
    {
        if (rsdp->length < sizeof(rsdp_t))
            return false;

        if (rsdp->length > ACPI_MAX_TABLE_SIZE)
            return false;

        if (checksum(rsdp, rsdp->length) != 0)
            return false;

        if (rsdp->xsdt_address)
        {
            root_address = rsdp->xsdt_address;
            xsdt_mode = true;
        }
    }

    if (!root_address && rsdp->rsdt_address)
    {
        root_address = rsdp->rsdt_address;
        xsdt_mode = false;
    }

    if (!root_address)
        return false;

    madt_t *madt = find_madt(root_address, xsdt_mode);

    if (!madt)
        return false;

    return parse_madt(madt);
}

const acpi_madt_info_t *acpi_get_madt(void)
{
    return &madt_info;
}
