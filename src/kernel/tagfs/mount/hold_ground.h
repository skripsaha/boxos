#ifndef TAGFS_MOUNT_HOLD_GROUND_H
#define TAGFS_MOUNT_HOLD_GROUND_H

#include "../tagfs.h"

/*
 * HOLDGROUND=on — a caller inside the volume when its medium leaves, and a
 * file held open across it (the note in hold_ground.c). A reproduction, not a
 * feature: every call is nothing at all unless `make HOLDGROUND=on`.
 *
 *   OpenFile     at the first mount: a file held open from then on;
 *   Take         the departure noticed: a caller steps in;
 *   Release      a take-down waits: the caller steps out once its time is up;
 *   AfterReturn  the volume mounted again: what the held handle sees;
 *   Resumed      the open book taken up: the caller steps out, and the same.
 */
#if CONFIG_TAGFS_HOLD_GROUND
void TagFSHoldGroundOpenFile(TagFSVolume* vol);
void TagFSHoldGroundTake(TagFSVolume* vol);
void TagFSHoldGroundRelease(TagFSVolume* vol);
void TagFSHoldGroundAfterReturn(TagFSVolume* vol);
void TagFSHoldGroundResumed(TagFSVolume* vol);
#else
#define TagFSHoldGroundOpenFile(vol)    ((void)(vol))
#define TagFSHoldGroundTake(vol)        ((void)(vol))
#define TagFSHoldGroundRelease(vol)     ((void)(vol))
#define TagFSHoldGroundAfterReturn(vol) ((void)(vol))
#define TagFSHoldGroundResumed(vol)     ((void)(vol))
#endif

#endif /* TAGFS_MOUNT_HOLD_GROUND_H */
