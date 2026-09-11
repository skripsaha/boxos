#include "acpi_internal.h"
#include "klib.h"
#include "vmm.h"
#include "pmtimer.h"

bool acpi_validate_table(const acpi_sdt_header_t* header) {
    if (!header) return false;

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
        return NULL;
    }

    return full;
}

static acpi_rsdt_t* g_rsdt = NULL;
static acpi_xsdt_t* g_xsdt = NULL;
static uint32_t g_rsdt_count = 0;
static uint32_t g_xsdt_count = 0;

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

typedef struct {
    const char* signature;
    acpi_table_iter_cb_t cb;
    void* user;
    acpi_sdt_header_t* first_hit;
} iter_ctx_t;

static bool find_first_cb(acpi_sdt_header_t* tbl, void* user) {
    iter_ctx_t* ctx = (iter_ctx_t*)user;
    ctx->first_hit = tbl;
    return false;
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
    if (g_xsdt) {
        iterate_xsdt(&ctx);
    } else {
        iterate_rsdt(&ctx);
    }
}

#define ACPI_FIND_CACHE_SIZE   32
static struct {
    char sig[4];
    acpi_sdt_header_t* hdr;
} g_find_cache[ACPI_FIND_CACHE_SIZE];
static uint8_t g_find_cache_used = 0;

acpi_sdt_header_t* acpi_find_table(const char* signature) {
    if (!signature) return NULL;
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

typedef struct {
    bool found;
} s5_ctx_t;

static bool s5_in_table_cb(acpi_sdt_header_t* tbl, void* user) {
    s5_ctx_t* ctx = (s5_ctx_t*)user;
    uint32_t hdr_size = (uint32_t)sizeof(acpi_sdt_header_t);
    if (tbl->length <= hdr_size) return true;

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

    acpi_fadt_t* fadt = (acpi_fadt_t*)acpi_find_table("FACP");
    if (!fadt) {
        debug_printf("[ACPI] FADT not found\n");
        return ACPI_ERR_FADT_NOT_FOUND;
    }

    g_acpi.fadt = fadt;
    g_acpi.pm1a_cnt_blk = fadt->pm1a_control_block;
    g_acpi.pm1b_cnt_blk = fadt->pm1b_control_block;

    pmtimer_adopt_from_fadt(fadt);

    debug_printf("[ACPI] FADT rev=%u len=%u PM1a_CNT=0x%x PM1b_CNT=0x%x\n",
                 fadt->header.revision, fadt->header.length,
                 g_acpi.pm1a_cnt_blk, g_acpi.pm1b_cnt_blk);

    if (g_acpi.pm1a_cnt_blk == 0 &&
        (fadt->header.length < offsetof(acpi_fadt_t, x_pm1a_control_block) +
                                   sizeof(acpi_gas_t) ||
         fadt->x_pm1a_control_block.address == 0)) {
        debug_printf("[ACPI] WARNING: neither legacy PM1a_CNT nor X_PM1a_CNT available\n");
    }

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

    s5_ctx_t s5 = { .found = false };
    s5_in_table_cb(dsdt, &s5);
    if (!s5.found) {
        debug_printf("[ACPI] _S5 not in DSDT; scanning SSDTs\n");
        acpi_for_each_table("SSDT", s5_in_table_cb, &s5);
    }

    if (!s5.found) {
        debug_printf("[ACPI] _S5 not found anywhere; using SLP_TYP=0x05\n");
        g_acpi.slp_typa = 0x05;
        g_acpi.slp_typb = 0x05;
        g_acpi.s5_found = false;
    }

    acpi_parse_hpet();
    acpi_parse_mcfg();
    acpi_parse_srat();
    acpi_parse_slit();
    acpi_parse_dmar();
    acpi_parse_ivrs();
    acpi_parse_apei();

    acpi_dmar_probe_registers();

    return ACPI_OK;
}