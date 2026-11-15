#ifndef TAGFS_RESERVED_H
#define TAGFS_RESERVED_H
/* Single source of the reserved tag vocabulary: the keys a running program's
 * cabin is given its authority by. List ORDER is two contracts at once:
 *   - the on-disk id: the Nth key is laid at tag_id N (0-based) by the image
 *     builder (tools/create_tagfs_cargo.c), and the kernel stamps the same
 *     keys as the system's tags at every mount (tagfs.c TagFsReservedKeys[]);
 *   - the authority bit: AUTH_TAG_<ID> is 1u << position (auth_tags.h),
 *     INDEPENDENT of the registry id, so a key need not land below id 64.
 * Ids are dense until the first release; from it on a key that is retired
 * leaves its id unused. Pure preprocessor + string literals — no kernel
 * headers, host-compilable. */
#define TAGFS_RESERVED_KEYS(X) \
    X(SYSTEM,"system") X(UTILITY,"utility") X(APP,"app") X(GOD,"god") \
    X(STOPPED,"stopped") X(BYPASS,"bypass") X(NETWORK,"network")
#define TAGFS_RESERVED_COUNT 7
#endif
