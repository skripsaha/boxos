#ifndef TAGFS_RESERVED_H
#define TAGFS_RESERVED_H
/* Single source of the reserved tag vocabulary. List ORDER is the on-disk ID
 * contract: the Nth key formats to tag_id N (0-based). Consumed by the kernel
 * (tagfs.c format-seed + TagFsReservedKeys[]) and the host mkfs tool
 * (tools/create_tagfs.c). Pure preprocessor + string literals — no kernel
 * headers, host-compilable. Keys 0..6 are the auth-privilege tags whose
 * (1ULL<<id) masks drive ManifestOpAuthorize; they MUST stay within ids 0..63. */
#define TAGFS_RESERVED_KEYS(X) \
    X("system") X("utility") X("app") X("god") X("stopped") X("bypass") \
    X("network") X("trashed") X("hidden") X("autostart") X("snapshot") X("name")
#define TAGFS_RESERVED_COUNT 12
#endif
