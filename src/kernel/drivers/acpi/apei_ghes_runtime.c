/*
 * APEI/GHES runtime path — implementation.
 *
 * See apei_ghes_runtime.h for design + spec citations. This file owns:
 *   1. Per-source runtime descriptor table (16 max, bounded).
 *   2. apei_ghes_register_source() — invoked from acpi_apei.c decode_ghes.
 *   3. Generic Error Status Block walker — parses header + Generic Error
 *      Data Entries, routes each section through the proper destination.
 *   4. Memory Error CPER section parser (UEFI 2.10 §N.2.5) → bridges to
 *      mce_migrate_request so firmware-delivered ECC reaches the same
 *      poison + page-migration pipeline as architectural #MC events.
 *   5. Polled / NMI / SCI entry points.
 *   6. v2 read-ack flow per ACPI 6.5 §18.3.2.7.2.
 *   7. Telemetry + dump helpers + test-only simulate path.
 */

#include "apei_ghes_runtime.h"
#include "klib.h"
#include "vmm.h"
#include "pmm.h"
#include "memtag.h"
#include "touch.h"
#include "mce_migrate.h"
#include "mce.h"
#include "irq_defer.h"
#include "cpuid.h"

/* TSC-frequency getter — published by cpu calibration into the cpu_caps
 * shared page; declared here to avoid a per-call lookup. */
extern uint64_t cpu_get_tsc_freq_khz(void);

/* ─── On-wire layouts (ACPI 6.5 §18.3.2.7.1 / UEFI 2.10 §N) ─────── */

typedef struct {
    uint32_t block_status;        /* bit 0 uncorr, 1 corr, 2 multi-uc, 3 multi-c
                                   * bits 13:4 entry count */
    uint32_t raw_data_offset;
    uint32_t raw_data_length;
    uint32_t data_length;         /* total bytes of Generic Error Data Entries */
    uint32_t error_severity;      /* 0=recoverable 1=fatal 2=corrected 3=info */
} __attribute__((packed)) gesb_header_t;
_Static_assert(sizeof(gesb_header_t) == 20, "GESB header = 20 B");

typedef struct {
    uint8_t  section_type[16];    /* GUID */
    uint32_t error_severity;
    uint16_t revision;
    uint8_t  validation_bits;
    uint8_t  flags;
    uint32_t error_data_length;
    uint8_t  fru_id[16];
    char     fru_text[20];
    uint8_t  timestamp[8];
} __attribute__((packed)) gedata_entry_t;
_Static_assert(sizeof(gedata_entry_t) == 72, "Generic Error Data Entry = 72 B");

/* Memory Error CPER section — UEFI 2.10 §N.2.5 r2 layout (80 bytes). */
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
    uint8_t  error_type;          /* 0=unknown 1=no-error 2=single-bit-ECC 3=multi-bit-ECC
                                   * 4=single-symbol-ChipKill 5=multi-symbol-ChipKill ... */
    uint8_t  extended[3];
} __attribute__((packed)) cper_memory_error_t;

#define MEM_VBIT_PHYSADDR        (1u << 1)
#define MEM_VBIT_PHYSADDR_MASK   (1u << 2)
#define MEM_VBIT_ERROR_TYPE      (1u << 14)

/* CPER section type GUIDs — wire byte order (little-endian data1..data4
 * then byte tail). Same constants as acpi_apei.c. */
static const uint8_t SECT_PROCESSOR[16] = {
    0xB0,0xA0,0x3E,0xDC, 0x44,0xA1, 0x97,0x47,
    0xB9,0x5B, 0x53,0xFA,0x24,0x2B,0x6E,0x1D };
static const uint8_t SECT_MEMORY[16] = {
    0x14,0x11,0xBC,0xA5, 0x64,0x6F, 0xDE,0x4E,
    0xB8,0x63, 0x3E,0x83,0xED,0x7C,0x83,0xB1 };
static const uint8_t SECT_PCIE[16] = {
    0x54,0xE9,0x95,0xD9, 0xC1,0xBB, 0x0F,0x43,
    0xAD,0x91, 0xB4,0x4D,0xCB,0x3C,0x6F,0x35 };

