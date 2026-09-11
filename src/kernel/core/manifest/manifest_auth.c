
#include "manifest_auth.h"
#include "process.h"

bool ManifestOpAuthorize(uint32_t op_kind, const OpContext *ctx)
{
    if (!ctx || !ctx->proc || !ctx->proc->cabin) return true;

    const OpRegistration *reg = OpRegistryLookup(op_kind);
    if (!reg) return false;

    return auth_level_permits(ctx->proc->cabin->auth_bits, reg->security_mask);
}