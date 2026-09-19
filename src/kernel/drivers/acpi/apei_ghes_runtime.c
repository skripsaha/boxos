/*
 * APEI/GHES runtime path — implementation.
 *
 * See apei_ghes_runtime.h for design + spec citations. This file owns:
 *   1. Per-source runtime descriptor table, sized from HEST at init.
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
#include "logbook.h"
#include "mce_migrate.h"
#include "mce.h"
#include "baton.h"
#include "cpuid.h"
#include "acpi_internal.h"
#include "io.h"

/* GAS address space identifiers (ACPI 6.5 §5.2.3.2) this file can read. */
#define GAS_AS_SYSTEM_MEMORY  0
#define GAS_AS_SYSTEM_IO      1

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

/* ACPI 6.5 §18.3.2.7.1, Table 18.12 — Generic Error Data Entry.
 *
 * The Timestamp at offset 64 exists only from Revision 0x300 onwards, so
 * the header is 72 bytes at revision >= 0x300 and 64 bytes before it.
 * Linux draws the same line in acpi_hest_get_size(). Reading a pre-0x300
 * entry as 72 bytes puts every section body eight bytes early and shifts
 * the NEXT entry's Section Type GUID out from under the comparison — which
 * on this path means a real ECC error reaches the generic branch instead
 * of mce_migrate_request. */
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
_Static_assert(sizeof(gedata_entry_t) == 72,
               "Generic Error Data Entry = 72 B at revision 0x300");
#define GEDATA_HEADER_PRE_V300  64u

/* Byte length of one Generic Error Data Entry header. Revision is packed
 * major.minor; Linux compares the same way in acpi_hest_get_version(). */
static uint32_t gedata_header_len(uint16_t revision) {
    return ((revision >> 8) >= 3) ? (uint32_t)sizeof(gedata_entry_t)
                                  : GEDATA_HEADER_PRE_V300;
}

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
    uint8_t   flags;              /* bit 0: in_use, 1: v2, 2: unmappable,
                                   * 3: window moved once already */
    uint32_t  gsiv;
    uint32_t  poll_interval_ms;
    uint32_t  gesb_len;
    /* ACPI 6.5 §18.3.2.7, Table 18.10 Error Status Address: the GAS of an
     * 8-byte register that HOLDS the status block's physical address. The
     * register is the source of truth and is read before every walk; the
     * pair below is only the block this source is pointed at right now. */
    uint64_t  status_reg_addr;
    void     *status_reg_va;      /* permanent read-only map (SystemMemory) */
    uint8_t   status_reg_space;
    uint8_t   status_reg_width;   /* bits, after the spec's 8-byte fixup */
    uint64_t  gesb_phys;
    void     *gesb_va;            /* mapped where mapping is allowed, and
                                   * never released — ghes_point_at_block */
    uint64_t  next_poll_tsc;
    /* GHESv2 read-ack registers (zero for v1). */
    uint64_t  read_ack_addr;
    void     *read_ack_va;        /* mapped at register-time, like gesb_va */
    uint64_t  read_ack_preserve;
    uint64_t  read_ack_write;
    /* Per-source counters. */
    volatile uint64_t events;
    volatile uint64_t errors;
    /* The NMI's one pass to a K-Core for this source (baton.h, Knock): an
     * NMI notices the block and knocks; the walk runs where locks are
     * allowed. A burst of NMIs for one block is one walk. */
    Knock     knock;
} apei_ghes_source_t;

#define SRC_IN_USE      (1u << 0)
#define SRC_V2          (1u << 1)
/* Set when the block this source's register names could not be mapped, so
 * the refusal is said once and not once per attempt. */
#define SRC_UNMAPPABLE  (1u << 2)
/* Set the first time the firmware republished the block somewhere else, so
 * that too is said once. See ghes_point_at_block. */
#define SRC_WINDOW_MOVED (1u << 3)

static void ghes_walk_worker(void *ctx);

