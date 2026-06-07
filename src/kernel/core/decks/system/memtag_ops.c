/*
 * MemTag — Manifest opcode handlers (RAM region tagging surface).
 *
 * Phase 1 surface is READ-ONLY: userspace observes regions and queries
 * by tag set but cannot apply/clear/register (mutation hooks land in
 * Phase 2 alongside the per-cabin security mask).
 *
 *   QUERY   in_crate = "req\0req\0\x1Fany\0\x1Fexcl\0" tag spec
 *           out_crate = [u32 count][u32 region_ids[]]
 *   INFO    params  = [u32 region_id]; out_crate = MemRegionSnapshot blob
 *   LOOKUP  params  = [u64 phys]; out_crate = [u32 region_id]
 *   TAGS    params  = [u32 region_id]; out_crate = [u32 count][char strs[]]
 *   STATS   out_crate = MemTagStats blob
 *
 * All input/output crates flow through vmm_user_buf_* helpers so they
 * survive multi-page user buffers without aliasing into kernel memory
 * (vmm_translate_user_addr clamps to a single physical page — using it
 * directly with multi-page outputs would write into adjacent physical
 * pages that don't match user's virtual layout).
 */

#include "system_deck.h"
#include "memtag.h"
#include "op_registry.h"
#include "boxos_manifest.h"
#include "boxos_crate.h"
#include "manifest_auth.h"
#include "process.h"
#include "vmm.h"
#include "klib.h"
#include "error.h"

/* ─── SYSTEM_OP_MEMTAG_QUERY ────────────────────────────────────────── */
#define MEMTAG_QUERY_SECTION_SEP 0x1F

