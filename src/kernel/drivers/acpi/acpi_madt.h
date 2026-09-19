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

/* What the MADT said is NOT copied into arrays here, and that is deliberate.
 *
 * ACPI 6.5 §5.2.12.13: "Each NMI connection to a processor requires a
 * separate NMI structure", and on a machine that has any logical processor
 * with an x2APIC ID of 255 or greater, "the Local X2APIC NMI structure must
 * be used to specify global LINTx for ALL logical processors". So the number
 * of NMI rules is a number the FIRMWARE states, once, in the table it wrote.
 * A fixed array sized here has no honest value to take: the previous 16 NMI
 * rules, 8 NMI sources and 64 processors were three ceilings invented by this
 * kernel, and on a large machine each one dropped the tail of the list in
 * silence — the per-core NMI rule that was never programmed, the processor
 * whose ACPI UID could no longer be found.
 *
 * So madt_info_t carries WHERE the table is, and every later question is put
 * to the table itself: madt_for_each_nmi_rule, madt_for_each_nmi_source,
 * madt_uid_for_apic_id, madt_name_unreachable_processors. Each walk is
 * bounded by the length the table declares in its own header — firmware's
 * number, checked against firmware's own end pointer, not ours.
 *
 * NOTHING WAS GIVEN UP BY DROPPING THE COPY, including the question the copy
 * was kept for. "Which APIC IDs did the firmware reserve for sockets that
 * can be filled during OS runtime" is answered by walking Type 0 and Type 9
 * for entries whose flags are Online-Capable, and those entries are still
 * there, in the table, complete — where the 64-entry copy would have
 * answered it only for the first sixty-four. The two places that ask it
 * today ask it that way: acpi_parse_madt names every such seat as it goes
 * past, and madt_name_unreachable_processors says which of them no device
 * interrupt could ever reach. A hot-add path, when there is one, adds a walk
 * of its own beside those; it does not need a cache to walk, and a cache is
 * what would make its answer short. */

/* One Local APIC NMI rule, in whichever of the two forms the firmware used:
 * the 8-bit Local APIC NMI (ACPI 6.5 §5.2.12.7, Table 5.28) or the 32-bit
 * Local x2APIC NMI (§5.2.12.13, Table 5.35).
 *
 * `all_processors` is a fact the firmware stated, carried as a fact. Both
 * forms spell "everyone" with an all-ones UID — Table 5.28: "A value of 0xFF
 * signifies that this applies to all processors in the machine"; Table 5.35:
 * "A value of 0xFFFFFFFF signifies that this applies to all processors in
 * the machine" — but those are two different numbers, and 0xFF is also a UID
 * a 32-bit entry may legitimately carry for processor 255. Folding both into
 * one sentinel is how a rule meant for one processor becomes a rule for the
 * whole machine, or the other way round. */
typedef struct {
    uint32_t acpi_processor_uid;
    bool     all_processors;
    uint8_t  lint;                /* LINT0 or LINT1 */
    uint16_t mps_flags;           /* MPS INTI: polarity[1:0], trigger[3:2] */
} MadtNmiRule;

typedef void (*MadtNmiRuleFn)(const MadtNmiRule *rule, void *ctx);
typedef void (*MadtNmiSourceFn)(uint32_t gsi, uint16_t mps_flags, void *ctx);

typedef struct madt_info {
    uintptr_t lapic_address;
    uintptr_t ioapic_address;
    uint8_t   ioapic_id;
    uint32_t  ioapic_gsi_base;

    /* The parsed table itself, at the address acpi_find_table mapped it to.
     * Kept so the walkers above can re-read it without going back through
     * the ACPI table subsystem: acpi_find_table() fills a global map cache
     * and can call the mapper, neither of which an AP may do concurrently
     * during bring-up, while a read-only walk of memory that is already
     * mapped is safe from any core at any time. NULL when no MADT was
     * parsed, which every walker treats as "no entries". */
    const acpi_madt_t *table;

    /* The BSP's APIC ID at FULL width, as CPUID.0BH:EDX reports it (falling
     * back to the 8-bit CPUID.01H:EBX[31:24] initial ID only on parts with
     * no leaf 0BH) — see detect_bsp_apic_id in acpi_madt.c. It is stored
     * whole because that is what the MADT Type 9 (Processor Local x2APIC,
     * 32-bit X2APIC ID, ACPI 6.5 §5.2.12.12) entries must be matched
     * against. Consumers whose destination field is narrower — the IOAPIC
     * redirection entry carries eight bits — must REFUSE an ID that does
     * not fit rather than truncate it: the low eight bits name a different,
     * running processor. */
    uint32_t  bsp_lapic_id;

    /* The BSP's ACPI Processor UID, at the full 32 bits Table 5.34 gives it.
     * Truncating it to a byte, as this field used to, is the same defect as
     * truncating a destination: on a machine that lists its processors as
     * Type 9, UID 0x100 becomes UID 0 and the BSP is handed the NMI rule
     * written for somebody else. */
    uint32_t  bsp_acpi_processor_uid;
    bool      bsp_lapic_found;
    bool      bsp_acpi_uid_resolved;
    bool      has_pic;
    bool      valid;
} madt_info_t;

acpi_error_t acpi_parse_madt(madt_info_t* info);

/* Collect all enabled LAPIC IDs from MADT. Returns count written. */
uint8_t amp_collect_lapics(uint32_t* ids_out, uint8_t max_count);

/* Hand every Local APIC NMI rule that applies to one processor to `fn`.
 *
 * A rule applies when the firmware named that processor's ACPI UID, or when
 * it said the rule is for every processor. `uid_known` false means the
 * caller could not find its own UID in the table at all — then only the
 * every-processor rules apply, because a rule addressed to a UID cannot be
 * claimed by a processor that does not know it is the one addressed. */
void madt_for_each_nmi_rule(const madt_info_t *info,
                            uint32_t acpi_processor_uid, bool uid_known,
                            MadtNmiRuleFn fn, void *ctx);

/* Hand every MADT NMI Source (Type 3, ACPI 6.5 §5.2.12.6) to `fn`. These are
 * platform lines wired into an IOAPIC input as NMI — watchdog, chassis
 * intrusion, PSU fault — which OSPM must program with Delivery Mode 100. */
void madt_for_each_nmi_source(const madt_info_t *info,
                              MadtNmiSourceFn fn, void *ctx);

/* Which ACPI Processor UID the firmware gave the processor with this APIC
 * ID, from Type 0 (8-bit APIC ID / Processor ID) or Type 9 (32-bit X2APIC ID
 * / Processor UID). False when the table names no such processor — which is
 * an answer, not a failure: it means no UID-addressed rule is this core's. */
bool madt_uid_for_apic_id(const madt_info_t *info, uint32_t apic_id,
                          uint32_t *out_uid);

/* Name, once per boot, every logical processor — and every Online-Capable
 * seat — this machine cannot deliver a legacy IRQ or an MSI to as it is
 * configured now, with its APIC ID and its ACPI processor UID. Prints
 * nothing when every one of them can be named, which is every machine whose
 * APIC IDs stay below 255. */
void madt_name_unreachable_processors(const madt_info_t *info);

#endif // ACPI_MADT_H
