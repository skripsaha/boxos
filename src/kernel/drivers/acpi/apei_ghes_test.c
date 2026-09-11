
#include "apei_ghes_runtime.h"
#include "memtag.h"
#include "mce_migrate.h"
#include "vmm.h"
#include "pmm.h"
#include "klib.h"

#define AGT_CHECK(cond, label) \
    do { if (cond) { pass++; } \
         else { fail++; \
                kprintf("[APEI TEST]   %[R]FAIL%[D]: " label "\n"); } \
    } while (0)

typedef struct {
    uint32_t block_status;
    uint32_t raw_data_offset;
    uint32_t raw_data_length;
    uint32_t data_length;
    uint32_t error_severity;
} __attribute__((packed)) gesb_header_t;

typedef struct {
    uint8_t  section_type[16];
    uint32_t error_severity;
    uint16_t revision;
    uint8_t  validation_bits;
    uint8_t  flags;
    uint32_t error_data_length;
    uint8_t  fru_id[16];
    char     fru_text[20];
    uint8_t  timestamp[8];
} __attribute__((packed)) gedata_entry_t;

typedef struct {
    uint64_t validation_bits;
    uint64_t error_status;
    uint64_t physical_address;
    uint64_t physical_address_mask;
    uint16_t node;
    uint16_t card;
    uint16_t module;
    uint16_t bank;
    uint16_t device;
    uint16_t row;
    uint16_t column;
    uint16_t bit_position;
    uint64_t requestor_id;
    uint64_t responder_id;
    uint64_t target_id;
    uint8_t  error_type;
    uint8_t  extended[3];
} __attribute__((packed)) cper_mem_t;

static const uint8_t GUID_MEMORY[16] = {
    0x14,0x11,0xBC,0xA5, 0x64,0x6F, 0xDE,0x4E,
    0xB8,0x63, 0x3E,0x83,0xED,0x7C,0x83,0xB1 };
static const uint8_t GUID_PROC[16] = {
    0xB0,0xA0,0x3E,0xDC, 0x44,0xA1, 0x97,0x47,
    0xB9,0x5B, 0x53,0xFA,0x24,0x2B,0x6E,0x1D };

static void build_header(void *buf, uint32_t status, uint32_t data_len, uint32_t sev) {
    gesb_header_t *h = (gesb_header_t *)buf;
    h->block_status     = status;
    h->raw_data_offset  = 0;
    h->raw_data_length  = 0;
    h->data_length      = data_len;
    h->error_severity   = sev;
}

static void build_mem_entry(void *buf, uint64_t phys, uint8_t sev, uint16_t bank) {
    gedata_entry_t *ge = (gedata_entry_t *)buf;
    for (int i = 0; i < 16; i++) ge->section_type[i] = GUID_MEMORY[i];
    ge->error_severity     = sev;
    ge->revision           = 0x0202;
    ge->validation_bits    = 0;
    ge->flags              = 0;
    ge->error_data_length  = (uint32_t)sizeof(cper_mem_t);
    cper_mem_t *m = (cper_mem_t *)(ge + 1);
    memset(m, 0, sizeof(*m));
    m->validation_bits   = (1u << 1);
    m->physical_address  = phys;
    m->bank              = bank;
    m->error_type        = 3;
}

static void build_proc_entry(void *buf, uint8_t sev) {
    gedata_entry_t *ge = (gedata_entry_t *)buf;
    for (int i = 0; i < 16; i++) ge->section_type[i] = GUID_PROC[i];
    ge->error_severity     = sev;
    ge->revision           = 0x0202;
    ge->validation_bits    = 0;
    ge->flags              = 0;
    ge->error_data_length  = 32;
    memset(ge + 1, 0xA5, 32);
}

