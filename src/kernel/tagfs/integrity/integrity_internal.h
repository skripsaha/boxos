#ifndef TAGFS_INTEGRITY_INTERNAL_H
#define TAGFS_INTEGRITY_INTERNAL_H

#include "integrity.h"
#include "../tagfs.h"

/*
 * What the verify (integrity.c) and the rot a mount names (integrity_rot.c)
 * share, and nobody else.
 */

/* A mismatch IntegrityVerify found, named — said and published once a mount;
 * and a block no DIGESTS element names, named as rot is. Called with no TagFS
 * lock held. */
void     IntegrityRotName(TagFSVolume* vol, uint64_t block, uint64_t stored, uint64_t read);
void     IntegrityUndigestedName(TagFSVolume* vol, uint64_t block);

/* What one volume holds of integrity_rot.c (TagFSVolume, volume/tagfs_volume.h). */
typedef struct {
    /*
     * The rotten blocks this mount has named.
     *
     * A block whose bytes do not match the digest it was written with is named
     * the moment a read finds it: one line that says which block and why it may
     * be, and one Touch on the "integrity" tag — each once a mount, however often
     * the block is read after. What remembers that it was named is a set that
     * grows as rot is found — the machine is told of
     * every rotten block, not the first 64 a ring had room for (the 65th was
     * counted and never told). A set that cannot grow, the heap spent, still has
     * each rot published and answered to its reader; only its own line is not
     * said, and how many went unnamed is said when the volume is let go: "and N
     * more".
     *
     * The set is open-addressed on block + 1 (0 is an empty slot), at most half
     * full, grown with its lock let go: the bigger table is allocated outside it
     * and swapped in under it. Its own leaf lock: a rot is named from inside a
     * read.
     */
    TagFSLock rot_lock;
    uint64_t* rot_set;
    uint32_t rot_slots;  /* a power of two, or 0 */
    uint32_t rot_named;  /* blocks in the set */
    uint32_t rot_more;  /* rot found that the set had no room to name */
    bool rot_room_said;  /* the set's want of room said for this run of misses */
    uint32_t rot_tag;
} IntegrityRotState;

/* What one volume holds of integrity.c (TagFSVolume, volume/tagfs_volume.h). */
typedef struct {
    uint32_t errors;
} IntegrityState;

void IntegrityRotVolumeBegin(TagFSVolume* vol);

#endif /* TAGFS_INTEGRITY_INTERNAL_H */
