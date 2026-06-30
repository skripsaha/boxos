#ifndef TAGFS_RESERVED_H
#define TAGFS_RESERVED_H
/* Single source of the reserved tag vocabulary. List ORDER is the on-disk ID
 * contract: the Nth key formats to tag_id N (0-based). Consumed by the kernel
 * (tagfs.c format-seed + TagFsReservedKeys[]) and the host mkfs tool
 * (tools/create_tagfs.c). Pure preprocessor + string literals — no kernel
 * headers, host-compilable. Keys 0..6 are the auth-privilege tags (see
 * TAGFS_AUTH_KEYS below); their op-authority is now a FIXED bit (auth_tags.h),
 * INDEPENDENT of the registry id, so they no longer need to land below id 64. */
#define TAGFS_RESERVED_KEYS(X) \
    X("system") X("utility") X("app") X("god") X("stopped") X("bypass") \
    X("network") X("trashed") X("hidden") X("autostart") X("snapshot") X("name")
#define TAGFS_RESERVED_COUNT 12

/* Auth-privilege subset = first 7 reserved keys. Authority bit is FIXED
 * (1u<<position, auth_tags.h), INDEPENDENT of TagFS registry id. Must stay the
 * prefix of TAGFS_RESERVED_KEYS — tagfs.c asserts this at mount. */
#define TAGFS_AUTH_KEYS(X) \
    X(SYSTEM,"system") X(UTILITY,"utility") X(APP,"app") X(GOD,"god") \
    X(STOPPED,"stopped") X(BYPASS,"bypass") X(NETWORK,"network")
#define TAGFS_AUTH_COUNT 7
#endif
