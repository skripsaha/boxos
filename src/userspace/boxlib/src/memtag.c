#include "box/timeouts.h"
#include "box/memtag.h"
#include "box/core/manifest.h"
#include "box/memory.h"   /* malloc/free — mem_query_all owns its answer */
#include "box/string.h"
#include "box/error.h"
#include "boxos_decks.h"  /* SYSTEM_OP_MEMTAG_* opcodes — single source */

#define MEMTAG_QUERY_SECTION_SEP 0x1F

/* Build "req\0req\0\x1Fany\0\x1Fexc\0" spec from three NULL-term arrays. */
static int build_query_spec(const char *const *req,
                             const char *const *any,
                             const char *const *exc,
                             char *out, uint32_t out_size,
                             uint32_t *out_len)
{
    uint32_t pos = 0;

    const char *const *sections[3] = { req, any, exc };
    for (int s = 0; s < 3; s++) {
        if (s > 0) {
            if (pos + 1 > out_size) return -ERR_INVALID_ARGS;
            out[pos++] = MEMTAG_QUERY_SECTION_SEP;
        }
        if (!sections[s]) continue;
        for (uint32_t i = 0; sections[s][i] != 0; i++) {
            uint32_t tlen = (uint32_t)strlen(sections[s][i]);
            if (pos + tlen + 1 > out_size) return -ERR_INVALID_ARGS;
            memcpy(out + pos, sections[s][i], tlen);
            pos += tlen;
            out[pos++] = '\0';
        }
    }
    *out_len = pos;
    return 0;
}

/*
 * The MEMTAG_QUERY answer is [u32 total][u32 ids[]], and the two halves
 * answer different questions:
 *
 *   total     — how many regions matched.
 *   delivered — how many ids the kernel actually wrote, which is
 *               (bytes_returned - header) / 4, NOT the header field.
 *
 * Reading the count from the header and reading the ids from the byte
 * count is deliberate. The header is a claim about the world; the byte
 * count is a fact about this buffer. Trusting the header for the array
 * length is exactly how a clipped answer turns into a read past the ids
 * that arrived, and trusting the byte count for the total is how "there
 * are 4000" silently becomes "there are 64".
 */
#define MEM_QUERY_HEADER  4u

/* Shared body: runs one QUERY into `raw` and splits the answer. Returns
 * the kernel rc; on OK, *out_delivered / *out_total are filled. */
static int mem_query_call(const char *spec, uint32_t spec_len,
                           uint8_t *raw, uint32_t raw_cap,
                           uint32_t *out_delivered, uint32_t *out_total)
{
    uint32_t got = 0;
    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_MEMTAG_QUERY,
                     0, 0,
                     spec, spec_len,
                     raw, raw_cap, &got,
                     BOX_ANSWER_GUARANTEED, 0);
    if (rc != 0) return rc;
    if (got < MEM_QUERY_HEADER) return -ERR_INTERNAL;

    uint32_t total = 0;
    memcpy(&total, raw, sizeof(uint32_t));
    uint32_t delivered = (got - MEM_QUERY_HEADER) / sizeof(uint32_t);
    /* A header below the id count would mean the kernel sent ids it did
     * not count; take the larger so the caller is never told there are
     * fewer than it holds. */
    if (total < delivered) total = delivered;

    *out_delivered = delivered;
    *out_total     = total;
    return 0;
}

int mem_query_ex(const char *const *required,
                 const char *const *any,
                 const char *const *excluded,
                 uint32_t *out, uint32_t max_results,
                 uint32_t *out_total)
{
    if (out_total) *out_total = 0;
    if (!out || max_results == 0) return -ERR_INVALID_ARGS;

    char     spec[2048];
    uint32_t spec_len = 0;
    int rc = build_query_spec(required, any, excluded,
                              spec, sizeof(spec), &spec_len);
    if (rc != 0) return box_fail(rc);

    uint32_t out_cap = MEM_QUERY_HEADER + max_results * sizeof(uint32_t);
    uint8_t  raw[MEM_QUERY_HEADER + MEMTAG_MAX_QUERY_RESULTS * sizeof(uint32_t)];
    if (out_cap > sizeof(raw)) out_cap = sizeof(raw);

    uint32_t delivered = 0, total = 0;
    rc = mem_query_call(spec, spec_len, raw, out_cap, &delivered, &total);
    if (rc != 0) return box_fail(rc);

    if (delivered > max_results) delivered = max_results;
    if (delivered > 0)
        memcpy(out, raw + MEM_QUERY_HEADER, delivered * sizeof(uint32_t));
    if (out_total) *out_total = total;
    return (int)delivered;
}

int mem_query(const char *const *required,
              const char *const *any,
              const char *const *excluded,
              uint32_t *out, uint32_t max_results)
{
    return mem_query_ex(required, any, excluded, out, max_results, 0);
}

/* First ask and the ceiling the walk keeps. The first ask holds
 * MEMTAG_MAX_QUERY_RESULTS ids — the widest single answer the kernel's
 * QUERY op can return today — so the common case is one syscall and the
 * doubling below only engages if that op ever learns to say more. The
 * ceiling is four times that, which is where a caller wanting the whole
 * RAM region set should be paging by tag instead of asking for it flat. */
