#ifndef GROUND_OWNER_H
#define GROUND_OWNER_H

/*
 * ‼ EVERY OWNER OF THE VOLUME'S GROUND, IN ONE LIST — GroundOwner.
 *
 * A format-v1 volume keeps a bitmap of its data run: a block the bitmap calls
 * taken is owned by exactly one of these, or it is an orphan; a block one of
 * them names that the bitmap calls free is missing. A format-v2 volume keeps
 * no bitmap at all — which blocks are taken is derived from these owners'
 * names, and the only finding left is a block two of them name (spec_v2
 * §4.6). The mount's census (tagfs/mount/ground_census.h) asks each owner of
 * the volume's format for its ground, and tools/volume-read.py's census asks
 * the same owners of the same image: its constants are generated from this
 * list (tools/gen_ground_owners.py), so a kind added here and not there is a
 * build of the tool that does not match its kernel — which the tool's
 * self-test refuses — and never an owner one side forgets. A missed owner is
 * a live block handed out again.
 *
 * One line per owner: its id, the name both sides print, whether it owns
 * alone, and the formats it owns ground in (1 format v1, 2 format v2, 3
 * both). An exclusive owner that meets another exclusive owner on one block
 * is two owners of one block — said aloud; the one that is not exclusive lies
 * over another owner by design: the BOOT run is the kernel file's own blocks,
 * named again in the Deed for the loader.
 *
 * Format v2's: the front of the volume before its data run that no slot
 * takes, the Spine's slots, every node of every Tally a slot names (every
 * copy its pointer names), the ribbon's Quires, the far copy of the Deed, the
 * blocks held for what rotted (a rotten range's files, locked — their own
 * class, never an orphan, given to no one until an explicit act lets them
 * go), and the ground the ribbon let go, lying fallow until the Quire it was
 * let go in is left behind (M-1). A file's run that carries SHARED (§8.2) is
 * not exclusive: it lies over the one DATA run that names its block, or over
 * none once that owner has gone, beside other SHARED runs — on any other
 * owner's ground it is two owners of one block. A bookmark's ground (§4.6:
 * the nodes of its trees, the runs under its Lien) is no owner's here: this
 * kernel does not read it, so a book whose Tally names a bookmark has its
 * free blocks not counted at all (book_ground.c).
 *
 * What a snapshot an earlier kernel took kept is no owner's: nothing reads a
 * format-v1 snapshot, so those blocks are orphans (cow.h).
 */
#define GROUND_OWNERS(X)                                          \
    X(GROUND_REGISTRY,        "registry",        1, 1)            \
    X(GROUND_TABLE,           "table",           1, 1)            \
    X(GROUND_POOL,            "pool",            1, 1)            \
    X(GROUND_FILE,            "file",            1, 3)            \
    X(GROUND_INTEGRITY,       "integrity",       1, 1)            \
    X(GROUND_MANIFEST,        "manifest",        1, 1)            \
    X(GROUND_USE_CHAIN,       "use_chain",       1, 1)            \
    X(GROUND_BOOT,            "boot",            0, 3)            \
    X(GROUND_FRONT,           "front",           1, 2)            \
    X(GROUND_SPINE,           "spine",           1, 2)            \
    X(GROUND_TALLY,           "tally",           1, 2)            \
    X(GROUND_RIBBON,          "ribbon",          1, 2)            \
    X(GROUND_FAR_DEED,        "far_deed",        1, 2)            \
    X(GROUND_HELD,            "held",            1, 2)            \
    X(GROUND_FALLOW,          "fallow",          1, 2)            \
    X(GROUND_SHARED,          "shared",          0, 2)

typedef enum {
#define GROUND_OWNER_ID(id, name, exclusive, formats) id,
    GROUND_OWNERS(GROUND_OWNER_ID)
#undef GROUND_OWNER_ID
    GROUND_OWNER_KINDS
} GroundOwner;

/* An owner by the name its lines say — the census's, and the block map's of
 * format v2 when a claim meets a block taken already. */
static inline const char *GroundOwnerName(GroundOwner owner)
{
    static const char *const said[GROUND_OWNER_KINDS] = {
#define GROUND_OWNER_SAID(id, name, exclusive, formats) [id] = name,
        GROUND_OWNERS(GROUND_OWNER_SAID)
#undef GROUND_OWNER_SAID
    };
    return owner < GROUND_OWNER_KINDS ? said[owner] : "unknown owner";
}

#endif /* GROUND_OWNER_H */
