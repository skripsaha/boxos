#include "acpi_internal.h"
#include "klib.h"
#include "vmm.h"

/*
 * ACPI 6.5 §5.2.6 — every System Description Table starts with a header
 * carrying a signature, length and one-byte checksum. The checksum is the
 * negative of the sum of the remaining bytes; i.e. the total sum of all
 * `length` bytes must equal zero. Software MUST verify before trusting
 * any contents.
 */
bool acpi_validate_table(const acpi_sdt_header_t* header) {
    if (!header) return false;

    /* `length` < sizeof(header) is firmware nonsense — reading more bytes
     * would chase OOB memory. */
    if (header->length < sizeof(acpi_sdt_header_t)) {
        debug_printf("[ACPI] Table %.4s length %u below header size\n",
                     header->signature, header->length);
        return false;
    }

    if (acpi_checksum((void*)header, header->length) != 0) {
        debug_printf("[ACPI] Table %.4s checksum failed (len=%u)\n",
                     header->signature, header->length);
        return false;
    }

    return true;
}

/*
 * Map a subordinate table at `phys`. Two-step process because the length
 * isn't known until we read the header:
 *   1. Map the 36-byte header to learn `length`.
 *   2. Remap the full table.
 *
 * Returns the mapped pointer (with length-side mapping) on success, or
 * NULL on map / validation failure.
 *
 * The returned pointer is intentionally never unmapped — ACPI tables are
 * referenced for the kernel lifetime (shutdown needs the FADT, MADT-
 * derived state stays live too).
 */
/* Physical address -> the mapping we already made for it.
 *
 * An ACPI table is immortal: it is mapped at boot, never unmapped, and its
 * contents never move. So the mapping should be made once — and until now it
 * was made again on every visit. acpi_find_table() walks the root table and
 * maps EVERY entry until a signature matches, and map_and_validate_table()
 * maps twice per visit (header first, then the declared length). A dozen
 * lookups across a machine with thirty tables is several hundred fresh MMIO
 * mappings, each burning kernel virtual address space and the page-table
 * pages under it, none of which is ever reclaimed. QEMU publishes six tables
 * and hid the cost; a real board publishes three times as many.
 *
 * Cached on the miss too. A table whose checksum fails will fail it again.
 */
static acpi_sdt_header_t* map_and_validate_table_uncached(uintptr_t phys);

#define ACPI_MAP_CACHE_SIZE 64
static struct {
    uintptr_t          phys;
    acpi_sdt_header_t* hdr;
} g_map_cache[ACPI_MAP_CACHE_SIZE];
static uint8_t g_map_cache_used = 0;

static acpi_sdt_header_t* map_and_validate_table(uintptr_t phys) {
    for (uint8_t i = 0; i < g_map_cache_used; i++)
        if (g_map_cache[i].phys == phys)
            return g_map_cache[i].hdr;

    acpi_sdt_header_t* result = map_and_validate_table_uncached(phys);

    /* Past the cache size we still answer correctly, just without the saving.
     * Sixty-four is comfortably more tables than any firmware publishes; a
     * machine that exceeds it gets the old behaviour, not a wrong one. */
    if (g_map_cache_used < ACPI_MAP_CACHE_SIZE) {
        g_map_cache[g_map_cache_used].phys = phys;
        g_map_cache[g_map_cache_used].hdr  = result;
        g_map_cache_used++;
    }
    return result;
}

static acpi_sdt_header_t* map_and_validate_table_uncached(uintptr_t phys) {
    acpi_sdt_header_t* hdr = (acpi_sdt_header_t*)acpi_map_physical(
        phys, sizeof(acpi_sdt_header_t));
    if (!hdr) {
        debug_printf("[ACPI] Failed to map table header at 0x%lx\n",
                     (unsigned long)phys);
        return NULL;
    }
    if (hdr->length < sizeof(acpi_sdt_header_t)) {
        debug_printf("[ACPI] Table at 0x%lx declares length %u < header (%lu)\n",
                     (unsigned long)phys, hdr->length,
                     (unsigned long)sizeof(acpi_sdt_header_t));
        return NULL;
    }

    acpi_sdt_header_t* full = (acpi_sdt_header_t*)acpi_map_physical(
        phys, hdr->length);
    if (!full) {
        debug_printf("[ACPI] Failed to remap table %.4s with full length %u\n",
                     hdr->signature, hdr->length);
        return NULL;
    }

    if (!acpi_validate_table(full)) {
        /* Caller decides whether absence is fatal; we just don't trust
         * a checksum-failed table. */
        return NULL;
    }

    return full;
}

