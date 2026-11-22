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
void    ShowMountReset(void);

/* The notes of every program that left — the one asked or the one asking —
 * forgotten (retire/retire.c). OK, or ERR_NO_MEMORY. Inside the door. */
error_t ShowAsksForgetGone(void);

#endif /* TAGFS_SHOW_H */
