/*
 * MemTag — BoxOS memory-region tagging (TagFS-shaped, for RAM areas).
 *
 * Tag memory AREAS (contiguous physical or virtual ranges) with
 * `key:value` strings. Identical model to TagFS but in RAM:
 *   - tags interned to uint16 IDs
 *   - regions identified by uint32 region_ids
 *   - multi-tag per region via sorted tag_ids[]
 *   - inverted bitmap index for AND/OR/EXCLUDED set queries
 *   - 16-slot LRU query cache (generation-invalidated)
 *   - dense phys_page → region_id array (O(1) reverse lookup)
 *   - dynamic growth (start 1024 regions / 64 tag bitmaps; double on demand)
 *   - per-bucket spinlocks for AMP concurrency
 *
 * Boot-seeded reserved tag namespace (`zone:*`, `purpose:*`, ...). User-side
 * tags live outside the reserved prefixes. Touch publish on every lifecycle
 * event drives userspace subscribers and Phase 2 PTE-shadow hooks.
 *
 * Real-HW integration (Phase 2 — additive on top of this Phase 1 ABI):
 *   - VMM #PF tag-check via id_by_page → region.tag_ids → cabin active mask
 *   - PTE bits 52-58 as optional region_id cache (Intel SDM Vol 3A §4.5)
 *   - NUMA / PAT / MCE / IOMMU / PKU / PKS / LAM / UAI / TME-MK / CET tags
 *     applied automatically by the relevant driver subsystem
 */

#ifndef MEMTAG_H
#define MEMTAG_H

#include "ktypes.h"
#include "klib.h"
#include "error.h"

#include "tag_registry.h"
#include "region_registry.h"
#include "tag_bitmap.h"

/* ─── Init ───────────────────────────────────────────────────────────── */

error_t       MemTagInit(void);
bool          MemTagIsInitialized(void);

/* Called by main.c AFTER Touch / TagFS are up so subsequent
 * lifecycle events publish to subscribers. Before this is called,
 * MemTag publishes silently no-op (resolution would fail anyway). */
void          MemTagEnableTouchPublish(void);

/* ─── Tag interning ──────────────────────────────────────────────────── */

uint16_t      MemTagInternStr(const char *kv);
uint16_t      MemTagResolveStr(const char *kv);
uint16_t      MemTagIntern(const char *key, const char *value);
uint16_t      MemTagResolve(const char *key, const char *value);
const char   *MemTagKey(uint16_t tag_id);
const char   *MemTagValue(uint16_t tag_id);

/* ─── Region lifecycle ──────────────────────────────────────────────── */

/* Create a region for `pages` 4 KiB pages backed at base_phys (and
 * optionally mapped at base_virt in cabin ctx). flags = MEMTAG_REGION_FLAG_*.
 * Returns region_id or MEMTAG_INVALID_REGION_ID. */
uint32_t      MemRegionCreate(uintptr_t base_phys,
                               uintptr_t base_virt,
                               void *ctx,
                               size_t pages,
                               uint16_t flags);

/* Destroy a region. Removes it from tag bitmap + id_by_page + frees slot. */
void          MemRegionDestroy(uint32_t region_id);

/* Snapshot a region (lock-protected copy). */
error_t       MemRegionInfo(uint32_t region_id, MemRegionSnapshot *out);

bool          MemRegionIsActive(uint32_t region_id);

/* O(1) phys → region_id (returns INVALID if no region covers `phys`). */
uint32_t      MemRegionFromPhys(uintptr_t phys);

/* virt → region_id via vmm_virt_to_phys + MemRegionFromPhys. */
uint32_t      MemRegionFromVirt(void *ctx, uintptr_t virt);

/* ─── Tag apply/clear ──────────────────────────────────────────────── */

/* Apply (intern + add) a "key:value" tag to a region. */
error_t       MemTagApply(uint32_t region_id, const char *tag_str);
error_t       MemTagApplyMany(uint32_t region_id, const char *const *tag_strs);

/* Clear a tag from a region (idempotent if not present). */
error_t       MemTagClear(uint32_t region_id, const char *tag_str);

