/*
 * APEI/GHES runtime path — ACPI 6.5 §18.3.2.7.
 *
 * Boot-time parsing (acpi_apei.c) discovers every HEST error source and
 * fingerprints each GHES entry's notification mechanism + Generic Error
 * Status Block address. This file owns the RUNTIME path that walks those
 * status blocks, decodes each Generic Error Data Entry, and routes
 *   Memory Error sections   → mce_migrate_request (poison + page swap)
 *   Processor Error sections → Touch publish + telemetry
 *   PCIe/Generic sections    → Touch publish
 * exactly the same way Phase 2F + 2F-migrate handle architecturally
 * delivered #MC events. The result: firmware-side-delivered errors on
 * server-class hardware (the dominant production delivery vector when
 * BIOS/UEFI pre-processes ECC events via SMI before handing them to the
 * OS) land in the same poison/migration pipeline as MSR-bank errors.
 *
 * Three delivery shapes are wired:
 *   Polled (notify type 0) — apei_ghes_poll_tick() runs from the PIT
 *                            IRQ handler at the scheduler tick rate;
 *                            per-source rate-limited via TSC compare so
 *                            the per-tick cost is one branch when no
 *                            source is due.
 *   NMI    (notify type 4) — apei_ghes_nmi_check() invoked from the NMI
 *                            entry in exception_handler. IRQ-safe;
 *                            defers the actual GESB walk to a K-Core
 *                            worker via irq_defer.
 *   SCI    (notify type 3) — apei_ghes_sci_check() invoked from
 *                            acpi_sci_handler alongside the GPE drain.
 *
 * GHESv2 sources are honoured: every read-ack flow rewrites the
 * Read-Ack Register per ACPI 6.5 §18.3.2.7.2 so the firmware sees the
 * event consumed and can deliver the next one.
 *
 * Boot-time call into apei_ghes_register_source happens from decode_ghes
 * in acpi_apei.c so we don't re-walk HEST.
 *
 * References:
 *   ACPI 6.5 §18 — APEI / HEST / GHES
 *   UEFI 2.10  §N.2.5 — Memory Error CPER section format
 *   Intel SDM Vol 3 §15 — MCE architecture (the destination pipeline)
 */

#ifndef APEI_GHES_RUNTIME_H
#define APEI_GHES_RUNTIME_H

#include "ktypes.h"
#include "error.h"

#define APEI_GHES_MAX_RUNTIME_SOURCES  16u

/* Initialise — registers Touch tag handles, zeroes counters. Safe to
 * call when no GHES source was seen. Must run AFTER:
 *   - touch_init  (TouchTagIntern available)
 *   - mce_migrate_init (we hand phys addresses to it)
 *   - acpi_parse_apei  (sources registered via decode_ghes hook) */
void apei_ghes_runtime_init(void);

bool apei_ghes_runtime_is_initialized(void);

/* Registered from acpi_apei.c decode_ghes(). Idempotent: re-registering
 * the same source_id refreshes the descriptor (firmware shouldn't but
 * may rev HEST across resume).
 *
 * notify_type values per ACPI 6.5 §18.3.2.7:
 *   0  Polled
 *   3  SCI
 *   4  NMI
 *   1  External IRQ (rare on x86 — treated as polled)
 *   2  Local IRQ (LVT) — TODO
 *
 * v2 reads from read_ack_* registers (zero if not v2).
 *
 * Returns OK or ERR_NO_MEMORY (table full). */
error_t apei_ghes_register_source(uint16_t source_id, bool v2,
                                   uint8_t  notify_type,
                                   uint32_t vector,
                                   uint32_t poll_interval_ms,
                                   uint64_t gesb_phys,
                                   uint32_t gesb_len,
                                   uint64_t read_ack_addr,
                                   uint64_t read_ack_preserve,
                                   uint64_t read_ack_write);

/* Periodic tick — called from PIT IRQ handler. Walks Polled sources
 * whose TSC deadline has elapsed and processes their GESB. Cheap when
 * nothing is due. */
void apei_ghes_poll_tick(void);

/* NMI-context entry. Walks NMI-notify sources, defers GESB processing
 * via irq_defer (NMI is even more restrictive than #MC IST — must not
 * take spinlocks held by interrupted code). Returns true if any source
 * advertised a pending error. */
bool apei_ghes_nmi_check(void);

/* SCI-context entry. Called from acpi_sci_handler alongside the GPE
 * drain. Returns the number of GHES sources whose GESB had a pending
 * error (0 = nothing — normal). */
uint32_t apei_ghes_sci_check(void);

/* Telemetry. */
typedef struct {
    uint32_t sources_registered;
    uint32_t sources_polled;
    uint32_t sources_nmi;
    uint32_t sources_sci;
    uint32_t sources_other;
    uint64_t events_processed;
    uint64_t memory_sections;
    uint64_t processor_sections;
    uint64_t pcie_sections;
    uint64_t generic_sections;
    uint64_t mce_migrate_requests;
    uint64_t poll_ticks;
    uint64_t nmi_invocations;
    uint64_t sci_invocations;
    uint64_t map_failures;
    uint64_t corrupt_records;
} apei_ghes_stats_t;

void apei_ghes_get_stats(apei_ghes_stats_t *out);
void apei_ghes_dump(void);

/* Test surface — synthesise a GESB at `gesb_phys` (caller-allocated +
 * pre-filled) and run the same processing path. Returns the number of
 * error sections decoded. Used by apei_ghes_test.c; not for production
 * callers. */
uint32_t apei_ghes_simulate(uintptr_t gesb_phys, uint32_t gesb_len);

#endif /* APEI_GHES_RUNTIME_H */
