#include "box/memtag.h"
#include "box/core/manifest.h"
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

int mem_query(const char *const *required,
              const char *const *any,
              const char *const *excluded,
              uint32_t *out, uint32_t max_results)
{
    if (!out || max_results == 0) return -ERR_INVALID_ARGS;

    char     spec[2048];
    uint32_t spec_len = 0;
    int rc = build_query_spec(required, any, excluded,
                              spec, sizeof(spec), &spec_len);
    if (rc != 0) return box_fail(rc);

    /* out format: [u32 count][u32 ids[]] */
    uint32_t out_cap = sizeof(uint32_t) + max_results * sizeof(uint32_t);
    uint8_t  raw[sizeof(uint32_t) + MEMTAG_MAX_QUERY_RESULTS * sizeof(uint32_t)];
    if (out_cap > sizeof(raw)) out_cap = sizeof(raw);

    rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_MEMTAG_QUERY,
                 0, 0,
                 spec, spec_len,
                 raw, out_cap, 0,
                 30000, 0);
    if (rc != 0) return box_fail(rc);

    uint32_t count = 0;
    memcpy(&count, raw, sizeof(uint32_t));
    if (count > max_results) count = max_results;
    if (count > 0)
        memcpy(out, raw + sizeof(uint32_t), count * sizeof(uint32_t));
    return (int)count;
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
                     30000, 0);
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
                     30000, 0);
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
                     30000, 0);
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
                     30000, 0);
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
                     30000, 0);
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
                     30000, 0);
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
                     30000, 0);
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
                     30000, 0);
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
                     30000, 0);
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
                     30000, 0);
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
                     30000, 0);
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
                     30000, 0);
    return box_fail(rc);
}