/* O(log K) "does region have tag?" via region.tag_ids binary search. */
bool          MemRegionHasTag(uint32_t region_id, uint16_t tag_id);
bool          MemRegionHasTagStr(uint32_t region_id, const char *tag_str);

/* Find-or-create a region covering [base_phys, base_phys + pages * 4KB)
 * and apply the tag. If a region already covers `base_phys` AND has the
 * exact same length, the tag is added to it; otherwise a new region is
 * created. Use to tag previously-untracked physical ranges (e.g. process
 * IPC ring pages, where PMM was called with no tag at alloc time). */
error_t       MemTagApplyByPhys(uintptr_t base_phys, size_t pages,
                                 const char *tag_str);

/* Mirror of ApplyByPhys: clear a tag from any region covering `base_phys`.
 * No-op if no region exists or no matching tag. */
error_t       MemTagClearByPhys(uintptr_t base_phys, const char *tag_str);

/* List region's tags into caller's buffer. Returns count. */
size_t        MemRegionListTags(uint32_t region_id, uint16_t *out, size_t max);

/* ─── Set-algebra queries (TagFS-shaped) ──────────────────────────── */

/* Result container. Stays consistent with old API for migration ease. */
typedef struct {
    uint32_t    region_ids[512];
    size_t      count;
} MemTagResult;

/* AND of all listed tags. NULL-terminated tag string array. */
MemTagResult  MemTagQueryAnd_(const char *const *tag_strs);

/* OR of all listed tags. */
MemTagResult  MemTagQueryOr_(const char *const *tag_strs);

/* Full algebra. Each array NULL-terminated; pass NULL to skip. */
MemTagResult  MemTagQueryMixed_(const char *const *required,
                                 const char *const *any,
                                 const char *const *excluded);

/* Variadic convenience macros — compound literal arrays.
 * Usage: MemTagAnd("zone:dma32", "purpose:kernel") */
#define MemTagAnd(...)  MemTagQueryAnd_((const char *const[]){__VA_ARGS__, NULL})
#define MemTagOr(...)   MemTagQueryOr_((const char *const[]){__VA_ARGS__, NULL})

/* ─── PMM integration (called from pmm_alloc with string tag) ────── */

/* Internal: allocate phys pages, optionally constrain to a zone tag prefix,
 * register a region with all the supplied tags. Used by the pmm_alloc
 * macro's string-tag dispatch path. Returns phys address or NULL. */
void         *MemTagPmmAlloc(size_t pages, const char *tag_str);
void         *MemTagPmmAllocZero(size_t pages, const char *tag_str);

/* Internal: pmm_free notification. Destroys any region covering the freed
 * range. Always safe to call; no-op if no region exists. */
void          MemTagPmmFreed(uintptr_t base_phys, size_t pages);

/* ═══════════════════════════════════════════════════════════════════
 *  Phase 2A — Enforcement infrastructure (capability tags)
 *
 *  Tags can be marked MEMTAG_FLAG_GUARD via MemTagSetGuard(). Guarded
 *  tags act as capability tokens: a region carrying a guard tag is
 *  accessible only to cabins whose `active_memtags` mask holds that
 *  tag_id. Non-guarded tags remain pure metadata (current behavior).
 *
 *  Default: zero guards in registry → zero enforcement → all access
 *  succeeds. Userspace opts into enforcement per-tag via MEMTAG_SET_GUARD
 *  Pocket op (gated on "system" tag-bit auth).
 *
 *  Phase 2A is SOFT-CHECK only (mem_check_access userspace op + new
 *  reservation that vmm_handle_page_fault will use in Phase 2B). No PTE
 *  bit manipulation yet — Phase 2B closes the loop with #PF interception.
 * ═══════════════════════════════════════════════════════════════════ */

/* Mark/clear a tag as a guard (capability token). Idempotent. */
error_t       MemTagSetGuard(const char *tag_str, bool guard);
bool          MemTagIsGuard(uint16_t tag_id);

/* Grant / revoke a guard capability to a specific process (by PID).
 * Both ends atomic — single bit set/clear on process_t.active_memtags.
 * Caller must have already authorized the operation; this is the
 * unconditional kernel-side primitive. */
