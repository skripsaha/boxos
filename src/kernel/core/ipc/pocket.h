#ifndef POCKET_H
#define POCKET_H

#include "ktypes.h"
#include "klib.h"
#include "boxos_decks.h"
#include "error.h"

/*
 * Pocket — kernel-bound syscall envelope (Phase 12: Manifest-only).
 *
 * Every Pocket either carries POCKET_FLAG_YIELD (cooperative tick, no work)
 * or POCKET_FLAG_MANIFEST (single-shot Manifest dispatch). The reinterpreted
 * fields below describe the Manifest payload:
 *
 *   data_addr       -> manifest_addr   (user vaddr of raw Manifest)
 *   data_length     -> manifest_size   (Manifest size in bytes)
 *   route_tag[0..7] -> crates_addr     (user vaddr of Crate[])
 *   route_tag[8..9] -> crate_count
 *   route_tag[10..11] -> pier_id
 *
 * The struct stays at 128 bytes for slot stride compatibility — _pad reserves
 * the bytes that used to hold the legacy prefix-chain fields. Future cleanup
 * can shrink this to 64 bytes once the slot stride is renegotiated.
 */

typedef struct __packed {
    uint32_t pid;                /* kernel overwrites from process_t (security) */
    uint32_t target_pid;         /* 0 = self, != 0 = IPC route */
    uint32_t error_code;         /* deck handlers write errors here */
    uint8_t  flags;              /* POCKET_FLAG_YIELD | POCKET_FLAG_MANIFEST */
    uint8_t  _reserved[3];
    uint32_t data_length;        /* manifest size */
    uint64_t data_addr;          /* manifest user vaddr */
    char     route_tag[32];      /* manifest mode: crates_addr/count/pier_id */
    uint8_t  _pad[68];           /* pad to 128 bytes for PocketRing slot stride */
} Pocket;

_Static_assert(sizeof(Pocket) == 128, "Pocket must be 128 bytes for PocketRing packing");

#define POCKET_FLAG_YIELD     0x80
#define POCKET_FLAG_MANIFEST  0x40

static inline uint64_t PocketManifestAddr(const Pocket *p)
{
    return p ? p->data_addr : 0;
}

static inline uint32_t PocketManifestSize(const Pocket *p)
{
    return p ? p->data_length : 0;
}

static inline uint64_t PocketCratesAddr(const Pocket *p)
{
    if (!p) return 0;
    uint64_t v;
    memcpy(&v, p->route_tag, sizeof(v));
    return v;
}

static inline uint16_t PocketCrateCount(const Pocket *p)
{
    if (!p) return 0;
    uint16_t v;
    memcpy(&v, p->route_tag + 8, sizeof(v));
    return v;
}

static inline uint16_t PocketPierId(const Pocket *p)
{
    if (!p) return 0;
    uint16_t v;
    memcpy(&v, p->route_tag + 10, sizeof(v));
    return v;
}

static inline void pocket_init(Pocket *p)
{
    if (!p) return;
    memset(p, 0, sizeof(Pocket));
}

#endif /* POCKET_H */