void ApeiGhesTest(void) {
    kprintf("[APEI TEST] Starting APEI/GHES runtime test...\n");
    size_t pass = 0, fail = 0;

    if (!apei_ghes_runtime_is_initialized()) {
        kprintf("[APEI TEST] %[Y]SKIPPED%[D]: not initialized\n");
        return;
    }

    void *buf = pmm_alloc_zero(1, PHYS_TAG_USER);
    if (!buf) buf = pmm_alloc_zero(1);
    AGT_CHECK(buf != NULL, "scratch GESB alloc");
    if (!buf) goto done;

    uintptr_t buf_va = (uintptr_t)vmm_phys_to_virt((uintptr_t)buf);
    uint32_t  buf_len = PMM_PAGE_SIZE;

    {
        memset((void *)buf_va, 0, buf_len);
        apei_ghes_stats_t s0; apei_ghes_get_stats(&s0);
        uint32_t r = apei_ghes_simulate(buf_va, buf_len);
        apei_ghes_stats_t s1; apei_ghes_get_stats(&s1);
        AGT_CHECK(r == 0, "T1: empty GESB → no event");
        AGT_CHECK(s1.events_processed == s0.events_processed,
                  "T1: events counter unchanged");
        AGT_CHECK(s1.memory_sections == s0.memory_sections,
                  "T1: memory sections counter unchanged");
    }

    {
        memset((void *)buf_va, 0, buf_len);
        void *bad_phys = pmm_alloc_zero(1, PHYS_TAG_USER);
        if (!bad_phys) bad_phys = pmm_alloc_zero(1);
        AGT_CHECK(bad_phys != NULL, "T2: bad_phys alloc");
        if (!bad_phys) goto t3;

        uint32_t mem_entry_len = (uint32_t)sizeof(gedata_entry_t) + (uint32_t)sizeof(cper_mem_t);
        build_header((void *)buf_va, 0x1, mem_entry_len, 0);
        build_mem_entry((void *)(buf_va + sizeof(gesb_header_t)),
                        (uint64_t)(uintptr_t)bad_phys, 0, 12);

        apei_ghes_stats_t b; apei_ghes_get_stats(&b);
        uint32_t r = apei_ghes_simulate(buf_va, buf_len);
        apei_ghes_stats_t a; apei_ghes_get_stats(&a);
        AGT_CHECK(r == 1, "T2: simulate returns 1 (event acked)");
        AGT_CHECK(a.memory_sections == b.memory_sections + 1,
                  "T2: memory_sections incremented");
        AGT_CHECK(a.mce_migrate_requests == b.mce_migrate_requests + 1,
                  "T2: mce_migrate_requests incremented (bridge active)");
        AGT_CHECK(a.events_processed == b.events_processed + 1,
                  "T2: events_processed incremented");
        gesb_header_t *h = (gesb_header_t *)buf_va;
        AGT_CHECK(h->block_status == 0,
                  "T2: block_status cleared (W1C ack)");
        pmm_free(bad_phys, 1);
    }

t3:
    {
        memset((void *)buf_va, 0, buf_len);
        void *bad3 = pmm_alloc_zero(1, PHYS_TAG_USER);
        if (!bad3) bad3 = pmm_alloc_zero(1);
        AGT_CHECK(bad3 != NULL, "T3: bad3 phys alloc");
        if (!bad3) goto t4;

        uint32_t mem_entry_len  = (uint32_t)sizeof(gedata_entry_t) + (uint32_t)sizeof(cper_mem_t);
        uint32_t proc_entry_len = (uint32_t)sizeof(gedata_entry_t) + 32u;

        build_header((void *)buf_va, 0x1, mem_entry_len + proc_entry_len, 0);
        build_mem_entry((void *)(buf_va + sizeof(gesb_header_t)),
                        (uint64_t)(uintptr_t)bad3, 0, 7);
        build_proc_entry((void *)(buf_va + sizeof(gesb_header_t) + mem_entry_len), 2);

        apei_ghes_stats_t b; apei_ghes_get_stats(&b);
        (void)apei_ghes_simulate(buf_va, buf_len);
        apei_ghes_stats_t a; apei_ghes_get_stats(&a);
        AGT_CHECK(a.memory_sections == b.memory_sections + 1,
                  "T3: memory_sections +1");
        AGT_CHECK(a.processor_sections == b.processor_sections + 1,
                  "T3: processor_sections +1");
        pmm_free(bad3, 1);
    }

t4:
    {
        memset((void *)buf_va, 0, buf_len);
        build_header((void *)buf_va, 0x1, buf_len * 4, 0);
        apei_ghes_stats_t b; apei_ghes_get_stats(&b);
        uint32_t r = apei_ghes_simulate(buf_va, buf_len);
        apei_ghes_stats_t a; apei_ghes_get_stats(&a);
        AGT_CHECK(r == 0, "T4: corrupt returns 0");
        AGT_CHECK(a.corrupt_records == b.corrupt_records + 1,
                  "T4: corrupt_records +1");
    }

    {
        memset((void *)buf_va, 0, buf_len);
        void *bad5 = pmm_alloc_zero(1, PHYS_TAG_USER);
        if (!bad5) bad5 = pmm_alloc_zero(1);
        if (bad5) {
            uint32_t mem_entry_len = (uint32_t)sizeof(gedata_entry_t) + (uint32_t)sizeof(cper_mem_t);
            build_header((void *)buf_va, 0x2, mem_entry_len, 2);
            build_mem_entry((void *)(buf_va + sizeof(gesb_header_t)),
                            (uint64_t)(uintptr_t)bad5, 2, 3);

            apei_ghes_stats_t b; apei_ghes_get_stats(&b);
            (void)apei_ghes_simulate(buf_va, buf_len);
            apei_ghes_stats_t a; apei_ghes_get_stats(&a);
            AGT_CHECK(a.memory_sections == b.memory_sections + 1,
                      "T5: memory_sections still +1 (event published)");
            AGT_CHECK(a.mce_migrate_requests == b.mce_migrate_requests,
                      "T5: NO migrate request for corrected severity");
            pmm_free(bad5, 1);
        }
    }

    pmm_free(buf, 1);

done:
    if (fail == 0)
        kprintf("[APEI TEST] %[S]PASSED%[D]: all %zu checks OK\n", pass);
    else
        kprintf("[APEI TEST] %[R]FAILED%[D]: %zu pass, %zu fail\n", pass, fail);

    apei_ghes_dump();
}