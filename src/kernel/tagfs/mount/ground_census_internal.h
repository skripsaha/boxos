#ifndef TAGFS_GROUND_CENSUS_INTERNAL_H
#define TAGFS_GROUND_CENSUS_INTERNAL_H

#include "ground_census.h"

/*
 * The census's own parts (ground_census.c, ground_shares.c): a claim as the
 * census keeps it, the claims as they stand, and the count of the owners of
 * each block in common taken from them.
 */

typedef struct {
    uint64_t start;
    uint64_t who;
    uint32_t count;
    uint32_t owner;
    uint32_t namer;     /* the Spine slot that gives it, + 1; 0 when no slot does */
} GroundRun;

/* The claims as they stand, in no order the caller may rely on: *n of them.
 * The mount's alone. */
GroundRun* GroundCensusRuns(TagFSVolume* vol, uint32_t* n);

/* Commonhold built anew from the claims (ink/commonhold.h): every block a run
 * SHARED names, owned by every file's run that names it — DATA or SHARED —
 * and, one owner for all of them, by what is held there for a rotten range or
 * a locked file. Before the census cuts the held to what no name owns. OK, or
 * ERR_NO_MEMORY said. */
error_t    GroundSharesCounted(TagFSVolume* vol);

/* What one volume holds of ground_census.c (TagFSVolume, volume/tagfs_volume.h). */
typedef struct {
    /* The mount's alone (the door is shut). */
    GroundRun* runs;
    uint32_t count;
    uint32_t cap;
    bool refused;  /* a claim found no memory: the census decides nothing */
    uint64_t reach;  /* past the farthest block an owner but the far Deed names */
    uint64_t doubled;  /* blocks the census said two owners name */
} GroundCensusState;

#endif /* TAGFS_GROUND_CENSUS_INTERNAL_H */
