/*
 * Manifest op authorization. See manifest_auth.h for policy.
 */

#include "manifest_auth.h"
#include "process.h"

bool ManifestOpAuthorize(uint32_t op_kind, const OpContext *ctx)
{
    /* Kernel-internal manifests (selftest, init paths) bypass auth — they
     * never carry a process. (Guard the cabin too so the auth_bits read below
     * is never reached with a half-built context.) */
    if (!ctx || !ctx->proc || !ctx->proc->cabin) return true;

    const OpRegistration *reg = OpRegistryLookup(op_kind);
    if (!reg) return false;

    /* Authority is the cabin's FIXED auth_bits (auth_tags.h), independent of
     * any TagFS registry id — a privilege never dies when its tag interns past
     * id 63. The decision itself lives in auth_level_permits (manifest_auth.h)
     * so the boot [AUTHDEC] self-test drives the same predicate. */
    return auth_level_permits(ctx->proc->cabin->auth_bits, reg->security_mask);
}