/* The per-source table: ONE allocation, sized by the firmware, never grown.
 *
 * It used to be sixteen slots of BSS while HEST.ErrorSourceCount (ACPI 6.5
 * §18.3, Table 18-2) is a 32-bit field the spec gives no maximum to — so
 * "sixteen" was a guess about somebody else's machine, and the seventeenth
 * error source on a large server was dropped with a debug_printf that a
 * shipped build compiles away. On the machines where GHES matters most, that
 * is silently losing the DIMM that is failing.
 *
 * Heap-backed, and the three rules that make that SAFE are the whole design,
 * because this table is walked from NMI CONTEXT (idt.c -> apei_ghes_nmi_check)
 * as well as from the SCI handler and the scheduler tick:
 *
 *   1. ONE allocation. Made on the first registration, never grown, never
 *      moved, never freed. There is no window in which the table's address
 *      changes under a walker.
 *   2. PUBLISHED BY ONE RELEASE STORE, and every walker loads the pointer with
 *      ACQUIRE. A walker that sees the pointer sees a fully built descriptor
 *      behind it.
 *   3. NO WALKER RUNS WHILE IT IS BEING BUILT. Every registration comes from
 *      decode_ghes during acpi_parse_apei, which runs on the BSP inside
 *      acpi_init; all three entry points return immediately while
 *      g_initialized is false, and g_initialized is set only by
 *      apei_ghes_runtime_init, which runs after acpi_init has finished. An NMI
 *      landing mid-build therefore sees no table at all — never half of one.
 *
 * A grow-and-copy table would break rule 1 and could not be freed safely from
 * under an NMI at all, which is exactly why the count comes from the firmware
 * once instead of being discovered as sources arrive. */
static apei_ghes_source_t *g_sources = NULL;
static uint32_t            g_source_capacity = 0;
static volatile uint32_t   g_source_count = 0;

/* Make the table exist, sized from the firmware's own count.
 *
 * ErrorSourceCount is a CLAIM; the table's length is a FACT. A corrupt or
 * hostile count — the field is 32 bits and unbounded — would otherwise size an
 * allocation nothing can serve, and losing GHES entirely to a bad number is
 * worse than the sixteen-slot guess this replaces. So the claim is capped by
 * what the HEST body can physically describe: the walk in acpi_parse_apei (the
 * only thing that ever reaches this function) must read at least a type and a
 * source id — four bytes — before it can classify an entry at all, so the
 * table cannot describe more sources than its body holds four-byte steps.
 * Normal firmware makes the claim the smaller of the two, and the ceiling
 * never comes into play. */
