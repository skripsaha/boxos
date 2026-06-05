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
 * or POCKET_FLAG_MANIFEST (single-shot Manifest dispatch). In Manifest mode
 * the envelope carries pointers to the raw Manifest byte stream and the
 * Crate[] array, both living in the sender cabin's user heap.
 *
 * Layout is 64 bytes — half the legacy 128-byte slot stride. The shrink
 * doubles PocketRing capacity within the same 1 MiB slot reservation
 * (POCKET_RING_SLOT_MAX: 8192 → 16384) and halves the cacheline footprint
 * of the hot KPocketPeek / pocket_ring_push path.
 *
 * The trailing _pad[24] is reserved for ABI growth (priority hints, deadline
 * stamps, sender-credential bits) so future expansion does not renegotiate
 * the slot stride again.
 */

typedef struct __packed {
    uint32_t pid;             /* kernel overwrites from process_t (security)  */
    uint32_t target_pid;      /* 0 = self, != 0 = IPC route                   */
    uint32_t error_code;      /* deck handlers write errors here              */
    uint8_t  flags;           /* POCKET_FLAG_YIELD | POCKET_FLAG_MANIFEST     */
    uint8_t  _reserved[3];
    uint32_t manifest_size;   /* bytes at manifest_addr                       */
    uint16_t crate_count;     /* number of entries in Crate[]                 */
    uint16_t pier_id;         /* urgency lane                                 */
    uint64_t manifest_addr;   /* user vaddr of raw Manifest                   */
    uint64_t crates_addr;     /* user vaddr of Crate[]                        */
    uint8_t  _pad[24];        /* reserved for ABI growth (pad to 64 bytes)    */
} Pocket;

_Static_assert(sizeof(Pocket) == 64, "Pocket must be 64 bytes for PocketRing packing");

#define POCKET_FLAG_YIELD            0x80
#define POCKET_FLAG_MANIFEST         0x40
/*
 * POCKET_FLAG_MANIFEST_HANDLE — handle-mode submit (set TOGETHER with
 * POCKET_FLAG_MANIFEST). manifest_addr carries a 64-bit ManifestHandle
 * returned by SYSTEM_OP_MANIFEST_COMPILE, not a user vaddr; manifest_size
 * is ignored. The kernel resolves the handle, verifies the calling cabin
 * owns it, and calls ManifestExecute on the cached CompiledManifest —
 * skipping the full validate + per-op OpRegistryLookup pass.
 */
#define POCKET_FLAG_MANIFEST_HANDLE  0x20

static inline uint64_t PocketManifestAddr(const Pocket *p)
{
    return p ? p->manifest_addr : 0;
}

static inline uint32_t PocketManifestSize(const Pocket *p)
{
    return p ? p->manifest_size : 0;
}

static inline uint64_t PocketCratesAddr(const Pocket *p)
{
    return p ? p->crates_addr : 0;
}

static inline uint16_t PocketCrateCount(const Pocket *p)
{
    return p ? p->crate_count : 0;
}

static inline uint16_t PocketPierId(const Pocket *p)
{
    return p ? p->pier_id : 0;
}

static inline void pocket_init(Pocket *p)
{
    if (!p) return;
    memset(p, 0, sizeof(Pocket));
}

#endif /* POCKET_H */