/* Cached pointers to RSDT/XSDT — produced once, used by all table lookups. */
static acpi_rsdt_t* g_rsdt = NULL;
static acpi_xsdt_t* g_xsdt = NULL;
static uint32_t g_rsdt_count = 0;
static uint32_t g_xsdt_count = 0;

/* Pull RSDT and XSDT into the global cache. Either may be absent; both
 * must validate before being trusted. */
static acpi_error_t cache_root_tables(acpi_rsdp_t* rsdp) {
    g_rsdt = NULL;
    g_xsdt = NULL;
    g_rsdt_count = 0;
    g_xsdt_count = 0;

    if (rsdp->revision >= 2 && rsdp->xsdt_address) {
        acpi_sdt_header_t* hdr = map_and_validate_table(
            (uintptr_t)rsdp->xsdt_address);
        if (hdr && memcmp(hdr->signature, "XSDT", 4) == 0) {
            g_xsdt = (acpi_xsdt_t*)hdr;
            g_xsdt_count = (hdr->length - sizeof(acpi_sdt_header_t))
                           / sizeof(uint64_t);
            debug_printf("[ACPI] XSDT at 0x%lx, %u entries\n",
                         (unsigned long)rsdp->xsdt_address, g_xsdt_count);
        } else {
            debug_printf("[ACPI] XSDT at 0x%lx invalid; "
                         "spec violation — will fall back to RSDT\n",
                         (unsigned long)rsdp->xsdt_address);
        }
    }

    if (rsdp->rsdt_address) {
        acpi_sdt_header_t* hdr = map_and_validate_table(
            (uintptr_t)rsdp->rsdt_address);
        if (hdr && memcmp(hdr->signature, "RSDT", 4) == 0) {
            g_rsdt = (acpi_rsdt_t*)hdr;
            g_rsdt_count = (hdr->length - sizeof(acpi_sdt_header_t))
                           / sizeof(uint32_t);
            debug_printf("[ACPI] RSDT at 0x%x, %u entries\n",
                         rsdp->rsdt_address, g_rsdt_count);
        }
    }

    if (!g_xsdt && !g_rsdt) {
        debug_printf("[ACPI] Neither XSDT nor RSDT could be mapped/validated\n");
        return ACPI_ERR_RSDT_NOT_FOUND;
    }
    return ACPI_OK;
}

/* Internal callback wiring for acpi_for_each_table / acpi_find_table.
 *
 * ACPI 6.5 §5.2.5.3: when both RSDT and XSDT are present, OSPM MUST use
 * XSDT. We honour that strictly — RSDT is only consulted when XSDT is
 * missing or unparseable.
 */
typedef struct {
    const char* signature;
    acpi_table_iter_cb_t cb;
    void* user;
    acpi_sdt_header_t* first_hit;   /* set by find_first_cb */
} iter_ctx_t;

static bool find_first_cb(acpi_sdt_header_t* tbl, void* user) {
    iter_ctx_t* ctx = (iter_ctx_t*)user;
    ctx->first_hit = tbl;
    return false;                   /* stop iteration */
}

static void iterate_xsdt(iter_ctx_t* ctx) {
    if (!g_xsdt) return;
    for (uint32_t i = 0; i < g_xsdt_count; i++) {
        uint64_t phys = g_xsdt->entries[i];
        if (phys == 0) continue;
        acpi_sdt_header_t* hdr = map_and_validate_table((uintptr_t)phys);
        if (!hdr) continue;
        if (memcmp(hdr->signature, ctx->signature, 4) != 0) continue;
        if (!ctx->cb(hdr, ctx->user))
            return;
    }
}

