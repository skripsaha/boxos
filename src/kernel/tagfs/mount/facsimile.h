#ifndef TAGFS_MOUNT_FACSIMILE_H
#define TAGFS_MOUNT_FACSIMILE_H

#include "tally_read.h"

/*
 * Facsimile — a rotten range taken from an older slot. A node below the root
 * of FILES or TAGS that reads in no copy is a rotten range of the Tally the
 * mount stands on; the slots before it hold, and their ribbon with them
 * (§3.4). The newest of them whose Tally reads in that range — and every root
 * of it, as a slot the mount could stand on — and whose ribbon goes on from
 * its `at` to the stitch the Tally stood on took in, gives the range back: its
 * elements read off that Tally, the records of the ribbon for the range's keys
 * alone replayed over them to that stitch — the state the Tally stood on, for
 * those keys. The next Tally lays the range anew (DogEar). A slot stands in
 * only where every file the range reaches stands in its Tally and its ribbon
 * is replayed for those keys whole; two FILES ranges that reach into one fid
 * are given back together or not at all. Honestly rarer than with whole
 * Tallies: a node Tallies laid in share rots for all of them, and no older
 * slot stands in (§16.5 п.16).
 *
 * Built for FILES and TAGS. A DIGESTS leaf is read only when a block of it is
 * (§16.3 п.12), so the mount does not weigh one, and a DIGESTS leaf no copy of
 * which reads is laid anew of the data by Escheat alone (settle/escheat.h); a
 * rotten node of USE is said and named as it lies.
 *
 * One rule with volume-read (tools/volumeread/bookmount.py, _Facsimile): the
 * state an older slot's Tally and ribbon give is the one binder.BindCheck
 * holds a Tally to.
 *
 *   FacsimileRotNoted  the walk of the Tally stood on: a rotten range of FILES
 *                      or TAGS noted, with why — said, and what it locks
 *                      locked, only once MountFacsimile has weighed it.
 *                      OK or ERR_NO_MEMORY
 *   MountFacsimile     once the walk is done, before the ribbon past the
 *                      Tally: every range noted given back or said rotten,
 *                      the files it reached taken or locked. OK, or memory's
 *                      or the medium's refusal, with the mount's words in
 *                      *what
 *   FacsimileLetGo     the ranges noted, and the files set aside, let go
 */

error_t FacsimileRotNoted(TagFSVolume* vol, uint64_t tree, const TallyPlace* at, const char* why);
error_t MountFacsimile(TagFSVolume* vol, const char** what);
void    FacsimileLetGo(TagFSVolume* vol);

typedef struct {
    uint64_t   tree;
    TallyPlace at;
    bool       taken;
    char       why[160];
} FacsimileRange;

/* What one volume holds of facsimile.c (TagFSVolume, volume/tagfs_volume.h). */
typedef struct {
    FacsimileRange* ranges;
    uint32_t nranges;
    uint32_t capranges;
} FacsimileState;

#endif /* TAGFS_MOUNT_FACSIMILE_H */