static inline bool guid_eq(const uint8_t *a, const uint8_t *b) {
    for (int i = 0; i < 16; i++) if (a[i] != b[i]) return false;
    return true;
}

/* ─── Per-source runtime descriptor ─────────────────────────────── */

typedef struct {
    uint16_t  source_id;
    uint8_t   notify_type;
    uint8_t   flags;              /* bit 0: in_use, bit 1: v2 */
    uint32_t  gsiv;
    uint32_t  poll_interval_ms;
    uint32_t  gesb_len;
    uint64_t  gesb_phys;
    void     *gesb_va;            /* mapped at register-time */
    uint64_t  next_poll_tsc;
    /* GHESv2 read-ack registers (zero for v1). */
    uint64_t  read_ack_addr;
    void     *read_ack_va;        /* mapped at register-time, like gesb_va */
    uint64_t  read_ack_preserve;
    uint64_t  read_ack_write;
    /* Per-source counters. */
    volatile uint64_t events;
    volatile uint64_t errors;
} apei_ghes_source_t;

#define SRC_IN_USE   (1u << 0)
#define SRC_V2       (1u << 1)

static apei_ghes_source_t g_sources[APEI_GHES_MAX_RUNTIME_SOURCES];
static volatile uint32_t  g_source_count = 0;

/* ─── Stats (RELAXED — never load-bearing) ──────────────────────── */

static volatile uint64_t g_stat_events_processed     = 0;
static volatile uint64_t g_stat_memory_sections      = 0;
static volatile uint64_t g_stat_processor_sections   = 0;
static volatile uint64_t g_stat_pcie_sections        = 0;
static volatile uint64_t g_stat_generic_sections     = 0;
static volatile uint64_t g_stat_mce_migrate_requests = 0;
static volatile uint64_t g_stat_poll_ticks           = 0;
static volatile uint64_t g_stat_nmi_invocations      = 0;
static volatile uint64_t g_stat_sci_invocations      = 0;
static volatile uint64_t g_stat_map_failures         = 0;
static volatile uint64_t g_stat_corrupt_records      = 0;

/* ─── Touch tag cache ───────────────────────────────────────────── */

static TouchTag g_tag_mem_error    = TOUCH_TAG_INVALID;
static TouchTag g_tag_proc_error   = TOUCH_TAG_INVALID;
static TouchTag g_tag_pcie_error   = TOUCH_TAG_INVALID;
static TouchTag g_tag_generic_err  = TOUCH_TAG_INVALID;
static TouchTag g_tag_ready        = TOUCH_TAG_INVALID;

static volatile bool g_initialized = false;

/* ─── Touch payload (64 B fits Pocket envelope) ─────────────────── */

typedef struct {
    uint64_t physical_address;   /* 8 */
    uint64_t status;             /* 8 */
    uint32_t source_id;          /* 4 */
    uint32_t section_severity;   /* 4 */
    uint16_t node;               /* 2 */
    uint16_t card;               /* 2 */
    uint16_t module;             /* 2 */
    uint16_t bank;               /* 2 */
    uint8_t  notify_type;        /* 1 */
    uint8_t  error_type;         /* 1 */
    uint8_t  v2;                 /* 1 */
    uint8_t  pad8;               /* 1 */
    uint32_t pad32;              /* 4 */
    uint8_t  pad[24];            /* 24 → total 64 B */
} apei_event_payload_t;
_Static_assert(sizeof(apei_event_payload_t) == 64,
               "APEI event payload must fit 64 B");

/* ─── TSC helper (uses g_cpu_caps.tsc_freq_khz when available) ──── */

static uint64_t apei_ghes_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t apei_ghes_tsc_per_ms(void) {
    uint64_t khz = cpu_get_tsc_freq_khz();
    if (khz == 0) khz = 1000000;  /* 1 GHz floor */
    return khz;                   /* khz = ticks per ms by definition */
}

/* ─── Init ──────────────────────────────────────────────────────── */

bool apei_ghes_runtime_is_initialized(void) { return g_initialized; }