static void iterate_rsdt(iter_ctx_t* ctx) {
    if (!g_rsdt) return;
    for (uint32_t i = 0; i < g_rsdt_count; i++) {
        uint32_t phys = g_rsdt->entries[i];
        if (phys == 0) continue;
        acpi_sdt_header_t* hdr = map_and_validate_table((uintptr_t)phys);
        if (!hdr) continue;
        if (memcmp(hdr->signature, ctx->signature, 4) != 0) continue;
        if (!ctx->cb(hdr, ctx->user))
            return;
    }
}

void acpi_for_each_table(const char* signature,
                         acpi_table_iter_cb_t cb,
                         void* user) {
    if (!signature || !cb) return;
    iter_ctx_t ctx = { signature, cb, user, NULL };
    /* XSDT is the spec-mandated source when available. */
    if (g_xsdt) {
        iterate_xsdt(&ctx);
    } else {
        iterate_rsdt(&ctx);
    }
}

/* Signature → header* cache. Populated lazily on first hit. ACPI
 * tables live for the kernel lifetime so once mapped, the pointer is
 * stable forever — perfect for a small open-addressed cache. */
#define ACPI_FIND_CACHE_SIZE   32
static struct {
    char sig[4];
    acpi_sdt_header_t* hdr;
} g_find_cache[ACPI_FIND_CACHE_SIZE];
static uint8_t g_find_cache_used = 0;

acpi_sdt_header_t* acpi_find_table(const char* signature) {
    if (!signature) return NULL;
    /* O(1)-amortised cache probe. The set is tiny (<= 12 named ACPI
     * tables on most platforms) so a linear scan over `used` is
     * cache-line friendly and beats a hash table. */
    for (uint8_t i = 0; i < g_find_cache_used; i++) {
        if (g_find_cache[i].sig[0] == signature[0] &&
            g_find_cache[i].sig[1] == signature[1] &&
            g_find_cache[i].sig[2] == signature[2] &&
            g_find_cache[i].sig[3] == signature[3]) {
            return g_find_cache[i].hdr;
        }
    }

    iter_ctx_t ctx = { signature, find_first_cb, NULL, NULL };
    ctx.user = &ctx;
    if (g_xsdt) iterate_xsdt(&ctx);
    else        iterate_rsdt(&ctx);

    /* Cache the hit (and the miss — NULL is also stable). */
    if (g_find_cache_used < ACPI_FIND_CACHE_SIZE) {
        g_find_cache[g_find_cache_used].sig[0] = signature[0];
        g_find_cache[g_find_cache_used].sig[1] = signature[1];
        g_find_cache[g_find_cache_used].sig[2] = signature[2];
        g_find_cache[g_find_cache_used].sig[3] = signature[3];
        g_find_cache[g_find_cache_used].hdr    = ctx.first_hit;
        g_find_cache_used++;
    }
    return ctx.first_hit;
}

/* DSDT body walker for _S5 — iterates DSDT itself and every SSDT. */
typedef struct {
    bool found;
} s5_ctx_t;

static bool s5_in_table_cb(acpi_sdt_header_t* tbl, void* user) {
    s5_ctx_t* ctx = (s5_ctx_t*)user;
    uint32_t hdr_size = (uint32_t)sizeof(acpi_sdt_header_t);
    if (tbl->length <= hdr_size) return true;     /* keep searching */

    uint8_t* aml = (uint8_t*)tbl + hdr_size;
    uint32_t aml_len = tbl->length - hdr_size;

    if (acpi_search_s5_in_aml(aml, aml_len)) {
        debug_printf("[ACPI] _S5 located inside %.4s (len=%u)\n",
                     tbl->signature, tbl->length);
        ctx->found = true;
        return false;
    }
    return true;
}

/* What the firmware actually handed us, said out loud once.
 *
 * kprintf rather than debug_printf, deliberately. Which root table the
 * firmware published and which tables hang off it is the first question asked
 * of every machine BoxOS has never run on before, and the answer decides
 * whether a missing subsystem is a missing feature or a parser that walked
 * past it. Hiding that behind a build flag means the one boot where it matters
 * — the first one, on someone else's hardware, photographed off a screen — is
 * the boot that does not have it. It costs one line and one pass over at most
 * a few dozen pointers, once.
 */