error_t       MemCabinGrant(uint32_t pid, uint16_t tag_id);
error_t       MemCabinRevoke(uint32_t pid, uint16_t tag_id);
bool          MemCabinHolds(uint32_t pid, uint16_t tag_id);

/* List cabin's held memtag tag_ids into caller buffer. Returns count. */
size_t        MemCabinListTags(uint32_t pid, uint16_t *out, size_t max);

/* Check whether `pid` can access `region_id` per current guard policy:
 *   for every guard-flagged tag on the region, cabin must hold it.
 * Returns true on permit, false on deny. Both region not-found and
 * cabin not-found return false (fail-closed). */
bool          MemRegionAccessAllowed(uint32_t pid, uint32_t region_id);

/* Phase 2B hook: extracts the missing-guard tag_id (first one) for the
 * failed access. Used by vmm_handle_page_fault to publish a precise
 * memtag:fault:denied payload. Returns MEMTAG_INVALID_TAG_ID if access
 * is allowed or the region/pid is invalid. */
uint16_t      MemRegionFirstMissingGuard(uint32_t pid, uint32_t region_id);

/* Phase 2B — combined check + publish helper. Returns true on permit.
 * On deny: publishes `memtag:fault:denied` with payload {pid, va,
 * region_id, missing_tag_id}. va may be 0 for non-VA contexts
 * (bay_open / brook_open enforce by phys-only). Pass region_id directly
 * to avoid redundant lookups in caller. */
bool          MemTagEnforce(uint32_t pid, uintptr_t va, uint32_t region_id);

/* Convenience: lookup region from phys then enforce. */
bool          MemTagEnforcePhys(uint32_t pid, uintptr_t phys);

/* ═══════════════════════════════════════════════════════════════════
 *  Phase 2C — Continuous PTE enforcement
 *
 *  Phase 2B enforces at map-time (Bay/Brook open) and #PF-time. Phase 2C
 *  closes the loop for the RUNTIME case: a cabin already holds a live
 *  mapping for a region, and either (a) its capability is revoked or
 *  (b) the tag flips from metadata to guard. Without continuous
 *  enforcement, the cabin's PTE is still PRESENT and the access never
 *  faults, defeating capability semantics.
 *
 *  We track per-region cabin mappings (MemRegionAttach chain on each
 *  MemRegion) populated by Bay/Brook map paths. On revoke / grant / guard
 *  change, we walk the affected attachments and CAS PTE.P=0 / restore
 *  PTE.P=1 + cross-core TLB shootdown.
 * ═══════════════════════════════════════════════════════════════════ */

/* Attach a cabin mapping record for `region_id` covering `pages` 4 KiB
 * units starting at `va_base` in `ctx` (vmm_context_t*). page_class
 * selects 4 KiB PTE vs 2 MiB PD-leaf semantics. `orig_flags` is the
 * VMM_PTE_FLAGS_MASK-bit pattern from the original vmm_map* call (e.g.
 * VMM_FLAGS_USER_RW | VMM_FLAG_NO_EXECUTE for typical Bay maps), saved
 * so the restore path on grant rebuilds the exact same PTE.
 * Called from Bay/Brook map paths AFTER vmm_map succeeded. */
error_t       MemRegionAttachCabin(uint32_t region_id, void *ctx,
                                    uintptr_t va_base, uint32_t pages,
                                    uint8_t page_class, uint64_t orig_flags);

/* Detach (release) a cabin's mapping record. Idempotent: returns OK
 * even if no matching attach exists. Called BEFORE vmm_unmap so the
 * record reflects the about-to-end mapping. */
error_t       MemRegionDetachCabin(uint32_t region_id, void *ctx,
                                    uintptr_t va_base);

/* Defensive scrub of every attach record matching `ctx`. O(R × A_avg) —
 * walks the whole region slot array under per-bucket locks. NOT called
 * from process_destroy (Bay/Brook cleanup is the contract); kept as a
 * direct API for debug tools and for test harnesses (Phase 14C) that
 * exercise the scrub without driving a full process teardown. */
