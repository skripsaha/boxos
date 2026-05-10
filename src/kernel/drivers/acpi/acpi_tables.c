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
static acpi_sdt_header_t* map_and_validate_table(uintptr_t phys) {
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

acpi_sdt_header_t* acpi_find_table(const char* signature) {
    if (!signature) return NULL;
    iter_ctx_t ctx = { signature, find_first_cb, NULL, NULL };
    ctx.user = &ctx;   /* find_first_cb writes back into ctx->first_hit */
    if (g_xsdt) {
        iterate_xsdt(&ctx);
    } else {
        iterate_rsdt(&ctx);
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

acpi_error_t acpi_parse_tables(acpi_rsdp_t* rsdp) {
    if (!rsdp) return ACPI_ERR_INVALID_RSDP;

    acpi_error_t err = cache_root_tables(rsdp);
    if (err != ACPI_OK) return err;

    /* FADT lookup (signature "FACP"). */
    acpi_fadt_t* fadt = (acpi_fadt_t*)acpi_find_table("FACP");
    if (!fadt) {
        debug_printf("[ACPI] FADT not found\n");
        return ACPI_ERR_FADT_NOT_FOUND;
    }

    g_acpi.fadt = fadt;
    g_acpi.pm1a_cnt_blk = fadt->pm1a_control_block;
    g_acpi.pm1b_cnt_blk = fadt->pm1b_control_block;
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

    /* Optional tables: HPET (timer) and MCFG (PCIe ECAM). Absence is not
     * fatal — only signalled in `acpi_*_info_t.present`. */
    acpi_parse_hpet();
    acpi_parse_mcfg();

    return ACPI_OK;
}