#define MEM_QUERY_FIRST_IDS  MEMTAG_MAX_QUERY_RESULTS
#define MEM_QUERY_MAX_IDS    (MEMTAG_MAX_QUERY_RESULTS * 4u)

int mem_query_all(const char *const *required,
                  const char *const *any,
                  const char *const *excluded,
                  uint32_t **out_ids, uint32_t *out_total)
{
    if (!out_ids) return -ERR_INVALID_ARGS;
    *out_ids = NULL;
    if (out_total) *out_total = 0;

    char     spec[2048];
    uint32_t spec_len = 0;
    int rc = build_query_spec(required, any, excluded,
                              spec, sizeof(spec), &spec_len);
    if (rc != 0) return box_fail(rc);

    uint32_t cap = MEM_QUERY_FIRST_IDS;
    uint8_t *raw = NULL;
    uint32_t delivered = 0, total = 0;

    for (;;) {
        uint32_t bytes = MEM_QUERY_HEADER + cap * sizeof(uint32_t);
        uint8_t *fresh = (uint8_t *)malloc(bytes);
        if (!fresh) { free(raw); return -ERR_NO_MEMORY; }
        free(raw);
        raw = fresh;

        rc = mem_query_call(spec, spec_len, raw, bytes, &delivered, &total);
        if (rc != 0) { free(raw); return box_fail(rc); }

        /* Everything that matched is in hand, or the ask is at the
         * ceiling and what came back is what there was room for — said
         * as a count either way, never as a silent truncation. */
        if (delivered >= total || cap >= MEM_QUERY_MAX_IDS) break;
        cap = cap * 2u > MEM_QUERY_MAX_IDS ? MEM_QUERY_MAX_IDS : cap * 2u;
    }

    if (out_total) *out_total = total;
    if (delivered == 0) { free(raw); return 0; }

    uint32_t *ids = (uint32_t *)malloc(delivered * sizeof(uint32_t));
    if (!ids) { free(raw); return -ERR_NO_MEMORY; }
    memcpy(ids, raw + MEM_QUERY_HEADER, delivered * sizeof(uint32_t));
    free(raw);
    *out_ids = ids;
    return (int)delivered;
}

int mem_region_info(uint32_t region_id, mem_region_info_t *out)
{
    if (!out) return -ERR_INVALID_ARGS;
    uint8_t params[sizeof(uint32_t)];
    memcpy(params, &region_id, sizeof(uint32_t));
    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_MEMTAG_INFO,
                     params, sizeof(params),
                     0, 0,
                     out, sizeof(*out), 0,
                     BOX_ANSWER_GUARANTEED, 0);
    return box_fail(rc);
}

uint32_t mem_region_from_phys(uint64_t phys)
{
    uint8_t  params[sizeof(uint64_t)];
    uint32_t result = MEMTAG_INVALID_REGION_ID;
    memcpy(params, &phys, sizeof(uint64_t));
    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_MEMTAG_LOOKUP,
                     params, sizeof(params),
                     0, 0,
                     &result, sizeof(result), 0,
                     BOX_ANSWER_GUARANTEED, 0);
    if (rc != 0) return MEMTAG_INVALID_REGION_ID;
    return result;
}

uint32_t mem_region_from_virt(const void *virt)
{
    uint8_t  params[sizeof(uint64_t)];
    uint64_t v = (uint64_t)(uintptr_t)virt;
    uint32_t result = MEMTAG_INVALID_REGION_ID;
    memcpy(params, &v, sizeof(uint64_t));
    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_MEMTAG_LOOKUP_VIRT,
                     params, sizeof(params),
                     0, 0,
                     &result, sizeof(result), 0,
                     BOX_ANSWER_GUARANTEED, 0);
    if (rc != 0) return MEMTAG_INVALID_REGION_ID;
    return result;
}

error_t mem_region_from_phys_ex(uint64_t phys, uint32_t *out_region_id)
{
    if (!out_region_id) return ERR_INVALID_ARGUMENT;
    uint8_t  params[sizeof(uint64_t)];
    uint32_t result = MEMTAG_INVALID_REGION_ID;
    memcpy(params, &phys, sizeof(uint64_t));
    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_MEMTAG_LOOKUP,
                     params, sizeof(params),
                     0, 0,
                     &result, sizeof(result), 0,
                     BOX_ANSWER_GUARANTEED, 0);
    if (rc != 0) return box_errno_of(box_fail(rc));
    *out_region_id = result;
    return OK;
}

error_t mem_region_from_virt_ex(const void *virt, uint32_t *out_region_id)
{
    if (!out_region_id) return ERR_INVALID_ARGUMENT;
    uint8_t  params[sizeof(uint64_t)];
    uint64_t v = (uint64_t)(uintptr_t)virt;
    uint32_t result = MEMTAG_INVALID_REGION_ID;
    memcpy(params, &v, sizeof(uint64_t));
    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_MEMTAG_LOOKUP_VIRT,
                     params, sizeof(params),
                     0, 0,
                     &result, sizeof(result), 0,
                     BOX_ANSWER_GUARANTEED, 0);
    if (rc != 0) return box_errno_of(box_fail(rc));
    *out_region_id = result;
    return OK;
}

