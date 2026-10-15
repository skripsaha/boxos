#ifndef TAGFS_INTEGRITY_GLOSS_H
#define TAGFS_INTEGRITY_GLOSS_H

#include "../tagfs.h"

/*
 * Gloss — the digests the ribbon's records carry for the blocks they put in
 * (spec_v2 §2.8, §8.1), held beside the Tally's DIGESTS.
 *
 * A Born or a Shaped names every block of fresh data it puts in with the
 * digest it was written with: 8 bytes a block, in the stitch. The Tally
 * learns them only when the next Tally is written; until then the ribbon is
 * the only word on what such a block holds, so the mount that replays the
 * ribbon keeps them here, and a read of a block of format v2 is held to the
 * Gloss first and to the Tally's DIGESTS only for a block the Gloss does not
 * name (integrity.c, IntegrityVerifyBook). A later record's digest of a block
 * stands over an earlier one's.
 *
 * Made by the mount with the door shut (ribbon_transcribe.c: GlossNote for
 * each digest a stitch applied carries, then GlossSealed), never changed
 * after it, and let go with the volume (MountBookLetGo): read with no lock.
 * The writer of the ribbon, which adds to it while the volume is up, is the
 * next step's — with it a lock.
 */

/* A digest the replay met, in its order: OK, or ERR_NO_MEMORY. */
error_t GlossNote(uint64_t block, uint64_t digest);

/* Every digest noted, sorted by block, the latest of a block standing. */
void    GlossSealed(void);

/* The digest the ribbon names for `block`: false when it names none. */
bool    GlossFind(uint64_t block, uint64_t* digest);

void    GlossLetGo(void);

#endif /* TAGFS_INTEGRITY_GLOSS_H */
