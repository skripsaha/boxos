#ifndef ACPI_MADT_H
#define ACPI_MADT_H

#include "acpi.h"

/* MADT (Multiple APIC Description Table) — signature "APIC" (ACPI 6.5 §5.2.12) */
typedef struct {
    acpi_sdt_header_t header;
    uint32_t local_apic_address;    /* 32-bit fallback; 64-bit override may follow */
    uint32_t flags;                 /* bit 0 = PCAT-compatible dual 8259s present */
} __attribute__((packed)) acpi_madt_t;
_Static_assert(sizeof(acpi_madt_t) == 44, "acpi_madt_t must be 44 bytes");

#define MADT_FLAG_PCAT_COMPAT   (1 << 0)

/* MADT entry types (ACPI 6.5 §5.2.12). The list below covers everything we
 * currently consume; later types are skipped by length. */
#define MADT_TYPE_LOCAL_APIC        0
#define MADT_TYPE_IO_APIC           1
#define MADT_TYPE_ISO               2
#define MADT_TYPE_NMI_SOURCE        3
#define MADT_TYPE_LOCAL_APIC_NMI    4
#define MADT_TYPE_LAPIC_OVERRIDE    5
#define MADT_TYPE_LX2APIC           9
#define MADT_TYPE_LX2APIC_NMI       0xA

typedef struct {
    uint8_t type;
    uint8_t length;
} __attribute__((packed)) madt_entry_header_t;

/* Type 0 — Processor Local APIC. Spec length = 8 bytes. */
typedef struct {
    madt_entry_header_t header;
    uint8_t acpi_processor_id;
    uint8_t apic_id;
    uint32_t flags;
} __attribute__((packed)) madt_local_apic_t;
_Static_assert(sizeof(madt_local_apic_t) == 8, "MADT Type 0 = 8 bytes");

#define MADT_LAPIC_ENABLED      (1 << 0)
#define MADT_LAPIC_ONLINE_CAP   (1 << 1)

/* Type 1 — IO APIC. Spec length = 12 bytes. */
typedef struct {
    madt_entry_header_t header;
    uint8_t io_apic_id;
    uint8_t reserved;
    uint32_t io_apic_address;
    uint32_t gsi_base;
} __attribute__((packed)) madt_io_apic_t;
_Static_assert(sizeof(madt_io_apic_t) == 12, "MADT Type 1 = 12 bytes");

/* Type 2 — Interrupt Source Override. Spec length = 10 bytes. */
typedef struct {
    madt_entry_header_t header;
    uint8_t bus;
    uint8_t source;
    uint32_t gsi;
    uint16_t flags;
} __attribute__((packed)) madt_iso_t;
_Static_assert(sizeof(madt_iso_t) == 10, "MADT Type 2 = 10 bytes");

/* Type 3 — NMI Source. Spec length = 8 bytes. */
typedef struct {
    madt_entry_header_t header;
    uint16_t flags;                /* MPS INTI flags */
    uint32_t gsi;                  /* Global System Interrupt being asserted */
} __attribute__((packed)) madt_nmi_source_t;
_Static_assert(sizeof(madt_nmi_source_t) == 8, "MADT Type 3 = 8 bytes");

/* Type 4 — Local APIC NMI. Spec length = 6 bytes. */
typedef struct {
    madt_entry_header_t header;
    uint8_t acpi_processor_id;     /* 0xFF = all processors */
    uint16_t flags;                /* MPS INTI: polarity[1:0], trigger[3:2] */
    uint8_t lint;                  /* LINT# pin (0 or 1) */
} __attribute__((packed)) madt_lapic_nmi_t;
_Static_assert(sizeof(madt_lapic_nmi_t) == 6, "MADT Type 4 = 6 bytes");

/* Type 5 — Local APIC Address Override. Spec length = 12 bytes. */
typedef struct {
    madt_entry_header_t header;
    uint16_t reserved;
    uint64_t local_apic_address;
} __attribute__((packed)) madt_lapic_override_t;
_Static_assert(sizeof(madt_lapic_override_t) == 12, "MADT Type 5 = 12 bytes");

