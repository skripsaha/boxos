#ifndef GROUND_OWNER_H
#define GROUND_OWNER_H

/*
 * ‼ EVERY OWNER OF THE DATA RUN, IN ONE LIST — GroundOwner.
 *
 * A block of the data run the bitmap calls taken is owned by exactly one of
 * these, or it is an orphan; a block one of them names that the bitmap calls
 * free is missing. The mount's GroundCensus (tagfs/mount/ground_census.h) asks
 * each owner for its ground, and tools/volume-read.py's census asks the same
 * owners of the same image: its constants are generated from this list
 * (tools/gen_ground_owners.py), so a kind added here and not there is a build
 * of the tool that does not match its kernel — which the tool's self-test
 * refuses — and never an owner one side forgets. A missed owner is a live
 * block handed out again the day orphans are given back.
 *
 * One line per owner: its id, the name both sides print, and whether it owns
 * alone. An exclusive owner that meets another exclusive owner on one block is
 * two owners of one block — said aloud; the one that is not exclusive lies
 * over another owner by design: the BOOT run is the kernel file's own blocks,
 * named again in the Deed for the loader.
 *
 * What a snapshot an earlier kernel took kept is no owner's: nothing reads a
 * format-v1 snapshot, so those blocks are orphans (cow.h).
 */
#define GROUND_OWNERS(X)                                          \
    X(GROUND_REGISTRY,        "registry",        1)               \
    X(GROUND_TABLE,           "table",           1)               \
    X(GROUND_POOL,            "pool",            1)               \
    X(GROUND_FILE,            "file",            1)               \
    X(GROUND_INTEGRITY,       "integrity",       1)               \
    X(GROUND_MANIFEST,        "manifest",        1)               \
    X(GROUND_USE_CHAIN,       "use_chain",       1)               \
    X(GROUND_BOOT,            "boot",            0)

typedef enum {
#define GROUND_OWNER_ID(id, name, exclusive) id,
    GROUND_OWNERS(GROUND_OWNER_ID)
#undef GROUND_OWNER_ID
    GROUND_OWNER_KINDS
} GroundOwner;

#endif /* GROUND_OWNER_H */
