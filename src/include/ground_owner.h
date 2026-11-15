#ifndef GROUND_OWNER_H
#define GROUND_OWNER_H

/*
 * ‼ EVERY OWNER OF THE VOLUME'S GROUND, IN ONE LIST — GroundOwner.
 *
 * A volume keeps no bitmap at all — which blocks are taken is derived from
 * these owners' names, and the only finding left is a block two of them name.
 * The mount's census (tagfs/mount/ground_census.h) asks each
 * owner for its ground, and tools/volume-read.py's census asks the same
 * owners of the same image: its constants are generated from this list
 * (tools/gen_ground_owners.py), so a kind added here and not there is a build
 * of the tool that does not match its kernel — which the tool's self-test
 * refuses — and never an owner one side forgets. A missed owner is a live
 * block handed out again.
 *
 * One line per owner: its id, the name both sides print, and whether it owns
 * alone. An exclusive owner that meets another exclusive owner on one block
 * is two owners of one block — said aloud; the one that is not exclusive lies
 * over another owner by design: the BOOT run is the kernel file's own blocks,
 * named again in the Deed for the loader.
 *
 * The owners: a file's runs, the front of the volume before its data run that
 * no slot takes, the Spine's slots, every node of every Tally a slot names
 * (every copy its pointer names), the ribbon's Quires, the far copy of the
 * Deed, the blocks held for what rotted (a rotten range's files, locked —
 * their own class, never an orphan, given to no one until an explicit act
 * lets them go), and the ground the ribbon let go, lying fallow until the
 * Quire it was let go in is left behind (M-1). A file's run that carries
 * SHARED (§8.2) is not exclusive: it lies over the one DATA run that names its
 * block, or over none once that owner has gone, beside other SHARED runs — on
 * any other owner's ground it is two owners of one block. A bookmark's ground
 * (§4.6: the nodes of its trees, the runs under its Lien) is no owner's here:
 * this kernel does not read it, so a book whose Tally names a bookmark has its
 * free blocks not counted at all (book_ground.c).
 */
#define GROUND_OWNERS(X)                                 \
    X(GROUND_FILE,            "file",            1)      \
    X(GROUND_BOOT,            "boot",            0)      \
    X(GROUND_FRONT,           "front",           1)      \
    X(GROUND_SPINE,           "spine",           1)      \
    X(GROUND_TALLY,           "tally",           1)      \
    X(GROUND_RIBBON,          "ribbon",          1)      \
    X(GROUND_FAR_DEED,        "far_deed",        1)      \
    X(GROUND_HELD,            "held",            1)      \
    X(GROUND_FALLOW,          "fallow",          1)      \
    X(GROUND_SHARED,          "shared",          0)

typedef enum {
#define GROUND_OWNER_ID(id, name, exclusive) id,
    GROUND_OWNERS(GROUND_OWNER_ID)
#undef GROUND_OWNER_ID
    GROUND_OWNER_KINDS
} GroundOwner;

/* An owner by the name its lines say — the census's, and the block map's
 * when a claim meets a block taken already. */
static inline const char *GroundOwnerName(GroundOwner owner)
{
    static const char *const said[GROUND_OWNER_KINDS] = {
#define GROUND_OWNER_SAID(id, name, exclusive) [id] = name,
        GROUND_OWNERS(GROUND_OWNER_SAID)
#undef GROUND_OWNER_SAID
    };
    return owner < GROUND_OWNER_KINDS ? said[owner] : "unknown owner";
}

#endif /* GROUND_OWNER_H */