void apei_ghes_runtime_init(void) {
    if (g_initialized) return;
    g_tag_mem_error   = TouchTagIntern("apei:memory:error");
    g_tag_proc_error  = TouchTagIntern("apei:processor:error");
    g_tag_pcie_error  = TouchTagIntern("apei:pcie:error");
    g_tag_generic_err = TouchTagIntern("apei:generic:error");
    g_tag_ready       = TouchTagIntern("apei:ghes:ready");
    g_initialized = true;
    debug_printf("[APEI] runtime init: %u source(s) tracked, tags resolved\n",
                 (unsigned)g_source_count);
    if (g_source_count == 0) {
        debug_printf("[APEI] no GHES sources discovered — runtime dormant\n");
    }
    apei_event_payload_t ev = {0};
    ev.source_id = g_source_count;
    if (g_tag_ready != TOUCH_TAG_INVALID) {
        TouchPublishId(g_tag_ready, &ev, sizeof(ev), 0u, 0u);
    }
}

error_t apei_ghes_register_source(uint16_t source_id, bool v2,
                                   uint8_t  notify_type,
                                   uint32_t vector,
                                   uint32_t poll_interval_ms,
                                   uint64_t gesb_phys,
                                   uint32_t gesb_len,
                                   uint64_t read_ack_addr,
                                   uint64_t read_ack_preserve,
                                   uint64_t read_ack_write) {
    if (gesb_phys == 0 || gesb_len < sizeof(gesb_header_t))
        return ERR_INVALID_ARGUMENT;

    /* Idempotent refresh — find existing slot for this source_id first. */
    apei_ghes_source_t *slot = NULL;
    for (uint32_t i = 0; i < g_source_count; i++) {
        if ((g_sources[i].flags & SRC_IN_USE) &&
            g_sources[i].source_id == source_id) {
            slot = &g_sources[i];
            break;
        }
    }
    if (!slot) {
        if (g_source_count >= APEI_GHES_MAX_RUNTIME_SOURCES) {
            debug_printf("[APEI] runtime source table full (%u), dropping src=%u\n",
                         APEI_GHES_MAX_RUNTIME_SOURCES, source_id);
            return ERR_NO_MEMORY;
        }
        slot = &g_sources[g_source_count];
        g_source_count++;
    }

    /* Map the GESB once. Read/write so we can W1C block_status to ack.
     * vmm_map_mmio returns volatile void* (MMIO accesses must not be
     * cached or reordered by the compiler); we store the address as
     * plain void* in the slot and re-apply the volatile qualifier at
     * every dereference site (see process_source_gesb / ghes_read_ack
     * which cast to `volatile <type>*`). The explicit cast here makes
     * the qualifier drop intentional rather than implicit. */
    void *gesb_va = (void *)(uintptr_t)vmm_map_mmio(gesb_phys, gesb_len, VMM_FLAGS_KERNEL_RW);
    if (!gesb_va) {
        debug_printf("[APEI] GESB map failed for src=%u (phys=0x%lx len=%u)\n",
                     source_id, (unsigned long)gesb_phys, gesb_len);
        __atomic_fetch_add(&g_stat_map_failures, 1, __ATOMIC_RELAXED);
        slot->flags = 0;
        return ERR_NO_MEMORY;
    }

    slot->source_id          = source_id;
    slot->notify_type        = notify_type;
    slot->flags              = SRC_IN_USE | (v2 ? SRC_V2 : 0);
    slot->gsiv               = vector;
    slot->poll_interval_ms   = poll_interval_ms ? poll_interval_ms : 1000u;
    slot->gesb_phys          = gesb_phys;
    slot->gesb_va            = gesb_va;
    slot->gesb_len           = gesb_len;
    slot->next_poll_tsc      = apei_ghes_rdtsc();   /* poll on first tick */
    slot->read_ack_addr      = read_ack_addr;
    /* Mapped here, once, for the same reason the GESB above is: ghes_read_ack
     * runs on every consumed error record, and mapping eight bytes per
     * acknowledgement leaks one kernel MMIO mapping — and the page-table pages
     * beneath it — per hardware error, forever. Firmware that reports
     * corrected ECC errors regularly is not exotic; it is what a machine with
     * one tired DIMM does all day. A leak that only appears on real hardware,
     * hours in, is the hardest kind to find and the easiest kind to prevent. */
    slot->read_ack_va        = (read_ack_addr != 0)
        ? (void *)(uintptr_t)vmm_map_mmio(read_ack_addr, 8, VMM_FLAGS_KERNEL_RW)
        : NULL;
    slot->read_ack_preserve  = read_ack_preserve;
    slot->read_ack_write     = read_ack_write;
    slot->events             = 0;
    slot->errors             = 0;

    debug_printf("[APEI] runtime source registered: id=%u %s notify=%u "
                 "poll=%u ms gesb=0x%lx (%u B) v2=%d\n",
                 source_id,
                 (notify_type == 0) ? "polled" :
                 (notify_type == 3) ? "sci"    :
                 (notify_type == 4) ? "nmi"    : "other",
                 notify_type, slot->poll_interval_ms,
                 (unsigned long)gesb_phys, gesb_len, (int)v2);
    return OK;
}

