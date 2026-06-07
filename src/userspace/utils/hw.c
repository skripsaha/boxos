/*
 * hw — real-HW per-process state utility.
 *
 *   hw                         — CPU feature summary (from cpu_caps page)
 *   hw cpu                     — full CPU feature table
 *   hw pku get [<pkey>]        — read PKRU (all keys or one)
 *   hw pku set <pkey> <ad> <wd>— write a single PKRU slot
 *   hw lam get                 — read this process's LAM mode
 *   hw lam set none|u48|u57    — set this process's LAM mode
 *
 * Region-tag operations (pku-region, dump-*) live in `memtag` — they
 * operate on a region, not on the calling process's CPU state.
 */

#include "box/print.h"
#include "box/ipc.h"
#include "box/string.h"
#include "box/cpu.h"
#include "box/pku.h"
#include "box/hw.h"
#include "box/system.h"

static int parse_uint(const char *s, uint32_t *out)
{
    if (!s || !*s) return -1;
    uint32_t v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (uint32_t)(*s - '0');
    }
    *out = v;
    return 0;
}

static const char *yn(bool b) { return b ? "yes" : "no"; }

static const char *lam_name(int mode)
{
    switch (mode) {
        case HW_LAM_NONE: return "none";
        case HW_LAM_U48:  return "u48";
        case HW_LAM_U57:  return "u57";
        default:          return "?";
    }
}

/* ─── CPU feature table ─────────────────────────────────────────────── */

static void do_cpu(void)
{
    printf("%colorCPU features%color (read from caps page, post-AP intersect):\n",
           COLOR_CYAN, COLOR_DEFAULT);
    printf("  invariant TSC : %s\n", yn(cpu_get_tsc_freq_khz() != 0));
    printf("  TSC freq      : %u kHz\n", (unsigned)cpu_get_tsc_freq_khz());
    printf("  WAITPKG       : %s\n", yn(cpu_has_waitpkg()));
    printf("  PKU           : %s\n", yn(cpu_has_pku()));
    printf("  PKS           : %s\n", yn(cpu_has_pks()));
    printf("  LAM           : %s\n", yn(cpu_has_lam()));
    printf("  CET (SS/IBT)  : %s\n", yn(cpu_has_cet()));
    printf("  TME / TME-MK  : %s\n", yn(cpu_has_tme()));
}

/* ─── PKRU ──────────────────────────────────────────────────────────── */

static void print_pkey_rights(uint8_t pkey)
{
    int ad = 0, wd = 0;
    int rc = pku_get_rights(pkey, &ad, &wd);
    if (rc != 0) {
        printf("  pku:%u → err %d\n", pkey, rc);
        return;
    }
    const char *desc;
    if      (ad)        desc = "no access (AD=1)";
    else if (wd)        desc = "read-only (WD=1)";
    else                desc = "read+write";
    printf("  pku:%u → %s\n", pkey, desc);
}

static void do_pku_get_all(void)
{
    if (!cpu_has_pku()) {
        println("PKU not supported on this CPU");
        return;
    }
    uint32_t pkru = pku_read_pkru();
    printf("%colorPKRU%color = 0x%x\n", COLOR_CYAN, COLOR_DEFAULT, pkru);
    for (uint8_t k = 0; k < 16; k++) print_pkey_rights(k);
}

static void do_pku_get_one(uint8_t pkey)
{
    if (!cpu_has_pku()) {
        println("PKU not supported on this CPU");
        return;
    }
    print_pkey_rights(pkey);
}

static void do_pku_set(uint8_t pkey, int ad, int wd)
{
    if (!cpu_has_pku()) {
        println("PKU not supported on this CPU");
        return;
    }
    int rc = pku_set_rights(pkey, ad, wd);
    if (rc == 0) {
        printf("pku:%u → ad=%d wd=%d\n", pkey, ad ? 1 : 0, wd ? 1 : 0);
    } else {
        printf("%colorpku set failed (rc=%d)%color\n",
               COLOR_RED, rc, COLOR_DEFAULT);
    }
}