size_t        MemTagDetachAllForCabin(void *ctx);

/* Phase 2C — enforce the result of MemCabinRevoke(pid, tag_id). Walks
 * every region carrying `tag_id` (if it is a guard) and for each cabin
 * attachment matching pid's cabin, clears PTE.P + shootdowns TLB.
 * Returns the number of attachments revoked.
 *
 * Called from MemCabinRevoke after the mask bit was cleared. Safe to
 * call when tag is not a guard (no-op). */
size_t        MemCabinEnforceRevokePost(uint32_t pid, uint16_t tag_id);

/* Phase 2C — enforce the result of MemCabinGrant(pid, tag_id). Walks
 * regions carrying `tag_id` and for cabin attachments now FULLY
 * satisfying all guards, restores PTE.P=1 + shootdown. Returns count
 * of attachments restored.
 *
 * Called from MemCabinGrant after the mask bit was set. Safe to call
 * when tag is not a guard (no-op). */
size_t        MemCabinEnforceGrantPost(uint32_t pid, uint16_t tag_id);

/* Phase 2C — full sweep when MemTagSetGuard flips a tag's guard state.
 * `guard_on=true`: walk all regions with this tag, for every attachment
 * whose cabin doesn't satisfy all guards → revoke PTEs.
 * `guard_on=false`: walk attachments and restore any that became
 * accessible (state was REVOKED with this tag the only blocker).
 * Returns count of attachments transitioned. */
size_t        MemTagSweepGuard(uint16_t tag_id, bool guard_on);

/* ═══════════════════════════════════════════════════════════════════
 *  Phase 2D — PTE bits 52-58 region-id fast cache + AP-verify (M1+M5)
 *
 *  Intel SDM Vol 3A §4.5 Tables 4-19/4-20/4-21 — for every paging mode
 *  (32-bit, PAE, IA-32e 4-level, IA-32e 5-level) PTE bits M..58 are
 *  "Ignored by the processor" (M = MAXPHYADDR; bits ≥ M and < 59 are
 *  always ignored). With MAXPHYADDR ≤ 52 (every Intel/AMD silicon
 *  shipped at the time of writing), bits 52-58 are unconditionally
 *  available for OS metadata.
 *
 *  Intel SDM Vol 3A §4.6.2 (Protection Keys): bits 62..59 form PKEY
 *  when CR4.PKE=1 — orthogonal to 52-58.
 *  Intel SDM Vol 3D §17 (CET): bit 60 selects supervisor shadow stack
 *  when CR4.CET=1 — also outside 52-58.
 *  AMD APM Vol 2 §5.4.1: bits 52-58 "Available to software" (AVL bits).
 *
 *  Encoding:  PTE bits 52-58 = region_id & 0x7F  (7 bits → 128 slots).
 *  Collision (region_id ≥ 128 wraps modulo 128) is resolved by verifying
 *  the candidate slot's [base_phys, base_phys + pages*4K) covers the
 *  resolved phys; on mismatch the fast-path falls back to the dense
 *  id_by_page reverse lookup (still O(1), just costs one extra cache
 *  line). Worst-case behaviour identical to Phase 1-2C.
 *
 *  M5 (boot probe) — MemTagVerifyPteMetadataBits is called from MemTagInit
 *  (BSP) and per_core_init_ap (every AP) to verify MAXPHYADDR ≤ 52 and
 *  log CR4.PKE / CR4.CET state. The function is informational on Intel/
 *  AMD x86_64 today; it is the safety net for future architectures that
 *  may repurpose those bits.
 * ═══════════════════════════════════════════════════════════════════ */

/* PTE-encoded region cache. 7-bit window, 128 fast-cache slots. */
#define MEMTAG_PTE_REGION_SHIFT     52
#define MEMTAG_PTE_REGION_BITS      7
#define MEMTAG_PTE_REGION_MAX       ((1u << MEMTAG_PTE_REGION_BITS) - 1u)
#define MEMTAG_PTE_REGION_MASK      (((uint64_t)MEMTAG_PTE_REGION_MAX) \
                                     << MEMTAG_PTE_REGION_SHIFT)