/* ─── CPER memory-error decoder + MCE bridge ────────────────────── */

static mce_severity_t cper_to_mce_sev(uint32_t cper_sev) {
    switch (cper_sev) {
        case 0: return MCE_SEV_UCR;       /* recoverable */
        case 1: return MCE_SEV_UC;        /* fatal */
        case 2: return MCE_SEV_CORRECTED; /* corrected */
        case 3:                           /* informational */
        default:return MCE_SEV_NONE;
    }
}

/* Process one Memory Error section. Returns true if a poison + migrate
 * request was queued (false on no phys / not poisonable severity). */
static bool process_memory_section(const apei_ghes_source_t *src,
                                    uint32_t section_severity,
                                    const cper_memory_error_t *m,
                                    uint32_t data_len) {
    if (data_len < sizeof(cper_memory_error_t)) return false;
    if (!(m->validation_bits & MEM_VBIT_PHYSADDR)) return false;

    uintptr_t phys = (uintptr_t)m->physical_address;
    if (phys == 0) return false;

    mce_severity_t sev = cper_to_mce_sev(section_severity);

    apei_event_payload_t ev = {0};
    ev.physical_address = m->physical_address;
    ev.status           = m->error_status;
    ev.source_id        = src->source_id;
    ev.section_severity = section_severity;
    ev.node             = m->node;
    ev.card             = m->card;
    ev.module           = m->module;
    ev.bank             = m->bank;
    ev.notify_type      = src->notify_type;
    ev.error_type       = m->error_type;
    ev.v2               = (src->flags & SRC_V2) ? 1 : 0;
    if (g_tag_mem_error != TOUCH_TAG_INVALID) {
        TouchPublishIrqPair(g_tag_mem_error, TOUCH_TAG_INVALID,
                             &ev, (uint16_t)sizeof(ev), 0u, 0u);
    }
    __atomic_fetch_add(&g_stat_memory_sections, 1, __ATOMIC_RELAXED);

    /* Only UCR + UC sections poison. Corrected/informational stay
     * advisory — they reflect HW that already recovered. Mirrors the
     * Phase 2F policy: mce.c only calls pmm_set_poisoned on UC|UCR. */
    if (sev != MCE_SEV_UCR && sev != MCE_SEV_UC) return false;

    uintptr_t page_phys = phys & ~(uintptr_t)0xFFF;
    pmm_set_poisoned(page_phys);
    (void)MemTagApplyByPhys(page_phys, 1, "mce:poisoned");
    (void)mce_migrate_request(page_phys, sev, m->error_status);
    __atomic_fetch_add(&g_stat_mce_migrate_requests, 1, __ATOMIC_RELAXED);
    return true;
}

/* Walk one Generic Error Data Entry, dispatch by section_type GUID.
 * Returns total bytes consumed (entry header + data) or 0 on corrupt. */
