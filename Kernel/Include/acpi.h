#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>
#include <stdbool.h>

#define ACPI_MAX_IOAPICS 8
#define ACPI_MAX_ISO     16

typedef struct
{
    uint8_t id;
    uint32_t address;
    uint32_t gsi_base;
} acpi_ioapic_t;

typedef struct
{
    uint8_t source;
    uint32_t gsi;
    uint16_t flags;
} acpi_iso_t;

typedef struct
{
    uint64_t lapic_address;

    uint32_t ioapic_count;
    acpi_ioapic_t ioapics[ACPI_MAX_IOAPICS];

    uint32_t iso_count;
    acpi_iso_t isos[ACPI_MAX_ISO];
} acpi_madt_info_t;

bool acpi_init(uint64_t rsdp_physical, uint64_t hhdm_offset);
const acpi_madt_info_t *acpi_get_madt(void);

#endif