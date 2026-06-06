/*
 * MemTag — Tag Registry
 *
 * Mirror of TagFS tag_registry (src/kernel/tagfs/tag_registry/) adapted for
 * in-RAM memory tagging. Same `key:value` interning model, same hash-bucket
 * design, same by_id flat array, same per-key wildcard buckets. No persistence
 * (RAM tags are ephemeral by nature). Adds:
 *   - 64-bit generation counter (bumped on every mutation; drives query cache
 *     invalidation in tag_bitmap and PTE-shadow invalidation in Phase 2)
 *   - ref_count per entry (region count holding the tag; informs GC + dump)
 *   - kernel-reserved flag (boot-time zone tags forbidden from user reuse)
 *
 * Tag form: "key:value" or "key" (key-only). Split on the FIRST colon to
 * match TagFS semantics — value "fmt:wav" under key "audio" is valid.
 */

#ifndef MEMTAG_TAG_REGISTRY_H
#define MEMTAG_TAG_REGISTRY_H

#include "ktypes.h"
#include "klib.h"
#include "error.h"

#define MEMTAG_INVALID_TAG_ID      0xFFFF
#define MEMTAG_MAX_TAG_ID          0xFFFE
#define MEMTAG_REG_BUCKETS         1024     /* (key,value) hash buckets       */
#define MEMTAG_KEY_BUCKETS         256      /* key-only hash for wildcards    */
#define MEMTAG_REG_INITIAL_BY_ID   64       /* initial by_id flat array slots */

#define MEMTAG_FLAG_HAS_VALUE         0x01
#define MEMTAG_FLAG_KERNEL_RESERVED   0x02  /* set by boot; user cannot reuse */
#define MEMTAG_FLAG_WILDCARD_ROOT     0x04  /* future: catch-all key tag      */
#define MEMTAG_FLAG_GUARD             0x08  /* capability — enforced access  */

typedef struct MemTagRegistryEntry {
    char       *key;
    char       *value;          /* NULL = key-only */
    uint16_t    tag_id;
    uint8_t     flags;
    uint32_t    region_count;   /* regions holding this tag — for GC + dump */
} MemTagRegistryEntry;

typedef struct MemTagRegistryNode {
    uint16_t                       tag_id;
    struct MemTagRegistryNode     *next;
} MemTagRegistryNode;

typedef struct MemTagKeyGroup {
    char                       *key;
    uint16_t                   *tag_ids;
    uint32_t                    count;
    uint32_t                    capacity;
    struct MemTagKeyGroup      *next;
} MemTagKeyGroup;

typedef struct MemTagRegistry {
    MemTagRegistryNode    **buckets;
    uint32_t                bucket_count;

    MemTagRegistryEntry   **by_id;
    uint32_t                by_id_capacity;

    MemTagKeyGroup        **key_buckets;
    uint32_t                key_bucket_count;

    uint32_t                total_tags;
    uint16_t                next_id;

    /* Bumped on every intern. Phase 2 hooks: tag_bitmap query cache
     * invalidation, PTE-shadow refresh, Touch publish ordering. */
    volatile uint64_t       generation;

    spinlock_t              lock;
} MemTagRegistry;

/* ─── Lifecycle ─────────────────────────────────────────────────────────── */

error_t       MemTagRegistryInit(MemTagRegistry *reg);
void          MemTagRegistryShutdown(MemTagRegistry *reg);

/* ─── Interning + Lookup ───────────────────────────────────────────────── */

/* Intern "key:value". value=NULL → key-only. Idempotent. Returns assigned
 * tag_id or MEMTAG_INVALID_TAG_ID on failure (registry full / OOM). */
uint16_t      MemTagRegistryIntern(MemTagRegistry *reg,
                                    const char *key, const char *value);

/* Convenience: parse "key:value" string (split on FIRST colon). Caller
 * passes a single string; we own the split internally. */
uint16_t      MemTagRegistryInternStr(MemTagRegistry *reg,
                                       const char *key_colon_value);

/* Read-only lookup. Returns INVALID if not interned. */
uint16_t      MemTagRegistryLookup(MemTagRegistry *reg,
                                    const char *key, const char *value);
uint16_t      MemTagRegistryLookupStr(MemTagRegistry *reg,
                                       const char *key_colon_value);

/* ─── Accessors ────────────────────────────────────────────────────────── */

const char  *MemTagRegistryKey(MemTagRegistry *reg, uint16_t tag_id);
const char  *MemTagRegistryValue(MemTagRegistry *reg, uint16_t tag_id);

/* Returns the group of all tag_ids sharing `key` (wildcard helper). */
MemTagKeyGroup *MemTagRegistryKeyGroup(MemTagRegistry *reg, const char *key);

uint64_t      MemTagRegistryGeneration(MemTagRegistry *reg);
uint32_t      MemTagRegistryTotalTags(MemTagRegistry *reg);

/* ─── Ref-counting (region_count per tag) ──────────────────────────────── */

void          MemTagRegistryRefInc(MemTagRegistry *reg, uint16_t tag_id);
void          MemTagRegistryRefDec(MemTagRegistry *reg, uint16_t tag_id);
uint32_t      MemTagRegistryRefCount(MemTagRegistry *reg, uint16_t tag_id);

/* ─── Kernel-reserved namespace ────────────────────────────────────────── */

/* Mark a tag as kernel-reserved (set at boot for zone:*, purpose:*, etc.).
 * Subsequent user-side intern attempts under reserved key prefixes will
 * fail at the policy layer (enforced in memtag.c, not here). */
void          MemTagRegistryMarkReserved(MemTagRegistry *reg, uint16_t tag_id);
bool          MemTagRegistryIsReserved(MemTagRegistry *reg, uint16_t tag_id);

/* Guard flag mutation — Phase 2A capability infrastructure. */
void          MemTagRegistryMarkGuard(MemTagRegistry *reg, uint16_t tag_id);
void          MemTagRegistryClearGuard(MemTagRegistry *reg, uint16_t tag_id);
bool          MemTagRegistryIsGuard(MemTagRegistry *reg, uint16_t tag_id);

/* ─── Diagnostics ─────────────────────────────────────────────────────── */

void          MemTagRegistryDump(MemTagRegistry *reg);

#endif /* MEMTAG_TAG_REGISTRY_H */