static uint32_t process_entry(const apei_ghes_source_t *src,
                               const uint8_t *entry, uint32_t avail) {
    if (avail < sizeof(gedata_entry_t)) return 0;
    const gedata_entry_t *ge = (const gedata_entry_t *)entry;
    uint32_t data_len = ge->error_data_length;
    if ((uint64_t)data_len + sizeof(gedata_entry_t) > avail) return 0;

    const uint8_t *body = entry + sizeof(gedata_entry_t);

    apei_event_payload_t generic = {0};
    generic.source_id        = src->source_id;
    generic.section_severity = ge->error_severity;
    generic.notify_type      = src->notify_type;
    generic.v2               = (src->flags & SRC_V2) ? 1 : 0;

    if (guid_eq(ge->section_type, SECT_MEMORY)) {
        (void)process_memory_section(src, ge->error_severity,
                                      (const cper_memory_error_t *)body, data_len);
    } else if (guid_eq(ge->section_type, SECT_PROCESSOR)) {
        if (g_tag_proc_error != TOUCH_TAG_INVALID) {
            TouchPublishIrqPair(g_tag_proc_error, TOUCH_TAG_INVALID,
                                 &generic, sizeof(generic), 0u, 0u);
        }
        __atomic_fetch_add(&g_stat_processor_sections, 1, __ATOMIC_RELAXED);
    } else if (guid_eq(ge->section_type, SECT_PCIE)) {
        if (g_tag_pcie_error != TOUCH_TAG_INVALID) {
            TouchPublishIrqPair(g_tag_pcie_error, TOUCH_TAG_INVALID,
                                 &generic, sizeof(generic), 0u, 0u);
        }
        __atomic_fetch_add(&g_stat_pcie_sections, 1, __ATOMIC_RELAXED);
    } else {
        if (g_tag_generic_err != TOUCH_TAG_INVALID) {
            TouchPublishIrqPair(g_tag_generic_err, TOUCH_TAG_INVALID,
                                 &generic, sizeof(generic), 0u, 0u);
        }
        __atomic_fetch_add(&g_stat_generic_sections, 1, __ATOMIC_RELAXED);
    }

    return (uint32_t)sizeof(gedata_entry_t) + data_len;
}

/* GHESv2 read-ack flow (ACPI 6.5 §18.3.2.7.2). Read the register, mask
 * preserve bits, OR write bits, store back. The read is mandatory —
 * firmware uses it to time the OS's consumption acknowledgement. v1
 * sources skip this entirely (no register specified). */
static void ghes_read_ack(const apei_ghes_source_t *src) {
    if (!(src->flags & SRC_V2)) return;
    if (src->read_ack_addr == 0) return;
    /* Mapped once at registration; see the comment there. Volatile is
     * re-applied at the dereference below, as everywhere else in this file. */
    void *va = src->read_ack_va;
    if (!va) return;
    volatile uint64_t *reg = (volatile uint64_t *)va;
    uint64_t cur = *reg;
    uint64_t nxt = (cur & src->read_ack_preserve) | src->read_ack_write;
    *reg = nxt;
    (void)*reg;            /* read-back to enforce ordering */
}

/* Process one source's GESB. Returns 1 if an event was acked, 0 if no
 * pending error, -1 on corrupt. Safe to call from K-Core context;
 * uses non-blocking primitives in the hot path. */
static int process_source_gesb(apei_ghes_source_t *src) {
    if (!src || !(src->flags & SRC_IN_USE) || !src->gesb_va) return 0;
    volatile gesb_header_t *hdr = (volatile gesb_header_t *)src->gesb_va;
    uint32_t status = __atomic_load_n((uint32_t *)&hdr->block_status, __ATOMIC_ACQUIRE);
    if (status == 0) return 0;          /* no error pending */

    uint32_t data_len = hdr->data_length;
    if (data_len > src->gesb_len - sizeof(gesb_header_t)) {
        __atomic_fetch_add(&g_stat_corrupt_records, 1, __ATOMIC_RELAXED);
        /* Clear block_status to let firmware advance. */
        __atomic_store_n((uint32_t *)&hdr->block_status, 0u, __ATOMIC_RELEASE);
        ghes_read_ack(src);
        return -1;
    }

    /* Iterate Generic Error Data Entries. Bit 13:4 of block_status
     * reports entry count but we walk via byte budget so a stale count
     * cannot drive us off the end. */
    const uint8_t *p   = (const uint8_t *)(uintptr_t)(src->gesb_va) + sizeof(gesb_header_t);
    uint32_t remaining = data_len;
    uint32_t entries   = 0;
    while (remaining >= sizeof(gedata_entry_t)) {
        uint32_t step = process_entry(src, p, remaining);
        if (step == 0) {
            __atomic_fetch_add(&g_stat_corrupt_records, 1, __ATOMIC_RELAXED);
            break;
        }
        p         += step;
        remaining -= step;
        entries++;
    }

    /* Acknowledge: W1C block_status (firmware armed it; OS clears it to
     * indicate consumption). For v2 sources also walk the Read-Ack
     * register protocol. */
    __atomic_store_n((uint32_t *)&hdr->block_status, 0u, __ATOMIC_RELEASE);
    ghes_read_ack(src);

    __atomic_fetch_add(&g_stat_events_processed, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&src->events, 1, __ATOMIC_RELAXED);
    if (entries > 0) {
        __atomic_fetch_add(&src->errors, 1, __ATOMIC_RELAXED);
    }
    return 1;
}

