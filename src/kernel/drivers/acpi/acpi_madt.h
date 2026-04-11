#ifndef ACPI_MADT_H
#define ACPI_MADT_H

#include "acpi.h"

// MADT (Multiple APIC Description Table) - signature "APIC"
typedef struct {
    acpi_sdt_header_t header;
    uint32_t local_apic_address;    // Physical address of Local APIC
    uint32_t flags;                 // 1 = dual 8259 PIC installed
} __attribute__((packed)) acpi_madt_t;

#define MADT_FLAG_PCAT_COMPAT   (1 << 0)    // Dual 8259 PICs present

// MADT entry types
#define MADT_TYPE_LOCAL_APIC        0   // Processor Local APIC
#define MADT_TYPE_IO_APIC           1   // IO-APIC
#define MADT_TYPE_ISO               2   // Interrupt Source Override
#define MADT_TYPE_NMI_SOURCE        3   // NMI Source
#define MADT_TYPE_LOCAL_APIC_NMI    4   // Local APIC NMI
#define MADT_TYPE_LAPIC_OVERRIDE    5   // Local APIC Address Override

// Common entry header
typedef struct {
    uint8_t type;
    uint8_t length;
} __attribute__((packed)) madt_entry_header_t;

// Type 0: Processor Local APIC
typedef struct {
    madt_entry_header_t header;
    uint8_t acpi_processor_id;
    uint8_t apic_id;
    uint32_t flags;             // bit 0 = enabled, bit 1 = online capable
} __attribute__((packed)) madt_local_apic_t;

#define MADT_LAPIC_ENABLED      (1 << 0)
#define MADT_LAPIC_ONLINE_CAP   (1 << 1)

// Type 1: IO-APIC
typedef struct {
    madt_entry_header_t header;
    uint8_t io_apic_id;
    uint8_t reserved;
    uint32_t io_apic_address;   // Physical address
    uint32_t gsi_base;          // Global System Interrupt base
} __attribute__((packed)) madt_io_apic_t;

// Type 2: Interrupt Source Override
typedef struct {
    madt_entry_header_t header;
    uint8_t bus;                // 0 = ISA
    uint8_t source;             // ISA IRQ number
    uint32_t gsi;               // GSI this IRQ maps to
    uint16_t flags;             // MPS INTI flags (polarity, trigger)
} __attribute__((packed)) madt_iso_t;

// Type 4: Local APIC NMI
typedef struct {
    madt_entry_header_t header;
    uint8_t acpi_processor_id;  // 0xFF = all processors
    uint16_t flags;
    uint8_t lint;               // LINT# (0 or 1)
} __attribute__((packed)) madt_lapic_nmi_t;

// Type 5: Local APIC Address Override (64-bit)
typedef struct {
    madt_entry_header_t header;
    uint16_t reserved;
    uint64_t local_apic_address;
} __attribute__((packed)) madt_lapic_override_t;

// Parsed MADT info
typedef struct {
    uintptr_t lapic_address;        // Local APIC physical address
    uintptr_t ioapic_address;       // IO-APIC physical address
    uint8_t   ioapic_id;            // IO-APIC ID
    uint32_t  ioapic_gsi_base;      // IO-APIC GSI base
    uint8_t   bsp_lapic_id;         // BSP Local APIC ID
    bool      bsp_lapic_found;     // BSP LAPIC ID was set (0 is a valid ID)
    bool      has_pic;              // Dual 8259 PICs present
    bool      valid;                // MADT was found and parsed
} madt_info_t;

acpi_error_t acpi_parse_madt(madt_info_t* info);

// Collect all enabled LAPIC IDs from MADT into ids_out[].
// Returns count of IDs written (0 on failure).
uint8_t amp_collect_lapics(uint8_t* ids_out, uint8_t max_count);

#endif // ACPI_MADT_H