static bool ghes_table_ready(void) {
    if (g_sources) return true;

    uint32_t claimed = g_acpi.apei.hest_error_source_count;
    if (claimed == 0) return false;

    const acpi_sdt_header_t *hest = acpi_find_table("HEST");
    if (!hest || hest->length <= sizeof(acpi_hest_t)) return false;
    uint32_t ceiling = (hest->length - (uint32_t)sizeof(acpi_hest_t)) / 4u;
    uint32_t slots   = (claimed < ceiling) ? claimed : ceiling;
    if (slots == 0) return false;

    apei_ghes_source_t *table =
        (apei_ghes_source_t *)kmalloc((size_t)slots * sizeof(*table));
    if (!table) {
        kprintf("[APEI] cannot hold %u GHES source(s) — no memory for the "
                "runtime table; firmware-delivered errors will not be "
                "processed on this boot\n", slots);
        return false;
    }
    memset(table, 0, (size_t)slots * sizeof(*table));

    g_source_capacity = slots;
    __atomic_store_n(&g_sources, table, __ATOMIC_RELEASE);
    debug_printf("[APEI] runtime source table: %u slot(s) from HEST "
                 "(claimed=%u, table ceiling=%u)\n", slots, claimed, ceiling);
    return true;
}

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
    g_tag_mem_error   = TouchLogbookIntern("apei:memory:error");
    g_tag_proc_error  = TouchLogbookIntern("apei:processor:error");
    g_tag_pcie_error  = TouchLogbookIntern("apei:pcie:error");
    g_tag_generic_err = TouchLogbookIntern("apei:generic:error");
    g_tag_ready       = TouchLogbookIntern("apei:ghes:ready");
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
                                   uint64_t status_reg_addr,
                                   uint8_t  status_reg_space,
                                   uint8_t  status_reg_width,
                                   uint64_t gesb_phys,
                                   uint32_t gesb_len,
                                   uint64_t read_ack_addr,
                                   uint64_t read_ack_preserve,
                                   uint64_t read_ack_write) {
    /* The REGISTER is what a source needs to exist; the block address is
     * only what that register happened to say at boot, and 0 there means
     * "the firmware has published no block yet" — not a broken source.
     * Refusing on it, as this function used to, threw away every source
     * whose block arrives after we looked. */
    if (status_reg_addr == 0 || gesb_len < sizeof(gesb_header_t))
        return ERR_INVALID_ARGUMENT;
    if (status_reg_space != GAS_AS_SYSTEM_MEMORY &&
        status_reg_space != GAS_AS_SYSTEM_IO) {
        kprintf("[APEI] GHES src=%u: error status address register lives in "
                "address space %u, which this kernel cannot read\n",
                source_id, status_reg_space);
        return ERR_INVALID_ARGUMENT;
    }

    if (!ghes_table_ready()) return ERR_NO_MEMORY;

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
        if (g_source_count >= g_source_capacity) {
            /* kprintf, not debug_printf: a source dropped here is a class of
             * hardware error this machine will never report again, and that
             * sentence has to survive into a shipped build. */
            kprintf("[APEI] HEST described more sources than it claimed — no "
                    "slot for src=%u (capacity %u)\n",
                    source_id, g_source_capacity);
            return ERR_NO_MEMORY;
        }
        slot = &g_sources[g_source_count];
        g_source_count++;
    }

    /* Map the status-address REGISTER once, read-only: it belongs to the
     * firmware and the OS has no business writing it, ever. This mapping is
     * what makes the re-read before every walk possible from NMI context,
     * where no mapping may be made — the same reason the read-ack register
     * below is mapped here. An I/O-port register needs no mapping. */
    void *status_reg_va = NULL;
    if (status_reg_space == GAS_AS_SYSTEM_MEMORY) {
        if (slot->status_reg_va && slot->status_reg_addr == status_reg_addr) {
            status_reg_va = slot->status_reg_va;      /* re-registration */
        } else {
            status_reg_va = (void *)(uintptr_t)
                vmm_map_mmio(status_reg_addr, 8, VMM_FLAGS_KERNEL_RO);
        }
        if (!status_reg_va) {
            kprintf("[APEI] GHES src=%u: cannot map error status address "
                    "register 0x%lx — this source cannot report a hardware "
                    "error\n", source_id, (unsigned long)status_reg_addr);
            __atomic_fetch_add(&g_stat_map_failures, 1, __ATOMIC_RELAXED);
            slot->flags = 0;
            return ERR_NO_MEMORY;
        }
    }

    /* Map the block the register named at boot, when it named one. Read and
     * write, so we can W1C block_status to acknowledge. vmm_map_mmio returns
     * volatile void* (MMIO accesses must not be cached or reordered by the
     * compiler); we store the address as plain void* in the slot and
     * re-apply the volatile qualifier at every dereference site (see
     * process_source_gesb / ghes_read_ack which cast to `volatile <type>*`).
     * The explicit cast here makes the qualifier drop intentional rather
     * than implicit. A source whose register still reads 0 is registered
     * without a window; the first walk that finds an address maps it. */
    void *gesb_va = NULL;
    if (gesb_phys != 0) {
        gesb_va = (void *)(uintptr_t)vmm_map_mmio(gesb_phys, gesb_len,
                                                   VMM_FLAGS_KERNEL_RW);
        if (!gesb_va) {
            kprintf("[APEI] GHES src=%u: error status block at 0x%lx (%u B) "
                    "cannot be mapped — this source cannot report a hardware "
                    "error\n", source_id, (unsigned long)gesb_phys, gesb_len);
            __atomic_fetch_add(&g_stat_map_failures, 1, __ATOMIC_RELAXED);
            slot->flags = 0;
            return ERR_NO_MEMORY;
        }
    }

    slot->source_id          = source_id;
    slot->notify_type        = notify_type;
    slot->flags              = SRC_IN_USE | (v2 ? SRC_V2 : 0);
    slot->gsiv               = vector;
    slot->poll_interval_ms   = poll_interval_ms ? poll_interval_ms : 1000u;
    slot->status_reg_addr    = status_reg_addr;
    slot->status_reg_va      = status_reg_va;
    slot->status_reg_space   = status_reg_space;
    slot->status_reg_width   = status_reg_width;
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
    KnockInit(&slot->knock, ghes_walk_worker, slot);

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
    /* Revision sits at offset 20, inside the shorter of the two headers, so
     * it can be read before the header's own length is known. */
    if (avail < GEDATA_HEADER_PRE_V300) return 0;
    const gedata_entry_t *ge = (const gedata_entry_t *)entry;
    uint32_t hdr_len  = gedata_header_len(ge->revision);
    if (avail < hdr_len) return 0;
    uint32_t data_len = ge->error_data_length;
    if ((uint64_t)data_len + hdr_len > avail) return 0;

    const uint8_t *body = entry + hdr_len;

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

    return hdr_len + data_len;
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