/* ─── LAM ───────────────────────────────────────────────────────────── */

static void do_lam_get(void)
{
    if (!cpu_has_lam()) {
        println("LAM not supported on this CPU");
        return;
    }
    int mode = hw_lam_get();
    if (mode < 0) {
        printf("%colorlam_get failed (rc=%d)%color\n",
               COLOR_RED, mode, COLOR_DEFAULT);
        return;
    }
    printf("this process LAM mode: %s\n", lam_name(mode));
}

static void do_lam_set(const char *arg)
{
    if (!cpu_has_lam()) {
        println("LAM not supported on this CPU");
        return;
    }
    hw_lam_mode_t mode;
    if      (strcmp(arg, "none") == 0) mode = HW_LAM_NONE;
    else if (strcmp(arg, "u48")  == 0) mode = HW_LAM_U48;
    else if (strcmp(arg, "u57")  == 0) mode = HW_LAM_U57;
    else {
        printf("unknown mode \"%s\" (want none|u48|u57)\n", arg);
        return;
    }
    int rc = hw_lam_set(mode);
    if (rc == 0) {
        printf("LAM mode → %s (effective on next CR3 reload)\n", lam_name(mode));
    } else {
        printf("%colorlam_set failed (rc=%d)%color\n",
               COLOR_RED, rc, COLOR_DEFAULT);
    }
}

static void print_help(void)
{
    println("hw commands:");
    println("  (no args)               — CPU feature summary");
    println("  cpu                     — full CPU feature table");
    println("  pku get [<pkey>]        — read PKRU (all 16 or one slot)");
    println("  pku set <pkey> <ad> <wd>— write a single PKRU slot");
    println("  lam get                 — read this process's LAM mode");
    println("  lam set none|u48|u57    — set this process's LAM mode");
}

int main(void)
{
    int  argc;
    char argv[16][64];
    receive_args(&argc, argv, 16);

    if (argc < 2) { do_cpu(); exit(0); return 0; }

    const char *cmd = argv[1];

    if (strcmp(cmd, "cpu") == 0) {
        do_cpu();
    } else if (strcmp(cmd, "pku") == 0) {
        if (argc < 3) { print_help(); exit(1); return 1; }
        const char *sub = argv[2];
        if (strcmp(sub, "get") == 0) {
            if (argc >= 4) {
                uint32_t pkey;
                if (parse_uint(argv[3], &pkey) != 0 || pkey >= 16) {
                    println("pkey must be 0..15"); exit(1); return 1;
                }
                do_pku_get_one((uint8_t)pkey);
            } else {
                do_pku_get_all();
            }
        } else if (strcmp(sub, "set") == 0) {
            if (argc < 6) { println("usage: hw pku set <pkey> <ad> <wd>"); exit(1); return 1; }
            uint32_t pkey, ad, wd;
            if (parse_uint(argv[3], &pkey) != 0 || pkey >= 16) {
                println("pkey must be 0..15"); exit(1); return 1;
            }
            if (parse_uint(argv[4], &ad) != 0 || ad > 1) {
                println("ad must be 0 or 1"); exit(1); return 1;
            }
            if (parse_uint(argv[5], &wd) != 0 || wd > 1) {
                println("wd must be 0 or 1"); exit(1); return 1;
            }
            do_pku_set((uint8_t)pkey, (int)ad, (int)wd);
        } else {
            print_help(); exit(1); return 1;
        }
    } else if (strcmp(cmd, "lam") == 0) {
        if (argc < 3) { print_help(); exit(1); return 1; }
        const char *sub = argv[2];
        if (strcmp(sub, "get") == 0) {
            do_lam_get();
        } else if (strcmp(sub, "set") == 0) {
            if (argc < 4) { println("usage: hw lam set none|u48|u57"); exit(1); return 1; }
            do_lam_set(argv[3]);
        } else {
            print_help(); exit(1); return 1;
        }
    } else {
        print_help(); exit(1); return 1;
    }

    exit(0);
    return 0;
}
