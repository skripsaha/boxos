#ifndef TAGFS_SHOW_H
#define TAGFS_SHOW_H

#include "../tagfs.h"

/*
 * The asks a program left for another to show it its tags (show.c): notes the
 * kernel holds in memory, one a pair of programs, until the program asked
 * answers — shows its tags to the asker or hides them from it
 * (TagFSShowTags) — either program leaves (the Retire duty), or the volume is
 * mounted again. Never on the medium: an ask is a moment's, and one lost to a
 * restart is asked again.
 */

/* Every note forgotten: a mount's start (settle.c). */
void    ShowMountReset(TagFSVolume* vol);

/* The notes of every program that left — the one asked or the one asking —
 * forgotten (retire/retire.c). OK, or ERR_NO_MEMORY. Inside the door. */
error_t ShowAsksForgetGone(TagFSVolume* vol);

/* One ask: `asker` asked `owner` to show it its tags. */
typedef struct {
    uint64_t owner;
    uint64_t asker;
} ShowAsk;

/* What one volume holds of show.c (TagFSVolume, volume/tagfs_volume.h). */
typedef struct {
    /* The notes, rising by owner and then by asker — a leaf lock: no other TagFS
     * lock is taken while it is held. */
    TagFSLock asks_lock;
    ShowAsk* asks;
    uint32_t asks_count;
    uint32_t asks_room;
} ShowState;

void ShowVolumeBegin(TagFSVolume* vol);

#endif /* TAGFS_SHOW_H */