/* ─── Delivery: Polled ──────────────────────────────────────────── */

void apei_ghes_poll_tick(void) {
    if (!g_initialized) return;
    __atomic_fetch_add(&g_stat_poll_ticks, 1, __ATOMIC_RELAXED);

    uint64_t now = apei_ghes_rdtsc();
    uint64_t tsc_ms = apei_ghes_tsc_per_ms();
    for (uint32_t i = 0; i < g_source_count; i++) {
        apei_ghes_source_t *src = &g_sources[i];
        if (!(src->flags & SRC_IN_USE)) continue;
        if (src->notify_type != 0 && src->notify_type != 1) continue;  /* Polled / External-IRQ both polled */
        if (now < src->next_poll_tsc) continue;
        src->next_poll_tsc = now + (uint64_t)src->poll_interval_ms * tsc_ms;
        (void)process_source_gesb(src);
    }
}

/* ─── Delivery: NMI ─────────────────────────────────────────────── */

/* Per-CPU NMI marker so the worker can do the heavy walk in K-Core. */
typedef struct { apei_ghes_source_t *src; } nmi_work_t;

static void nmi_worker(void *ctx) {
    nmi_work_t *w = (nmi_work_t *)ctx;
    if (!w || !w->src) return;
    (void)process_source_gesb(w->src);
}

/* Static work-slot ring — one slot per (source × outstanding count). */
#define APEI_NMI_RING  16u
static nmi_work_t  g_nmi_ring[APEI_NMI_RING];
static volatile uint32_t g_nmi_ring_head = 0;

bool apei_ghes_nmi_check(void) {
    if (!g_initialized) return false;
    __atomic_fetch_add(&g_stat_nmi_invocations, 1, __ATOMIC_RELAXED);

    bool any = false;
    for (uint32_t i = 0; i < g_source_count; i++) {
        apei_ghes_source_t *src = &g_sources[i];
        if (!(src->flags & SRC_IN_USE)) continue;
        if (src->notify_type != 4) continue;
        if (!src->gesb_va) continue;
        volatile gesb_header_t *hdr = (volatile gesb_header_t *)src->gesb_va;
        if (__atomic_load_n((uint32_t *)&hdr->block_status, __ATOMIC_ACQUIRE) == 0) continue;
        any = true;
        uint32_t idx = __atomic_fetch_add(&g_nmi_ring_head, 1, __ATOMIC_RELAXED)
                       & (APEI_NMI_RING - 1u);
        g_nmi_ring[idx].src = src;
        irq_defer(nmi_worker, &g_nmi_ring[idx]);
    }
    return any;
}

/* ─── Delivery: SCI ─────────────────────────────────────────────── */

uint32_t apei_ghes_sci_check(void) {
    if (!g_initialized) return 0;
    __atomic_fetch_add(&g_stat_sci_invocations, 1, __ATOMIC_RELAXED);
    uint32_t fired = 0;
    for (uint32_t i = 0; i < g_source_count; i++) {
        apei_ghes_source_t *src = &g_sources[i];
        if (!(src->flags & SRC_IN_USE)) continue;
        if (src->notify_type != 3) continue;
        if (process_source_gesb(src) == 1) fired++;
    }
    return fired;
}

