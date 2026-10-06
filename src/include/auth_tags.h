#ifndef AUTH_TAGS_H
#define AUTH_TAGS_H

/*
 * Fixed process op-authority bits, decoupled from the TagFS registry id.
 *
 * Each of the 7 auth-privilege keys (TAGFS_AUTH_KEYS) owns a FIXED bit
 * 1u<<position in cabin_t.auth_bits — never (1u<<registry_id). A cabin sets
 * AUTH_TAG_X iff it carries the BARE key X (value empty); the three mutation
 * points (cabin_create's cabin_wear_tags, process_add_tag, process_remove_tag)
 * funnel through the single checkpoint AuthBitForKey. The gate
 * (manifest_auth.h), the spawn gate (system_ops.c) and the Ink guard's warrant
 * (storage_ops.c, StorageCallWarrant) read these bits, so a privilege never
 * dies when its registry id interns past 63.
 */

#include "ktypes.h"
#include "tagfs_reserved.h"

enum AuthTagIndex {
#define X(id, key) AUTH_IX_##id,
    TAGFS_AUTH_KEYS(X)
#undef X
    AUTH_TAG_COUNT
};

enum AuthTagBit {
#define X(id, key) AUTH_TAG_##id = (1u << AUTH_IX_##id),
    TAGFS_AUTH_KEYS(X)
#undef X
};

_Static_assert(AUTH_TAG_COUNT <= 32, "auth_bits is uint32_t");
_Static_assert(AUTH_TAG_COUNT == TAGFS_AUTH_COUNT, "auth vocab count drift");

/* The single checkpoint: the key_len bytes of a bare auth key -> its fixed
 * bit, 0 for any other key. A key as a tag's spelling holds it, in place. */
static inline uint32_t AuthBitForKey(const char *key, size_t key_len)
{
#define X(id, keystr) \
    if (key_len == sizeof(keystr) - 1 && __builtin_memcmp(key, keystr, key_len) == 0) \
        return AUTH_TAG_##id;
    TAGFS_AUTH_KEYS(X)
#undef X
    return 0;
}

#endif /* AUTH_TAGS_H */