static void acpi_log_inventory(acpi_rsdp_t* rsdp) {
    kprintf("[ACPI] RSDP rev=%u", (unsigned)rsdp->revision);
    if (g_xsdt)
        kprintf(" XSDT=0x%lx (%u tables)",
                (unsigned long)rsdp->xsdt_address, g_xsdt_count);
    if (g_rsdt)
        kprintf(" RSDT=0x%lx (%u tables)%s",
                (unsigned long)rsdp->rsdt_address, g_rsdt_count,
                g_xsdt ? " [unused: XSDT wins]" : "");
    kprintf("\n[ACPI] tables: ");

    /* One pass over whichever root table is authoritative. The iterator
     * filters by signature, so ask it for every signature we see by walking
     * the root ourselves — cheaper and simpler than 30 lookups. */
    uint32_t n = g_xsdt ? g_xsdt_count : g_rsdt_count;
    uint32_t shown = 0;
    for (uint32_t i = 0; i < n; i++) {
        uintptr_t phys = g_xsdt ? (uintptr_t)g_xsdt->entries[i]
                                : (uintptr_t)g_rsdt->entries[i];
        if (!phys) continue;
        acpi_sdt_header_t* hdr = map_and_validate_table(phys);
        if (!hdr) { kprintf("<bad@0x%lx> ", (unsigned long)phys); continue; }
        kprintf("%.4s ", hdr->signature);
        shown++;
    }
    if (shown == 0) kprintf("(none)");
    kprintf("\n");
}

