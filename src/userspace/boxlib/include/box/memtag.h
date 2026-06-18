#ifndef BOX_MEMTAG_H
#define BOX_MEMTAG_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/types.h"
#include "box/error.h"

/*
 * MemTag — userspace introspection for tagged RAM regions.
 *
 * The kernel runs a TagFS-shaped registry over RAM areas: each
 * (base_phys, base_virt, pages) region carries a sorted set of
 * "key:value" tags. Userspace can:
 *   - QUERY for region_ids matching a tag set (AND/OR/EXCLUDED)
 *   - INFO a region_id back to its descriptor
 *   - LOOKUP a phys (or virt, in the caller's cabin) address to the region
 *   - TAGS list a region's tag strings
 *   - STATS read global counters
 *
 * Introspection (the calls above) is unprivileged. Capability mutation —
 * set_guard / cabin_grant / cabin_revoke (below) — requires the caller to
 * hold the TagFS "system" tag; apply_pkey (box/pku.h) stamps by region_id.
 * The default registry is permissive (no guard ⇒ every access allowed).
 *
 * All operations are O(1) cache-hit / O(N words) cache-miss on the
 * kernel side. Cache invalidates implicitly via generation counter.
 */

#define MEMTAG_INVALID_REGION_ID  0xFFFFFFFFu
#define MEMTAG_MAX_QUERY_RESULTS  512u

/* Snapshot mirror of kernel MemRegionSnapshot. */
typedef struct {
    uint64_t   base_phys;
    uint64_t   base_virt;
    uint64_t   pages;
    uint16_t   tag_count;
    uint16_t   flags;
    uint32_t   generation;
} mem_region_info_t;

/* Global stats — mirror MemTagStats. */
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
} mem_stats_t;

/* Query regions whose tag set satisfies (required ∧ any ∧ ¬excluded).
 * Each array NUL-terminated (last entry NULL); pass NULL to skip a
 * section. Writes up to `max_results` region_ids into `out`. Returns
 * count, or -error on failure. */
int      mem_query(const char *const *required,
                   const char *const *any,
                   const char *const *excluded,
                   uint32_t *out, uint32_t max_results);

/* Snapshot a region by id. Returns 0 on success, -error otherwise. */
int      mem_region_info(uint32_t region_id, mem_region_info_t *out);

/* Phys → region_id. Returns MEMTAG_INVALID_REGION_ID if no region
 * covers the address. */
uint32_t mem_region_from_phys(uint64_t phys);

/* Virt → region_id for an address in the CALLER's cabin (virt → phys via
 * the caller's page tables → covering region). The only userspace path from
 * an owned pointer to its region_id — pku stamping needs it. Returns
 * MEMTAG_INVALID_REGION_ID if the address maps to no tagged region. */
uint32_t mem_region_from_virt(const void *virt);

/* List tag strings on a region. `out_buf` filled with NUL-separated
 * "key:value" strings; *out_count receives entry count. Returns 0
 * on success or -error. */
int      mem_region_tags(uint32_t region_id,
                         char *out_buf, uint32_t out_buf_size,
                         uint32_t *out_count);

/* Read global MemTag stats snapshot. */
int      mem_stats(mem_stats_t *out);

/* ═══════════════════════════════════════════════════════════════════
 *  Phase 2A — Capability mutation surface
 *
 *  set_guard / grant / revoke require the caller to hold the TagFS
 *  "system" tag-bit. cabin_tags / check_access are unprivileged.
 *
 *  Default state is permissive — zero guards in registry → check_access
 *  returns true for every (pid, region). Call set_guard("x") to flip a
 *  tag into enforcement mode; from that point on regions bearing that
 *  tag deny access from cabins without the matching grant.
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t  allowed;
    uint8_t  pad[3];
    uint16_t missing_tag_id;
    uint16_t reserved;
} mem_check_t;

int  mem_set_guard(const char *tag_str, int on);
int  mem_cabin_grant(uint32_t pid, const char *tag_str);
int  mem_cabin_revoke(uint32_t pid, const char *tag_str);
int  mem_cabin_tags(uint32_t pid,
                     char *out_buf, uint32_t out_buf_size,
                     uint32_t *out_count);
int  mem_check_access(uint32_t pid, uint32_t region_id, mem_check_t *out);

#ifdef __cplusplus
}
#endif

#endif /* BOX_MEMTAG_H */
