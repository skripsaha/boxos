
#ifndef APEI_GHES_RUNTIME_H
#define APEI_GHES_RUNTIME_H

#include "ktypes.h"
#include "error.h"

#define APEI_GHES_MAX_RUNTIME_SOURCES  16u

void apei_ghes_runtime_init(void);

bool apei_ghes_runtime_is_initialized(void);

error_t apei_ghes_register_source(uint16_t source_id, bool v2,
                                   uint8_t  notify_type,
                                   uint32_t vector,
                                   uint32_t poll_interval_ms,
                                   uint64_t gesb_phys,
                                   uint32_t gesb_len,
                                   uint64_t read_ack_addr,
                                   uint64_t read_ack_preserve,
                                   uint64_t read_ack_write);

void apei_ghes_poll_tick(void);

bool apei_ghes_nmi_check(void);

uint32_t apei_ghes_sci_check(void);

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

uint32_t apei_ghes_simulate(uintptr_t gesb_phys, uint32_t gesb_len);

#endif