/* Read the Error Status Address register and return the physical address of
 * the status block it currently names. 0 means the firmware has published
 * no block for this source right now — a normal answer, not a fault
 * (Linux's __ghes_peek_estatus returns -ENOENT for the same value).
 *
 * Lock-free and allocation-free by construction: the register was mapped
 * once at registration, or it is an I/O port. That is what lets the NMI path
 * ask this question where nothing else may be done. */
static uint64_t ghes_read_status_register(const apei_ghes_source_t *src) {
    if (src->status_reg_space == GAS_AS_SYSTEM_IO) {
        uint16_t port = (uint16_t)src->status_reg_addr;
        switch (src->status_reg_width) {
            case 8:  return inb(port);
            case 16: return inw(port);
            case 32: return inl(port);
            default: return 0;    /* no 64-bit port access exists on x86 */
        }
    }
    if (!src->status_reg_va) return 0;
    volatile void *reg = src->status_reg_va;
    switch (src->status_reg_width) {
        case 8:  return *(volatile uint8_t  *)reg;
        case 16: return *(volatile uint16_t *)reg;
        case 32: return *(volatile uint32_t *)reg;
        default: return *(volatile uint64_t *)reg;
    }
}

/* Point a source at the block its register now names.
 *
 * Mapping takes kernel locks and may allocate page tables, so this runs
 * ONLY where that is allowed — the K-Core walk behind the source's knock.
 *
 * A WINDOW THIS FUNCTION HAS PUBLISHED IS NEVER RELEASED, and that is a
 * decision, not an oversight. An NMI on any core reads the window to learn
 * whether the source holds an unconsumed record — that is the question
 * vector 2 must answer to know whether the interrupt was its own — and an
 * NMI cannot be held off while this core tears a mapping down. Unmapping
 * here would leave a window in which an NMI that had already loaded the
 * old address dereferences a page that no longer maps, and a #PF taken
 * inside an NMI handler is not survivable: the fault's IRET unblocks NMIs
 * while the handler is still on its IST stack. Keeping the old window
 * costs one MMIO mapping per DISTINCT block address a source ever
 * publishes — firmware publishes one and keeps it, which is why the
 * pre-allocation fields exist in Table 18.10 at all — and buys an NMI path
 * that cannot fault by construction.
 *
 * The pair (gesb_phys, gesb_va) is published va-first so a reader that
 * sees the new address also sees the window that maps it: gesb_va with a
 * release store, then gesb_phys with a release store, and readers load
 * gesb_phys with acquire before gesb_va. */
static bool ghes_point_at_block(apei_ghes_source_t *src, uint64_t phys) {
    void *old = src->gesb_va;

    void *va = (void *)(uintptr_t)vmm_map_mmio(phys, src->gesb_len,
                                                VMM_FLAGS_KERNEL_RW);
    if (!va) {
        /* The old window stays mapped and stays readable; what stops it
         * being read is that gesb_phys no longer matches any address the
         * register can name. */
        __atomic_store_n(&src->gesb_phys, (uint64_t)0, __ATOMIC_RELEASE);
        __atomic_fetch_add(&g_stat_map_failures, 1, __ATOMIC_RELAXED);
        /* Said once per source: the firmware keeps signalling and the poll
         * keeps coming, and a line per attempt would bury the console under
         * the same sentence. The counter above keeps the tally. */
        if (!(src->flags & SRC_UNMAPPABLE)) {
            src->flags |= SRC_UNMAPPABLE;
            kprintf("[APEI] GHES src=%u: the error status block its register "
                    "names (0x%lx, %u B) cannot be mapped — this source "
                    "cannot report a hardware error\n",
                    src->source_id, (unsigned long)phys, src->gesb_len);
        }
        return false;
    }
    src->flags &= (uint8_t)~SRC_UNMAPPABLE;
    __atomic_store_n(&src->gesb_va, va, __ATOMIC_RELEASE);
    __atomic_store_n(&src->gesb_phys, phys, __ATOMIC_RELEASE);
    debug_printf("[APEI] GHES src=%u now points at status block 0x%lx\n",
                 src->source_id, (unsigned long)phys);
    /* Said once per source, because a firmware that moves the block is
     * telling the operator something about itself, and because the mapping
     * the move leaves behind is kept for the life of the machine. */
    if (old && old != va && !(src->flags & SRC_WINDOW_MOVED)) {
        src->flags |= SRC_WINDOW_MOVED;
        kprintf("[APEI] GHES src=%u: firmware republished its error status "
                "block at 0x%lx; the previous window stays mapped because an "
                "NMI on another core may be reading it\n",
                src->source_id, (unsigned long)phys);
    }
    return true;
}

