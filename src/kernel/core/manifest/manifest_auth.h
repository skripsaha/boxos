#ifndef MANIFEST_AUTH_H
#define MANIFEST_AUTH_H

#include "ktypes.h"
#include "op_registry.h"
#include "auth_tags.h"


#define OP_AUTH_NONE     0u
#define OP_AUTH_APP      1u
#define OP_AUTH_UTILITY  2u
#define OP_AUTH_SYSTEM   3u
#define OP_AUTH_NETWORK  4u

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

static inline bool auth_level_permits(uint32_t auth_bits, uint32_t level)
{
    if (level == OP_AUTH_NONE)        return true;
    if (auth_bits & AUTH_TAG_GOD)     return true;
    if (auth_bits & AUTH_TAG_STOPPED) return false;
    uint32_t allowed = auth_mask_for_level(level);
    if (allowed == 0) return true;
    return (auth_bits & allowed) != 0;
}

bool ManifestOpAuthorize(uint32_t op_kind, const OpContext *ctx);

#endif