/* ─── Simulate (test-only) ──────────────────────────────────────── */

uint32_t apei_ghes_simulate(uintptr_t gesb_phys, uint32_t gesb_len) {
    if (!g_initialized) return 0;
    /* Find or create a slot bound to this synthetic GESB. */
    apei_ghes_source_t local = {0};
    local.source_id        = 0xFFFF;
    local.notify_type      = 0;
    local.flags            = SRC_IN_USE;
    local.gsiv             = 0;
    local.poll_interval_ms = 0;
    local.gesb_phys        = gesb_phys;
    local.gesb_len         = gesb_len;
    /* The simulate caller hands a kernel-VA buffer via gesb_phys
     * (we don't actually map mmio in test mode). Reuse it directly. */
    local.gesb_va = (void *)gesb_phys;
    int r = process_source_gesb(&local);
    return (r > 0) ? 1u : 0u;
}

/* ─── Telemetry ─────────────────────────────────────────────────── */

void apei_ghes_get_stats(apei_ghes_stats_t *out) {
    if (!out) return;
    uint32_t polled = 0, nmi = 0, sci = 0, other = 0;
    for (uint32_t i = 0; i < g_source_count; i++) {
        if (!(g_sources[i].flags & SRC_IN_USE)) continue;
        switch (g_sources[i].notify_type) {
            case 0: polled++; break;
            case 3: sci++;    break;
            case 4: nmi++;    break;
            default: other++; break;
        }
    }
    out->sources_registered = g_source_count;
    out->sources_polled = polled;
    out->sources_nmi    = nmi;
    out->sources_sci    = sci;
    out->sources_other  = other;
    out->events_processed     = __atomic_load_n(&g_stat_events_processed,     __ATOMIC_RELAXED);
    out->memory_sections      = __atomic_load_n(&g_stat_memory_sections,      __ATOMIC_RELAXED);
    out->processor_sections   = __atomic_load_n(&g_stat_processor_sections,   __ATOMIC_RELAXED);
    out->pcie_sections        = __atomic_load_n(&g_stat_pcie_sections,        __ATOMIC_RELAXED);
    out->generic_sections     = __atomic_load_n(&g_stat_generic_sections,     __ATOMIC_RELAXED);
    out->mce_migrate_requests = __atomic_load_n(&g_stat_mce_migrate_requests, __ATOMIC_RELAXED);
    out->poll_ticks           = __atomic_load_n(&g_stat_poll_ticks,           __ATOMIC_RELAXED);
    out->nmi_invocations      = __atomic_load_n(&g_stat_nmi_invocations,      __ATOMIC_RELAXED);
    out->sci_invocations      = __atomic_load_n(&g_stat_sci_invocations,      __ATOMIC_RELAXED);
    out->map_failures         = __atomic_load_n(&g_stat_map_failures,         __ATOMIC_RELAXED);
    out->corrupt_records      = __atomic_load_n(&g_stat_corrupt_records,      __ATOMIC_RELAXED);
}

void apei_ghes_dump(void) {
    apei_ghes_stats_t s;
    apei_ghes_get_stats(&s);
    debug_printf("[APEI] runtime stats: srcs=%u (polled=%u nmi=%u sci=%u other=%u) "
                 "events=%lu sections{mem=%lu proc=%lu pcie=%lu gen=%lu} "
                 "mce_req=%lu ticks=%lu nmi=%lu sci=%lu maps_fail=%lu corrupt=%lu\n",
                 (unsigned)s.sources_registered, (unsigned)s.sources_polled,
                 (unsigned)s.sources_nmi, (unsigned)s.sources_sci,
                 (unsigned)s.sources_other,
                 (unsigned long)s.events_processed,
                 (unsigned long)s.memory_sections,
                 (unsigned long)s.processor_sections,
                 (unsigned long)s.pcie_sections,
                 (unsigned long)s.generic_sections,
                 (unsigned long)s.mce_migrate_requests,
                 (unsigned long)s.poll_ticks,
                 (unsigned long)s.nmi_invocations,
                 (unsigned long)s.sci_invocations,
                 (unsigned long)s.map_failures,
                 (unsigned long)s.corrupt_records);
}