static int SysMemTagQuery(const ManifestOp *op, Crate *crates,
                          uint16_t crate_count, const OpContext *ctx) {
    (void)crate_count;
    if (!ctx || !ctx->proc)                return ERR_INVALID_ARGUMENT;
    if (op->in_crate  == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    const Crate *in  = &crates[op->in_crate];
    Crate       *out = &crates[op->out_crate];
    if (in->size == 0 || in->size > 4096) return ERR_INVALID_ARGUMENT;
    if (out->capacity < sizeof(uint32_t)) return ERR_INVALID_ARGUMENT;

    /* Pull the spec into a kernel buffer — page-walk safe. */
    char *spec_k = (char *)vmm_user_buf_in(ctx->proc->cabin,
                                            (uintptr_t)in->addr, in->size);
    if (!spec_k) return ERR_INVALID_ADDRESS;

    /* Append a guard NUL so SplitColon-style walking never reads past. */
    char *spec = (char *)kmalloc(in->size + 1);
    if (!spec) { vmm_user_buf_free(spec_k); return ERR_NO_MEMORY; }
    memcpy(spec, spec_k, in->size);
    spec[in->size] = '\0';
    vmm_user_buf_free(spec_k);

    const char *req_buf[17] = {0}, *any_buf[17] = {0}, *exc_buf[17] = {0};
    size_t      req_n = 0, any_n = 0, exc_n = 0;
    int         section = 0;
    size_t      i = 0;
    while (i < in->size) {
        if (spec[i] == MEMTAG_QUERY_SECTION_SEP) {
            spec[i++] = '\0';
            section++;
            if (section > 2) break;
            continue;
        }
        if (spec[i] == '\0') { i++; continue; }
        const char *tag = &spec[i];
        while (i < in->size && spec[i] != '\0' &&
               spec[i] != MEMTAG_QUERY_SECTION_SEP) i++;
        spec[i] = '\0';
        i++;
        if      (section == 0 && req_n < 16) req_buf[req_n++] = tag;
        else if (section == 1 && any_n < 16) any_buf[any_n++] = tag;
        else if (section == 2 && exc_n < 16) exc_buf[exc_n++] = tag;
    }
    req_buf[req_n] = NULL;
    any_buf[any_n] = NULL;
    exc_buf[exc_n] = NULL;

    MemTagResult r = MemTagQueryMixed_(req_buf, any_buf, exc_buf);

    size_t need_bytes = sizeof(uint32_t) + r.count * sizeof(uint32_t);
    if (need_bytes > out->capacity) {
        size_t max_results = (out->capacity - sizeof(uint32_t)) / sizeof(uint32_t);
        if (r.count > max_results) r.count = max_results;
        need_bytes = sizeof(uint32_t) + r.count * sizeof(uint32_t);
    }

    uint8_t *kbuf = (uint8_t *)vmm_user_buf_alloc_out(need_bytes);
    if (!kbuf) { kfree(spec); return ERR_NO_MEMORY; }
    uint32_t count_u32 = (uint32_t)r.count;
    memcpy(kbuf, &count_u32, sizeof(uint32_t));
    if (r.count > 0)
        memcpy(kbuf + sizeof(uint32_t), r.region_ids,
               r.count * sizeof(uint32_t));
    error_t cr = vmm_user_buf_commit_out(ctx->proc->cabin,
                                          (uintptr_t)out->addr,
                                          kbuf, need_bytes);
    vmm_user_buf_free(kbuf);
    kfree(spec);
    if (cr != OK) return cr;

    out->size = (uint32_t)need_bytes;
    return OK;
}

/* ─── SYSTEM_OP_MEMTAG_INFO ─────────────────────────────────────────── */
static int SysMemTagInfo(const ManifestOp *op, Crate *crates,
                         uint16_t crate_count, const OpContext *ctx) {
    (void)crate_count;
    if (!ctx || !ctx->proc)                return ERR_INVALID_ARGUMENT;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    if (op->param_size < sizeof(uint32_t)) return ERR_INVALID_ARGUMENT;

    uint32_t region_id;
    memcpy(&region_id, op->params, sizeof(uint32_t));

    MemRegionSnapshot snap;
    error_t rc = MemRegionInfo(region_id, &snap);
    if (rc != OK) return rc;

    Crate *out = &crates[op->out_crate];
    if (out->capacity < sizeof(snap)) return ERR_INVALID_ARGUMENT;

    error_t cr = vmm_user_buf_commit_out(ctx->proc->cabin,
                                          (uintptr_t)out->addr,
                                          &snap, sizeof(snap));
    if (cr != OK) return cr;
    out->size = sizeof(snap);
    return OK;
}

/* ─── SYSTEM_OP_MEMTAG_LOOKUP ───────────────────────────────────────── */
static int SysMemTagLookup(const ManifestOp *op, Crate *crates,
                           uint16_t crate_count, const OpContext *ctx) {
    (void)crate_count;
    if (!ctx || !ctx->proc)                return ERR_INVALID_ARGUMENT;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    if (op->param_size < sizeof(uint64_t)) return ERR_INVALID_ARGUMENT;

    uint64_t phys;
    memcpy(&phys, op->params, sizeof(uint64_t));
    uint32_t region_id = MemRegionFromPhys((uintptr_t)phys);

    Crate *out = &crates[op->out_crate];
    if (out->capacity < sizeof(region_id)) return ERR_INVALID_ARGUMENT;

    error_t cr = vmm_user_buf_commit_out(ctx->proc->cabin,
                                          (uintptr_t)out->addr,
                                          &region_id, sizeof(region_id));
    if (cr != OK) return cr;
    out->size = sizeof(region_id);
    return OK;
}

/* ─── SYSTEM_OP_MEMTAG_TAGS ─────────────────────────────────────────── */
static int SysMemTagTags(const ManifestOp *op, Crate *crates,
                         uint16_t crate_count, const OpContext *ctx) {
    (void)crate_count;
    if (!ctx || !ctx->proc)                return ERR_INVALID_ARGUMENT;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    if (op->param_size < sizeof(uint32_t)) return ERR_INVALID_ARGUMENT;

    uint32_t region_id;
    memcpy(&region_id, op->params, sizeof(uint32_t));

    uint16_t ids[64];
    size_t   n = MemRegionListTags(region_id, ids, 64);

    Crate *out = &crates[op->out_crate];
    if (out->capacity < sizeof(uint32_t)) return ERR_INVALID_ARGUMENT;
    if (out->capacity > 16384)            return ERR_INVALID_ARGUMENT;

    uint8_t *kbuf = (uint8_t *)vmm_user_buf_alloc_out(out->capacity);
    if (!kbuf) return ERR_NO_MEMORY;

    char    *cursor = (char *)kbuf + sizeof(uint32_t);
    char    *end    = (char *)kbuf + out->capacity;
    uint32_t count  = 0;

    for (size_t i = 0; i < n; i++) {
        const char *k = MemTagKey(ids[i]);
        const char *v = MemTagValue(ids[i]);
        if (!k) continue;
        size_t klen = strlen(k);
        size_t vlen = v ? strlen(v) : 0;
        size_t need = klen + (vlen ? vlen + 1 : 0) + 1;
        if (cursor + need > end) break;
        memcpy(cursor, k, klen);
        cursor += klen;
        if (vlen) {
            *cursor++ = ':';
            memcpy(cursor, v, vlen);
            cursor += vlen;
        }
        *cursor++ = '\0';
        count++;
    }
    memcpy(kbuf, &count, sizeof(uint32_t));

    size_t total = (size_t)(cursor - (char *)kbuf);
    error_t cr = vmm_user_buf_commit_out(ctx->proc->cabin,
                                          (uintptr_t)out->addr,
                                          kbuf, total);
    vmm_user_buf_free(kbuf);
    if (cr != OK) return cr;
    out->size = (uint32_t)total;
    return OK;
}

/* ─── SYSTEM_OP_MEMTAG_STATS ────────────────────────────────────────── */
static int SysMemTagStats(const ManifestOp *op, Crate *crates,
                          uint16_t crate_count, const OpContext *ctx) {
    (void)crate_count;
    if (!ctx || !ctx->proc)                return ERR_INVALID_ARGUMENT;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    MemTagStats s;
    MemTagGetStats(&s);

    Crate *out = &crates[op->out_crate];
    if (out->capacity < sizeof(s)) return ERR_INVALID_ARGUMENT;

    error_t cr = vmm_user_buf_commit_out(ctx->proc->cabin,
                                          (uintptr_t)out->addr,
                                          &s, sizeof(s));
    if (cr != OK) return cr;
    out->size = sizeof(s);
    return OK;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Phase 2A — Capability mutation handlers (SET_GUARD / GRANT / REVOKE
 *  / CABIN_TAGS / CHECK)
 *
 *  Auth model: SET_GUARD / GRANT / REVOKE require the caller to hold
 *  the TagFS "system" tag — same gate already used by other privileged
 *  ops (proc_kill, etc.). CABIN_TAGS / CHECK are unprivileged (anyone
 *  may observe; mutating requires system).
 * ═══════════════════════════════════════════════════════════════════ */

/* ─── SYSTEM_OP_MEMTAG_SET_GUARD ───────────────────────────────────── */
/* params: [u8 on/off]; in_crate: tag string. */
static int SysMemTagSetGuard(const ManifestOp *op, Crate *crates,
                              uint16_t crate_count, const OpContext *ctx) {
    (void)crate_count;
    if (!ctx || !ctx->proc)                return ERR_INVALID_ARGUMENT;
    if (op->in_crate == CRATE_INDEX_NONE)  return ERR_INVALID_ARGUMENT;
    if (op->param_size < 1)                return ERR_INVALID_ARGUMENT;
    if (!process_has_tag(ctx->proc, "system")) return ERR_PERMISSION_DENIED;

    const Crate *in = &crates[op->in_crate];
    if (in->size == 0 || in->size > 128) return ERR_INVALID_ARGUMENT;

    char *tag = (char *)vmm_user_buf_in(ctx->proc->cabin,
                                         (uintptr_t)in->addr, in->size);
    if (!tag) return ERR_INVALID_ADDRESS;
    /* Ensure NUL-terminated */
    char tag_buf[129];
    size_t cp = in->size < 128 ? in->size : 128;
    memcpy(tag_buf, tag, cp);
    tag_buf[cp] = '\0';
    vmm_user_buf_free(tag);

    bool on = (op->params[0] != 0);
    return MemTagSetGuard(tag_buf, on);
}

/* ─── SYSTEM_OP_MEMTAG_GRANT ────────────────────────────────────────── */
/* params: [u32 pid]; in_crate: tag string. */
static int SysMemTagGrant(const ManifestOp *op, Crate *crates,
                          uint16_t crate_count, const OpContext *ctx) {
    (void)crate_count;
    if (!ctx || !ctx->proc)                return ERR_INVALID_ARGUMENT;
    if (op->in_crate == CRATE_INDEX_NONE)  return ERR_INVALID_ARGUMENT;
    if (op->param_size < sizeof(uint32_t)) return ERR_INVALID_ARGUMENT;
    if (!process_has_tag(ctx->proc, "system")) return ERR_PERMISSION_DENIED;

    uint32_t pid;
    memcpy(&pid, op->params, sizeof(uint32_t));

    const Crate *in = &crates[op->in_crate];
    if (in->size == 0 || in->size > 128) return ERR_INVALID_ARGUMENT;
    char *tag = (char *)vmm_user_buf_in(ctx->proc->cabin,
                                         (uintptr_t)in->addr, in->size);
    if (!tag) return ERR_INVALID_ADDRESS;
    char tag_buf[129];
    size_t cp = in->size < 128 ? in->size : 128;
    memcpy(tag_buf, tag, cp);
    tag_buf[cp] = '\0';
    vmm_user_buf_free(tag);

    /* Intern (creates if missing) — grant of an unknown tag is a no-op
     * functionally but auto-creates the tag entry so it can be guarded
     * later. */
    uint16_t tid = MemTagInternStr(tag_buf);
    if (tid == MEMTAG_INVALID_TAG_ID) return ERR_NO_MEMORY;
    return MemCabinGrant(pid, tid);
}

/* ─── SYSTEM_OP_MEMTAG_REVOKE ───────────────────────────────────────── */
static int SysMemTagRevoke(const ManifestOp *op, Crate *crates,
                           uint16_t crate_count, const OpContext *ctx) {
    (void)crate_count;
    if (!ctx || !ctx->proc)                return ERR_INVALID_ARGUMENT;
    if (op->in_crate == CRATE_INDEX_NONE)  return ERR_INVALID_ARGUMENT;
    if (op->param_size < sizeof(uint32_t)) return ERR_INVALID_ARGUMENT;
    if (!process_has_tag(ctx->proc, "system")) return ERR_PERMISSION_DENIED;

    uint32_t pid;
    memcpy(&pid, op->params, sizeof(uint32_t));

    const Crate *in = &crates[op->in_crate];
    if (in->size == 0 || in->size > 128) return ERR_INVALID_ARGUMENT;
    char *tag = (char *)vmm_user_buf_in(ctx->proc->cabin,
                                         (uintptr_t)in->addr, in->size);
    if (!tag) return ERR_INVALID_ADDRESS;
    char tag_buf[129];
    size_t cp = in->size < 128 ? in->size : 128;
    memcpy(tag_buf, tag, cp);
    tag_buf[cp] = '\0';
    vmm_user_buf_free(tag);

    uint16_t tid = MemTagResolveStr(tag_buf);
    if (tid == MEMTAG_INVALID_TAG_ID) return OK;  /* never interned */
    return MemCabinRevoke(pid, tid);
}

/* ─── SYSTEM_OP_MEMTAG_CABIN_TAGS ───────────────────────────────────── */
/* params: [u32 pid]; out_crate: [u32 count][char strs[]] (same shape as TAGS). */
static int SysMemTagCabinTags(const ManifestOp *op, Crate *crates,
                              uint16_t crate_count, const OpContext *ctx) {
    (void)crate_count;
    if (!ctx || !ctx->proc)                return ERR_INVALID_ARGUMENT;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    if (op->param_size < sizeof(uint32_t)) return ERR_INVALID_ARGUMENT;

    uint32_t pid;
    memcpy(&pid, op->params, sizeof(uint32_t));

    /* List up to 1024 tags (mask cap). */
    uint16_t ids[1024];
    size_t   n = MemCabinListTags(pid, ids, 1024);

    Crate *out = &crates[op->out_crate];
    if (out->capacity < sizeof(uint32_t)) return ERR_INVALID_ARGUMENT;
    if (out->capacity > 65536)            return ERR_INVALID_ARGUMENT;

    uint8_t *kbuf = (uint8_t *)vmm_user_buf_alloc_out(out->capacity);
    if (!kbuf) return ERR_NO_MEMORY;

    char    *cursor = (char *)kbuf + sizeof(uint32_t);
    char    *end    = (char *)kbuf + out->capacity;
    uint32_t count  = 0;

    for (size_t i = 0; i < n; i++) {
        const char *k = MemTagKey(ids[i]);
        const char *v = MemTagValue(ids[i]);
        if (!k) continue;
        size_t klen = strlen(k);
        size_t vlen = v ? strlen(v) : 0;
        size_t need = klen + (vlen ? vlen + 1 : 0) + 1;
        if (cursor + need > end) break;
        memcpy(cursor, k, klen);
        cursor += klen;
        if (vlen) { *cursor++ = ':'; memcpy(cursor, v, vlen); cursor += vlen; }
        *cursor++ = '\0';
        count++;
    }
    memcpy(kbuf, &count, sizeof(uint32_t));

    size_t total = (size_t)(cursor - (char *)kbuf);
    error_t cr = vmm_user_buf_commit_out(ctx->proc->cabin,
                                          (uintptr_t)out->addr,
                                          kbuf, total);
    vmm_user_buf_free(kbuf);
    if (cr != OK) return cr;
    out->size = (uint32_t)total;
    return OK;
}

/* ─── SYSTEM_OP_MEMTAG_CHECK ────────────────────────────────────────── */
/* params: [u32 pid][u32 region_id]; out_crate: [u8 allowed][u8][u8][u8]
 * [u16 missing_tag_id][u16 reserved] = 8 bytes. */
static int SysMemTagCheck(const ManifestOp *op, Crate *crates,
                          uint16_t crate_count, const OpContext *ctx) {
    (void)crate_count;
    if (!ctx || !ctx->proc)                return ERR_INVALID_ARGUMENT;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    if (op->param_size < 2 * sizeof(uint32_t)) return ERR_INVALID_ARGUMENT;

    uint32_t pid, region_id;
    memcpy(&pid,       op->params,                       sizeof(uint32_t));
    memcpy(&region_id, op->params + sizeof(uint32_t),    sizeof(uint32_t));

    struct { uint8_t allowed; uint8_t pad[3]; uint16_t missing_tag_id; uint16_t reserved; } result;
    result.allowed         = MemRegionAccessAllowed(pid, region_id) ? 1 : 0;
    result.pad[0] = result.pad[1] = result.pad[2] = 0;
    result.missing_tag_id  = result.allowed ? MEMTAG_INVALID_TAG_ID
                                            : MemRegionFirstMissingGuard(pid, region_id);
    result.reserved        = 0;

    Crate *out = &crates[op->out_crate];
    if (out->capacity < sizeof(result)) return ERR_INVALID_ARGUMENT;

    error_t cr = vmm_user_buf_commit_out(ctx->proc->cabin,
                                          (uintptr_t)out->addr,
                                          &result, sizeof(result));
    if (cr != OK) return cr;
    out->size = sizeof(result);
    return OK;
}

/* ─── Registration ──────────────────────────────────────────────────── */

error_t MemTagOpsRegister(void) {
    struct {
        uint16_t    opcode;
        OpHandler   handler;
        uint32_t    auth;
        const char *name;
    } table[] = {
        { SYSTEM_OP_MEMTAG_QUERY,      SysMemTagQuery,     OP_AUTH_APP, "system.memtag.query"      },
        { SYSTEM_OP_MEMTAG_INFO,       SysMemTagInfo,      OP_AUTH_APP, "system.memtag.info"       },
        { SYSTEM_OP_MEMTAG_LOOKUP,     SysMemTagLookup,    OP_AUTH_APP, "system.memtag.lookup"     },
        { SYSTEM_OP_MEMTAG_TAGS,       SysMemTagTags,      OP_AUTH_APP, "system.memtag.tags"       },
        { SYSTEM_OP_MEMTAG_STATS,      SysMemTagStats,     OP_AUTH_APP, "system.memtag.stats"      },
        { SYSTEM_OP_MEMTAG_SET_GUARD,  SysMemTagSetGuard,  OP_AUTH_APP, "system.memtag.set_guard"  },
        { SYSTEM_OP_MEMTAG_GRANT,      SysMemTagGrant,     OP_AUTH_APP, "system.memtag.grant"      },
        { SYSTEM_OP_MEMTAG_REVOKE,     SysMemTagRevoke,    OP_AUTH_APP, "system.memtag.revoke"     },
        { SYSTEM_OP_MEMTAG_CABIN_TAGS, SysMemTagCabinTags, OP_AUTH_APP, "system.memtag.cabin_tags" },
        { SYSTEM_OP_MEMTAG_CHECK,      SysMemTagCheck,     OP_AUTH_APP, "system.memtag.check"      },
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        error_t rc = OpRegistryRegister(OP_KIND(DECK_SYSTEM, table[i].opcode),
                                        table[i].handler,
                                        table[i].auth,
                                        table[i].name);
        if (rc != OK && rc != ERR_ALREADY_EXISTS) {
            kprintf("[MemTagOps] register %s failed: %s\n",
                    table[i].name, ErrorString(rc));
            return rc;
        }
    }
    debug_printf("[MemTagOps] registered %zu memtag ops\n",
                 sizeof(table) / sizeof(table[0]));
    return OK;
}
