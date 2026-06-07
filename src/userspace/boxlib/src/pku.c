#include "box/pku.h"
#include "box/core/manifest.h"
#include "box/string.h"
#include "box/error.h"
#include "boxos_decks.h"

/* Mirror opcode from src/kernel/core/decks/system/system_deck.h */
#define SYSTEM_OP_MEMTAG_APPLY_PKEY  0xAA

/* ─── PKU detection via CPUID.07H.0:ECX[3] ───────────────────────── */
/*
 * Linux/glibc convention is to wrap PKU support detection through the
 * kernel; BoxOS doesn't have a userspace cpuid syscall yet, so we
 * inline the CPUID query. The instruction is unprivileged. If PKU isn't
 * supported, RDPKRU/WRPKRU would #UD — we gate to avoid that.
 */
static int g_pku_detected = -1;   /* -1=unknown, 0=no, 1=yes */

static void cpuid_raw(uint32_t leaf, uint32_t subleaf,
                       uint32_t *out_eax, uint32_t *out_ebx,
                       uint32_t *out_ecx, uint32_t *out_edx) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid"
                     : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(leaf), "c"(subleaf));
    if (out_eax) *out_eax = a;
    if (out_ebx) *out_ebx = b;
    if (out_ecx) *out_ecx = c;
    if (out_edx) *out_edx = d;
}

static int pku_supported(void) {
    if (g_pku_detected >= 0) return g_pku_detected;
    uint32_t max_leaf;
    cpuid_raw(0, 0, &max_leaf, 0, 0, 0);
    if (max_leaf < 7) { g_pku_detected = 0; return 0; }
    uint32_t a = 0, b = 0, c = 0, d = 0;
    cpuid_raw(7, 0, &a, &b, &c, &d);
    g_pku_detected = ((c >> 3) & 1u) ? 1 : 0;
    return g_pku_detected;
}

/* ─── RDPKRU / WRPKRU ─────────────────────────────────────────────── */

uint32_t pku_read_pkru(void) {
    if (!pku_supported()) return 0;
    uint32_t eax;
    uint32_t edx_dummy;
    /* RDPKRU = 0F 01 EE — ECX must be 0. EDX cleared on exit. */
    __asm__ volatile(".byte 0x0F, 0x01, 0xEE"
                     : "=a"(eax), "=d"(edx_dummy)
                     : "c"(0));
    return eax;
}

void pku_write_pkru(uint32_t value) {
    if (!pku_supported()) return;
    /* WRPKRU = 0F 01 EF — EAX=value, ECX=0, EDX=0 (else #GP). */
    __asm__ volatile(".byte 0x0F, 0x01, 0xEF"
                     :
                     : "a"(value), "c"(0), "d"(0)
                     : "memory");
}

/* ─── Per-key rights ──────────────────────────────────────────────── */

int pku_set_rights(uint8_t pkey, int ad, int wd) {
    if (pkey >= PKU_MAX_KEYS) return -ERR_INVALID_ARGS;
    if (!pku_supported()) return -ERR_NOT_IMPLEMENTED;
    uint32_t pkru = pku_read_pkru();
    uint32_t shift = (uint32_t)pkey * 2u;
    uint32_t mask  = 0x3u << shift;
    uint32_t bits  = (((ad ? 1u : 0u)) | ((wd ? 2u : 0u))) << shift;
    pkru = (pkru & ~mask) | bits;
    pku_write_pkru(pkru);
    return 0;
}

int pku_get_rights(uint8_t pkey, int *out_ad, int *out_wd) {
    if (pkey >= PKU_MAX_KEYS) return -ERR_INVALID_ARGS;
    if (!pku_supported()) return -ERR_NOT_IMPLEMENTED;
    uint32_t pkru = pku_read_pkru();
    uint32_t shift = (uint32_t)pkey * 2u;
    if (out_ad) *out_ad = (int)((pkru >> shift) & 1u);
    if (out_wd) *out_wd = (int)((pkru >> (shift + 1u)) & 1u);
    return 0;
}

/* ─── Region stamping via MemTag syscall ──────────────────────────── */

int pku_apply_region(uint32_t region_id, uint8_t pkey) {
    if (pkey >= PKU_MAX_KEYS) return -ERR_INVALID_ARGS;
    uint8_t params[sizeof(uint32_t) + sizeof(uint8_t)];
    memcpy(params, &region_id, sizeof(uint32_t));
    params[sizeof(uint32_t)] = pkey;
    return MfCall1(DECK_SYSTEM, SYSTEM_OP_MEMTAG_APPLY_PKEY,
                   params, sizeof(params),
                   0, 0,
                   0, 0, 0,
                   30000, 0);
}