/* Process one source's GESB. Returns 1 if an event was acked, 0 if no
 * pending error, -1 on corrupt. Safe to call from K-Core context;
 * uses non-blocking primitives in the hot path.
 *
 * `may_map` says whether this caller stands where a mapping may be made.
 * Only the K-Core walk does; the PIT tick and the SCI handler do not, and
 * when one of them finds the register naming a block that is not the mapped
 * one it knocks instead of reading the wrong memory. */
static int process_source_gesb(apei_ghes_source_t *src, bool may_map) {
    if (!src || !(src->flags & SRC_IN_USE)) return 0;

    /* The block address is read from the register EVERY time, because that
     * is what the register is for (ACPI 6.5 Table 18.10: "a register that
     * contains the physical address of a block of memory that holds the
     * error status data"). A snapshot taken once while walking HEST follows
     * a firmware that republishes the block nowhere. Linux re-reads the same
     * register in __ghes_peek_estatus() before every parse. */
    if (src->status_reg_addr != 0) {
        uint64_t phys = ghes_read_status_register(src);
        if (phys == 0) return 0;          /* no block published right now */
        if (phys != __atomic_load_n(&src->gesb_phys, __ATOMIC_ACQUIRE) ||
            !__atomic_load_n(&src->gesb_va, __ATOMIC_ACQUIRE)) {
            if (!may_map) { KnockOn(&src->knock); return 0; }
            if (!ghes_point_at_block(src, phys)) return -1;
        }
    }

    void *window = __atomic_load_n(&src->gesb_va, __ATOMIC_ACQUIRE);
    if (!window) return 0;
    volatile gesb_header_t *hdr = (volatile gesb_header_t *)window;
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
    const uint8_t *p   = (const uint8_t *)window + sizeof(gesb_header_t);
    uint32_t remaining = data_len;
    uint32_t entries   = 0;
    while (remaining >= GEDATA_HEADER_PRE_V300) {
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

    apei_ghes_source_t *table = __atomic_load_n(&g_sources, __ATOMIC_ACQUIRE);
    if (!table) return;

    uint64_t now = apei_ghes_rdtsc();
    uint64_t tsc_ms = apei_ghes_tsc_per_ms();
    for (uint32_t i = 0; i < g_source_count; i++) {
        apei_ghes_source_t *src = &table[i];
        if (!(src->flags & SRC_IN_USE)) continue;
        if (src->notify_type != 0 && src->notify_type != 1) continue;  /* Polled / External-IRQ both polled */
        if (now < src->next_poll_tsc) continue;
        src->next_poll_tsc = now + (uint64_t)src->poll_interval_ms * tsc_ms;
        /* The PIT tick may have interrupted kernel code holding the very
         * locks a mapping needs, so this caller never maps: a block that
         * moved is walked by the K-Core behind the source's knock. */
        (void)process_source_gesb(src, false);
    }
}

/* ─── Delivery: NMI ─────────────────────────────────────────────── */

/* The walk, on a K-Core, once per knock on the source. The door opens
 * first: an NMI that lands during the walk brings another visit, and a visit
 * that finds the block already acknowledged returns at once. This is the one
 * caller that stands where a mapping may be made, so it is also where a
 * block the firmware has republished elsewhere is followed. */
static void ghes_walk_worker(void *ctx) {
    apei_ghes_source_t *src = (apei_ghes_source_t *)ctx;
    KnockOpen(&src->knock);
    (void)process_source_gesb(src, true);
}

bool apei_ghes_nmi_check(void) {
    if (!g_initialized) return false;
    __atomic_fetch_add(&g_stat_nmi_invocations, 1, __ATOMIC_RELAXED);

    /* ACQUIRE against the single RELEASE store that published the table. This
     * is NMI context: the only table it can ever see is a finished one. */
    apei_ghes_source_t *table = __atomic_load_n(&g_sources, __ATOMIC_ACQUIRE);
    if (!table) return false;

    bool any = false;
    for (uint32_t i = 0; i < g_source_count; i++) {
        apei_ghes_source_t *src = &table[i];
        if (!(src->flags & SRC_IN_USE)) continue;
        if (src->notify_type != 4) continue;
        if (src->status_reg_addr == 0) continue;

        /* Two loads, both of which an NMI may make: the register (mapped
         * once at registration, never remapped) and the window (published
         * once per block address and never released — see
         * ghes_point_at_block). Neither can fault and neither takes a lock. */
        uint64_t phys = ghes_read_status_register(src);
        if (phys == 0) continue;      /* no block published: not this source */

        uint64_t mapped = __atomic_load_n(&src->gesb_phys, __ATOMIC_ACQUIRE);
        void    *window = __atomic_load_n(&src->gesb_va,   __ATOMIC_ACQUIRE);
        if (phys == mapped && window) {
            /* The question vector 2 actually has to answer: does this source
             * hold a record nobody has consumed? Block Status zero means the
             * firmware has armed nothing, so this NMI came from somewhere
             * else and must be left to the handler's other branches — that
             * is the whole point of the "unclaimed" line in idt.c. */
            volatile gesb_header_t *hdr = (volatile gesb_header_t *)window;
            uint32_t status = __atomic_load_n((uint32_t *)&hdr->block_status,
                                               __ATOMIC_ACQUIRE);
            if (status == 0) continue;
            any = true;
            /* The source's own knock: allocation-free, never dropped, and a
             * burst of NMIs for one block is one walk. */
            KnockOn(&src->knock);
            continue;
        }

        /* The register names a block this kernel has no window for: either
         * the source was registered before the firmware published one, or
         * the firmware has just republished it elsewhere. Both are the
         * firmware speaking, so the NMI is claimed and the K-Core walk maps
         * the block and reads it. A source whose block could not be mapped
         * is the exception — it has already said so once, and claiming every
         * NMI forever on its behalf would hide every other NMI reason for
         * the life of the machine. */
        KnockOn(&src->knock);
        if (!(src->flags & SRC_UNMAPPABLE)) any = true;
    }
    return any;
}

/* ─── Delivery: SCI ─────────────────────────────────────────────── */

uint32_t apei_ghes_sci_check(void) {
    if (!g_initialized) return 0;
    __atomic_fetch_add(&g_stat_sci_invocations, 1, __ATOMIC_RELAXED);
    apei_ghes_source_t *table = __atomic_load_n(&g_sources, __ATOMIC_ACQUIRE);
    if (!table) return 0;

    uint32_t fired = 0;
    for (uint32_t i = 0; i < g_source_count; i++) {
        apei_ghes_source_t *src = &table[i];
        if (!(src->flags & SRC_IN_USE)) continue;
        if (src->notify_type != 3) continue;
        /* The SCI handler is an interrupt too: it never maps. A source
         * whose block moved is handed to the K-Core walk by the knock
         * process_source_gesb raises. */
        if (process_source_gesb(src, false) == 1) fired++;
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
    /* No status register: the caller hands the block itself, so nothing is
     * re-read and nothing is mapped. */
    int r = process_source_gesb(&local, false);
    return (r > 0) ? 1u : 0u;
}

/* ─── Telemetry ─────────────────────────────────────────────────── */

void apei_ghes_get_stats(apei_ghes_stats_t *out) {
    if (!out) return;
    uint32_t polled = 0, nmi = 0, sci = 0, other = 0;
    apei_ghes_source_t *table = __atomic_load_n(&g_sources, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; table && i < g_source_count; i++) {
        if (!(table[i].flags & SRC_IN_USE)) continue;
        switch (table[i].notify_type) {
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
