#ifndef AUTH_TAGS_H
#define AUTH_TAGS_H


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

static inline uint32_t auth_bit_for_key(const char *key)
{
#define X(id, keystr) if (__builtin_strcmp(key, keystr) == 0) return AUTH_TAG_##id;
    TAGFS_AUTH_KEYS(X)
#undef X
    return 0;
}

#endif