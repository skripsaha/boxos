#ifndef TAGFS_RESERVED_H
#define TAGFS_RESERVED_H
/* Single source of what the kernel and the image builder both say about a
 * volume's tags: the reserved vocabulary at the bottom of the ids, the last id
 * a volume gives, and how wide a tag the kernel holds. Pure preprocessor +
 * string literals — no kernel headers, host-compilable; the kernel takes it
 * through tagfs_constants.h, the builder (tools/create_tagfs_cargo.c)
 * directly, and neither spells a number of it again.
 *
 * The reserved keys' list ORDER is two contracts at once:
 *   - the on-disk id: the Nth key is laid at tag_id N (0-based) by the image
 *     builder, and the kernel stamps the same keys as the system's tags at
 *     every mount (tagfs.c TagFsReservedKeys[]);
 *   - the authority bit: AUTH_TAG_<ID> is 1u << position (auth_tags.h),
 *     INDEPENDENT of the registry id, so a key need not land below id 64.
 * Ids are dense until the first release; from it on a key that is retired
 * leaves its id unused. */
#define TAGFS_RESERVED_KEYS(X) \
    X(SYSTEM,"system") X(UTILITY,"utility") X(APP,"app") X(GOD,"god") \
    X(STOPPED,"stopped") X(BYPASS,"bypass") X(NETWORK,"network") \
    X(SEE_TAGS,"see-tags") X(CHANGE_TAGS,"change-tags") X(CHANGE_DISKS,"change-disks")
#define TAGFS_RESERVED_COUNT 10

/* Volume tag ids stop one short of bit 31. Bit 31 belongs to the kernel's
 * Logbook (core/touch/logbook.h) — the vocabulary of occurrences, which must
 * exist on a machine that never mounted a volume. Capping here is what makes
 * the split an invariant instead of a convention: the volume cannot issue an
 * id that collides with an occurrence even in principle. The book writes an id
 * as a varint and has no width of its own; a volume naming an id past this one
 * is a volume the kernel may only read (tally.py TagGlass), so the builder
 * interns none past it. */
#define TAGFS_MAX_TAG_ID            0x7FFFFFFEu

/* The widest tag the kernel holds: a key of at most TAG_KEY_WIDTH bytes, and
 * its key and value together at most TAG_KEY_VALUE_WIDTH — what every station,
 * the book's TAGS and a stitch's Coined hold a tag to (tag_spelling.h,
 * mount/tally_leaves.c, settle/endpaper_fact.h). The builder refuses a wider
 * one before its first write: a volume carrying it could only be read. */
#define TAG_KEY_WIDTH               255u
#define TAG_KEY_VALUE_WIDTH         4074u
#endif