/* Type 9 — Processor Local x2APIC. Spec length = 16 bytes.
 * Required for ACPI processor enumeration of CPUs whose APIC ID > 255. */
typedef struct {
    madt_entry_header_t header;
    uint16_t reserved;
    uint32_t x2apic_id;
    uint32_t flags;                /* same enabled / online-capable bits */
    uint32_t acpi_processor_uid;
} __attribute__((packed)) madt_lx2apic_t;
_Static_assert(sizeof(madt_lx2apic_t) == 16, "MADT Type 9 = 16 bytes");

/* Type 10 — Local x2APIC NMI. Spec length = 12 bytes. */
typedef struct {
    madt_entry_header_t header;
    uint16_t flags;                /* MPS INTI */
    uint32_t acpi_processor_uid;   /* 0xFFFFFFFF = all */
    uint8_t  lint;
    uint8_t  reserved[3];
} __attribute__((packed)) madt_lx2apic_nmi_t;
_Static_assert(sizeof(madt_lx2apic_nmi_t) == 12, "MADT Type 10 = 12 bytes");

/* Per-CPU LAPIC NMI assignment captured during MADT parse, applied to LVT
 * during LAPIC bring-up. ACPI 6.5 §5.2.12.7: OSPM uses this to wire the
 * Local APIC NMI source. Without it, NMI watchdogs cannot fire. */
#define MADT_NMI_PROCESSOR_ALL  0xFFu
#define MADT_MAX_NMI_ENTRIES    16u
#define MADT_MAX_NMI_SOURCES     8u

typedef struct {
    uint8_t  acpi_processor_id;   /* 0xFF = broadcast to all */
    uint8_t  lint;                /* LINT0 or LINT1 */
    uint16_t mps_flags;           /* polarity / trigger bits */
    bool     valid;
} madt_nmi_entry_t;

typedef struct {
    uint32_t gsi;                 /* Global System Interrupt */
    uint16_t mps_flags;           /* polarity / trigger bits */
    bool     valid;
} madt_nmi_source_info_t;

/* Bounded so madt_info_t (which lives on the stack in some callers) stays
 * under ~1 KB. BoxOS does not currently scale past this CPU count; the
 * limit can be raised when the scheduler / per-core data path supports
 * larger fleets. */
#define MADT_MAX_CPU_MAP    64

typedef struct {
    uint32_t apic_id;
    uint32_t acpi_processor_id;
    bool     enabled;
} madt_cpu_map_t;

typedef struct madt_info {
    uintptr_t lapic_address;
    uintptr_t ioapic_address;
    uint8_t   ioapic_id;
    uint32_t  ioapic_gsi_base;
    uint8_t   bsp_lapic_id;             /* from CPUID(1).EBX[31:24] */
    uint8_t   bsp_acpi_processor_id;    /* matching MADT entry, used for NMI lookup */
    bool      bsp_lapic_found;
    bool      bsp_acpi_id_resolved;
    bool      has_pic;
    bool      valid;

    madt_nmi_entry_t nmi[MADT_MAX_NMI_ENTRIES];
    uint8_t          nmi_count;

    madt_nmi_source_info_t nmi_sources[MADT_MAX_NMI_SOURCES];
    uint8_t                 nmi_source_count;

    /* APIC ID ⇄ ACPI processor ID mapping captured from Type 0 (LAPIC)
     * and Type 9 (x2APIC) MADT entries. Used to apply per-CPU NMI rules
     * on APs without re-walking the table. */
    madt_cpu_map_t cpu_map[MADT_MAX_CPU_MAP];
    uint16_t       cpu_map_count;
} madt_info_t;

acpi_error_t acpi_parse_madt(madt_info_t* info);

/* Collect all enabled LAPIC IDs from MADT. Returns count written. */
uint8_t amp_collect_lapics(uint8_t* ids_out, uint8_t max_count);

#endif // ACPI_MADT_H
