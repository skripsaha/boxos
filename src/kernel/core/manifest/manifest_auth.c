/*
 * Manifest op authorization. See manifest_auth.h for policy.
 */

#include "manifest_auth.h"
#include "klib.h"
#include "process.h"
#include "tagfs.h"

bool ManifestOpAuthorize(uint32_t op_kind, const OpContext *ctx)
{
    /* Kernel-internal manifests (selftest, init paths) bypass auth — they
     * never carry a process. */
    if (!ctx || !ctx->proc) return true;

    const OpRegistration *reg = OpRegistryLookup(op_kind);
    if (!reg) return false;

    uint32_t level = reg->security_mask;
    if (level == OP_AUTH_NONE) return true;

    process_t     *p = ctx->proc;
    WellKnownTags *w = tagfs_get_well_known_tags();
    /* Tagfs not initialized yet (boot-time selftest already filtered out by
     * the proc==NULL check above): allow. */
    if (!w) return true;

    /* god overrides every restriction. */
    if (p->cabin->tag_bits & w->god) return true;
    /* stopped processes can do nothing. */
    if (p->cabin->tag_bits & w->stopped) return false;

    uint64_t allowed;
    switch (level) {
    case OP_AUTH_APP:     allowed = w->app | w->utility | w->system | w->bypass; break;
    case OP_AUTH_UTILITY: allowed = w->utility | w->system | w->bypass;          break;
    case OP_AUTH_SYSTEM:  allowed = w->system | w->bypass;                       break;
    case OP_AUTH_NETWORK: allowed = w->network | w->system | w->bypass;          break;
    default:              return true;  /* unknown level — fail open */
    }

    return (p->cabin->tag_bits & allowed) != 0;
}