acpi_error_t acpi_parse_tables(acpi_rsdp_t* rsdp) {
    if (!rsdp) return ACPI_ERR_INVALID_RSDP;

    acpi_error_t err = cache_root_tables(rsdp);
    if (err != ACPI_OK) return err;

    acpi_log_inventory(rsdp);

    /* FADT lookup (signature "FACP"). */
    acpi_fadt_t* fadt = (acpi_fadt_t*)acpi_find_table("FACP");
    if (!fadt) {
        debug_printf("[ACPI] FADT not found\n");
        return ACPI_ERR_FADT_NOT_FOUND;
    }

    g_acpi.fadt = fadt;
    g_acpi.pm1a_cnt_blk = fadt->pm1a_control_block;
    g_acpi.pm1b_cnt_blk = fadt->pm1b_control_block;

    /* ACPI 6.5 §4.8.3.3 — Power Management Timer block.
     *   pm_timer_length == 4 → 24/32-bit I/O port at pm_timer_block.
     *   FADT.flags bit 8 (TMR_VAL_EXT, 0x100) → 32-bit, else 24-bit.
     * Prefer X_PM_TIMER_BLOCK GAS when the FADT is long enough and
     * the GAS is non-zero (ACPI 2.0+); fall back to legacy 32-bit
     * pm_timer_block I/O port. SystemMemory GAS is rare for PM
     * Timer; we only consume SystemIO (address_space == 1). */
    g_acpi.pm_timer_present = false;
    g_acpi.pm_timer_io      = 0;
    g_acpi.pm_timer_bits    = 0;
    if (fadt->pm_timer_length == 4 && fadt->pm_timer_block != 0) {
        bool use_x = (fadt->header.length >=
                      offsetof(acpi_fadt_t, x_pm_timer_block) + sizeof(acpi_gas_t)) &&
                     fadt->x_pm_timer_block.address != 0 &&
                     fadt->x_pm_timer_block.address_space == 1;  /* SystemIO */
        uint32_t io_port = use_x ? (uint32_t)fadt->x_pm_timer_block.address
                                  : fadt->pm_timer_block;
        g_acpi.pm_timer_io      = io_port;
        g_acpi.pm_timer_bits    = (fadt->flags & 0x100) ? 32 : 24;
        g_acpi.pm_timer_present = true;
        debug_printf("[ACPI] PM Timer: I/O 0x%x, %u-bit, 3.579545 MHz\n",
                     g_acpi.pm_timer_io, g_acpi.pm_timer_bits);
    } else {
        debug_printf("[ACPI] PM Timer not advertised\n");
    }

    debug_printf("[ACPI] FADT rev=%u len=%u PM1a_CNT=0x%x PM1b_CNT=0x%x\n",
                 fadt->header.revision, fadt->header.length,
                 g_acpi.pm1a_cnt_blk, g_acpi.pm1b_cnt_blk);

    if (g_acpi.pm1a_cnt_blk == 0 &&
        (fadt->header.length < offsetof(acpi_fadt_t, x_pm1a_control_block) +
                                   sizeof(acpi_gas_t) ||
         fadt->x_pm1a_control_block.address == 0)) {
        debug_printf("[ACPI] WARNING: neither legacy PM1a_CNT nor X_PM1a_CNT available\n");
    }

    /* DSDT resolution.
     *
     * ACPI 6.5 §5.2.9.1: prefer X_DSDT (64-bit) when the FADT is long enough
     * to contain it AND it is non-zero. Fall back to the 32-bit DSDT field.
     * Real UEFI machines with high-RAM ACPI tables (>4 GB) require X_DSDT.
     */
    uint64_t dsdt_addr = 0;
    if (fadt->header.length >= offsetof(acpi_fadt_t, x_dsdt) + sizeof(uint64_t)
        && fadt->x_dsdt != 0) {
        dsdt_addr = fadt->x_dsdt;
    } else if (fadt->dsdt != 0) {
        dsdt_addr = (uint64_t)fadt->dsdt;
    }

    if (dsdt_addr == 0) {
        debug_printf("[ACPI] FADT exposes no DSDT address\n");
        return ACPI_ERR_DSDT_NOT_FOUND;
    }

    acpi_sdt_header_t* dsdt = map_and_validate_table((uintptr_t)dsdt_addr);
    if (!dsdt) {
        debug_printf("[ACPI] DSDT at 0x%lx invalid\n",
                     (unsigned long)dsdt_addr);
        return ACPI_ERR_DSDT_NOT_FOUND;
    }
    if (memcmp(dsdt->signature, "DSDT", 4) != 0) {
        debug_printf("[ACPI] Table at FADT.DSDT has signature %.4s, not DSDT\n",
                     dsdt->signature);
        return ACPI_ERR_DSDT_NOT_FOUND;
    }
    debug_printf("[ACPI] DSDT at 0x%lx length %u\n",
                 (unsigned long)dsdt_addr, dsdt->length);

    /* Search _S5 in DSDT first; if absent, walk every SSDT.
     *
     * ACPI 6.5 §5.2.11.2: SSDTs supplement the namespace and may carry
     * objects (including _S5) that the DSDT lacks. Real boards routinely
     * publish _S5 from an OEM SSDT. */
    s5_ctx_t s5 = { .found = false };
    s5_in_table_cb(dsdt, &s5);
    if (!s5.found) {
        debug_printf("[ACPI] _S5 not in DSDT; scanning SSDTs\n");
        acpi_for_each_table("SSDT", s5_in_table_cb, &s5);
    }

    if (!s5.found) {
        /* ACPI 1.0 default for SLP_TYPa/b when no _S5 is available.
         * This is a last-resort guess; soft-off may not work on the
         * board but at least PM1 won't be programmed with garbage. */
        debug_printf("[ACPI] _S5 not found anywhere; using SLP_TYP=0x05\n");
        g_acpi.slp_typa = 0x05;
        g_acpi.slp_typb = 0x05;
        g_acpi.s5_found = false;
    }

    /* Optional tables — absence is not fatal, only signalled by the
     * `present` flag in each accessor. Order is deliberate:
     *   HPET / MCFG          — used by Timer + PCI subsystems
     *   SRAT / SLIT          — NUMA topology + distance
     *   DMAR / IVRS          — IOMMU detection (Intel / AMD)
     *   APEI (HEST/BERT/ERST) — RAS error reporting
     */
    acpi_parse_hpet();
    acpi_parse_mcfg();
    acpi_parse_srat();
    acpi_parse_slit();
    acpi_parse_dmar();
    acpi_parse_ivrs();
    acpi_parse_apei();

    /* Phase J — once DMAR's DRHD entries are known, peek inside each
     * register block to log version + features. Read-only, never enables
     * translation; that belongs to the future IOMMU driver. */
    acpi_dmar_probe_registers();

    return ACPI_OK;
}