/* Extract encoded region_id (low 7 bits) from a PTE value, verify the
 * candidate slot actually covers `phys`. On miss fall back to dense
 * phys → id reverse lookup. Returns MEMTAG_INVALID_REGION_ID when no
 * region tracks this phys, or when MemTag not yet initialized. */
uint32_t      MemRegionFromPte(uint64_t pte_value, uintptr_t phys);

/* M5 boot probe. Verifies bits 52-58 are still "Ignored" on the calling
 * CPU. Logs detected MAXPHYADDR and CR4.PKE / CR4.CET state. Returns
 * false (and logs ABORT) if any condition would invalidate the encoding
 * assumption. Idempotent — safe to call from BSP + every AP. */
bool          MemTagVerifyPteMetadataBits(void);

/* ═══════════════════════════════════════════════════════════════════
 *  Phase 2E — PAT comprehensive + MTRR audit
 *
 *  Cache type tags (cache:wb / cache:wt / cache:uc / cache:uc- /
 *  cache:wc / cache:wp) follow Intel SDM Vol 3A §11.12 nomenclature.
 *  Phase 2E completes "every MemRegion carries a derived cache:* tag":
 *    - Boot-seeded USABLE-RAM zones get cache:wb (BoxOS PAT[0/4] = WB)
 *    - Boot-seeded MMIO/reserved E820 entries get cache:uc (vmm_map_mmio
 *      uses PAT index 3 = UC for them)
 *    - Boot-seeded kernel image gets cache:wb (kernel maps via WB)
 *    - vmm_map_mmio + vmm_map_framebuffer already applied cache:uc /
 *      cache:wc to their per-allocation regions (unchanged)
 *
 *  AP consistency:
 *    Intel SDM Vol 3A §11.12.4 mandates IA32_PAT (MSR 0x277) be identical
 *    across all logical processors in a coherent domain. Heterogeneous PAT
 *    would cause cache-type splits (e.g. P-core renders WC, E-core
 *    coalesces UC-) leading to torn pixels or stale DMA. The probe is
 *    called from per_core_init_ap after vmm_pat_init programs the AP's
 *    own MSR copy.
 *
 *  MTRR audit:
 *    Intel SDM Vol 3A §11.12.5 — effective memory type = MTRR ∩ PAT.
 *    BoxOS doesn't reprogram MTRRs (firmware sets them), but logs the
 *    layout once at boot so operators see when MTRR_DEF_TYPE != WB.
 * ═══════════════════════════════════════════════════════════════════ */

/* AP-side IA32_PAT consistency probe. Returns true on match with BSP
 * snapshot. Logs ABORT and returns false on mismatch. Idempotent. */
bool          MemTagVerifyPatMsr(void);

/* Boot-time MTRR layout audit. Reads IA32_MTRRCAP (0xFE), MTRR_DEF_TYPE
 * (0x2FF), and variable MTRRs (0x200..0x20F). Logs the layout once;
 * called from MemTagInit after Touch publish is up. Idempotent.
 * Non-fatal on missing MTRR support — no-op. */
void          MemTagDumpMtrrLayout(void);

/* ─── Statistics ───────────────────────────────────────────────────── */

typedef struct {
    uint32_t  tag_count;
    uint32_t  region_active;
    uint32_t  region_slot_count;
    uint32_t  region_slot_cap;
    uint64_t  registry_generation;
    uint64_t  region_generation;
    uint64_t  bitmap_generation;
    uint64_t  cache_hits;
    uint64_t  cache_misses;
} MemTagStats;

void          MemTagGetStats(MemTagStats *out);

/* ─── Diagnostics ──────────────────────────────────────────────────── */

void          MemTagDump(void);
void          MemTagStressTest(void);

/* ─── Direct subsystem access (for advanced callers) ──────────────── */

MemTagRegistry      *MemTagGetRegistry(void);
MemRegionRegistry   *MemTagGetRegionRegistry(void);
MemTagBitmapIndex   *MemTagGetBitmap(void);

#endif /* MEMTAG_H */
