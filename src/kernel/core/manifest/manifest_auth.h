#ifndef MANIFEST_AUTH_H
#define MANIFEST_AUTH_H

#include "ktypes.h"
#include "op_registry.h"
#include "auth_tags.h"

/*
 * Manifest op authorization — Phase 13 restoration of the kernel-side gate
 * that the prefix-chain dispatcher used to provide via system_security_gate.
 *
 * Each registered op declares an auth level via OpRegistration.security_mask
 * (re-purposed from a bitfield to an enum value). ManifestExecuteOnce calls
 * ManifestOpAuthorize before invoking each handler; on denial the op fails
 * with ERR_ACCESS_DENIED and the rest of the manifest follows the standard
 * OP_FLAG_OPTIONAL / OP_FLAG_SKIP_ON_ERROR semantics.
 *
 * Levels are ordered: SYSTEM > UTILITY > APP > NONE. A process with the
 * "system" tag passes any check; "utility" passes APP+UTILITY; "app" passes
 * APP only.
 *
 * Special tags:
 *   "god"     — passes everything regardless of level
 *   "stopped" — fails everything (process frozen)
 */

#define OP_AUTH_NONE     0u   /* anyone (kernel-internal manifests too) */
#define OP_AUTH_APP      1u   /* app | utility | system */
#define OP_AUTH_UTILITY  2u   /* utility | system */
#define OP_AUTH_SYSTEM   3u   /* system | bypass */
#define OP_AUTH_NETWORK  4u   /* network */

/* Fixed auth bits that satisfy each level. Mirrored by the spawn gate
 * (system_ops.c) so a child can never gain an authority its spawner lacks. */
static inline uint32_t auth_mask_for_level(uint32_t level)
{
    switch (level) {
    case OP_AUTH_APP:     return AUTH_TAG_APP | AUTH_TAG_UTILITY | AUTH_TAG_SYSTEM | AUTH_TAG_BYPASS;
    case OP_AUTH_UTILITY: return AUTH_TAG_UTILITY | AUTH_TAG_SYSTEM | AUTH_TAG_BYPASS;
    case OP_AUTH_SYSTEM:  return AUTH_TAG_SYSTEM | AUTH_TAG_BYPASS;
    case OP_AUTH_NETWORK: return AUTH_TAG_NETWORK | AUTH_TAG_SYSTEM | AUTH_TAG_BYPASS;
    default:              return 0;
    }
}

/* The pure gate decision over fixed auth bits — no registry, no process lookup,
 * so the boot [AUTHDEC] self-test exercises the real predicate directly. */
static inline bool auth_level_permits(uint32_t auth_bits, uint32_t level)
{
    if (level == OP_AUTH_NONE)        return true;  /* unrestricted op */
    if (auth_bits & AUTH_TAG_GOD)     return true;  /* god overrides all */
    if (auth_bits & AUTH_TAG_STOPPED) return false; /* frozen process */
    uint32_t allowed = auth_mask_for_level(level);
    if (allowed == 0) return true;                  /* unknown level — fail open */
    return (auth_bits & allowed) != 0;
}

bool ManifestOpAuthorize(uint32_t op_kind, const OpContext *ctx);

#endif /* MANIFEST_AUTH_H */