int mem_region_tags(uint32_t region_id,
                    char *out_buf, uint32_t out_buf_size,
                    uint32_t *out_count)
{
    if (!out_buf || !out_count || out_buf_size < sizeof(uint32_t))
        return -ERR_INVALID_ARGS;

    uint8_t params[sizeof(uint32_t)];
    memcpy(params, &region_id, sizeof(uint32_t));

    /* Out is [u32 count][char strs[]]. Read into out_buf with prefix.
     * Then memmove the strings to the front for caller convenience. */
    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_MEMTAG_TAGS,
                     params, sizeof(params),
                     0, 0,
                     out_buf, out_buf_size, 0,
                     BOX_ANSWER_GUARANTEED, 0);
    if (rc != 0) return box_fail(rc);
    memcpy(out_count, out_buf, sizeof(uint32_t));
    /* Shift the string region left over the count prefix. Source comes
     * AFTER destination so a forward byte loop is safe (no overlap
     * hazard). */
    uint32_t str_bytes = out_buf_size - sizeof(uint32_t);
    char *dst = out_buf;
    char *src = out_buf + sizeof(uint32_t);
    for (uint32_t i = 0; i < str_bytes; i++) dst[i] = src[i];
    return 0;
}

int mem_stats(mem_stats_t *out)
{
    if (!out) return -ERR_INVALID_ARGS;
    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_MEMTAG_STATS,
                     0, 0, 0, 0,
                     out, sizeof(*out), 0,
                     BOX_ANSWER_GUARANTEED, 0);
    return box_fail(rc);
}

/* ─── Phase 2A — capabilities ───────────────────────────────────────── */
/* SYSTEM_OP_MEMTAG_* (SET_GUARD..CHECK) come from boxos_decks.h above. */

int mem_set_guard(const char *tag_str, int on)
{
    if (!tag_str) return -ERR_INVALID_ARGS;
    uint8_t params[1];
    params[0] = on ? 1 : 0;
    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_MEMTAG_SET_GUARD,
                     params, sizeof(params),
                     tag_str, (uint32_t)(strlen(tag_str) + 1),
                     0, 0, 0,
                     BOX_ANSWER_GUARANTEED, 0);
    return box_fail(rc);
}

int mem_cabin_grant(uint32_t pid, const char *tag_str)
{
    if (!tag_str) return -ERR_INVALID_ARGS;
    uint8_t params[sizeof(uint32_t)];
    memcpy(params, &pid, sizeof(uint32_t));
    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_MEMTAG_GRANT,
                     params, sizeof(params),
                     tag_str, (uint32_t)(strlen(tag_str) + 1),
                     0, 0, 0,
                     BOX_ANSWER_GUARANTEED, 0);
    return box_fail(rc);
}

int mem_cabin_revoke(uint32_t pid, const char *tag_str)
{
    if (!tag_str) return -ERR_INVALID_ARGS;
    uint8_t params[sizeof(uint32_t)];
    memcpy(params, &pid, sizeof(uint32_t));
    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_MEMTAG_REVOKE,
                     params, sizeof(params),
                     tag_str, (uint32_t)(strlen(tag_str) + 1),
                     0, 0, 0,
                     BOX_ANSWER_GUARANTEED, 0);
    return box_fail(rc);
}

int mem_cabin_tags(uint32_t pid,
                   char *out_buf, uint32_t out_buf_size,
                   uint32_t *out_count)
{
    if (!out_buf || !out_count || out_buf_size < sizeof(uint32_t))
        return -ERR_INVALID_ARGS;
    uint8_t params[sizeof(uint32_t)];
    memcpy(params, &pid, sizeof(uint32_t));
    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_MEMTAG_CABIN_TAGS,
                     params, sizeof(params),
                     0, 0,
                     out_buf, out_buf_size, 0,
                     BOX_ANSWER_GUARANTEED, 0);
    if (rc != 0) return box_fail(rc);
    memcpy(out_count, out_buf, sizeof(uint32_t));
    uint32_t str_bytes = out_buf_size - sizeof(uint32_t);
    char *dst = out_buf;
    char *src = out_buf + sizeof(uint32_t);
    for (uint32_t i = 0; i < str_bytes; i++) dst[i] = src[i];
    return 0;
}

int mem_check_access(uint32_t pid, uint32_t region_id, mem_check_t *out)
{
    if (!out) return -ERR_INVALID_ARGS;
    uint8_t params[2 * sizeof(uint32_t)];
    memcpy(params,                  &pid,       sizeof(uint32_t));
    memcpy(params + sizeof(uint32_t), &region_id, sizeof(uint32_t));
    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_MEMTAG_CHECK,
                     params, sizeof(params),
                     0, 0,
                     out, sizeof(*out), 0,
                     BOX_ANSWER_GUARANTEED, 0);
    return box_fail(rc);
}
