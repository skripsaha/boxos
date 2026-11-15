#ifndef TAGFS_INTEGRITY_GLOSS_H
#define TAGFS_INTEGRITY_GLOSS_H

#include "../tagfs.h"

/*
 * Gloss — the digests of the blocks of fresh data no Tally names yet, held
 * beside the Tally's DIGESTS.
 *
 * A Born or a Shaped names every block of fresh data it puts in with the
 * digest it was written with: 8 bytes a block, in the stitch. The Tally learns
 * them only when the next Tally is written; until then the ribbon is the only
 * word on what such a block holds. So the mount that replays the ribbon notes
 * each digest a stitch applied carries here (GlossNote, ribbon_transcribe.c),
 * an Ink notes the digests of the blocks a write put in as it puts the record
 * in the mirror (GlossPut, under the state lock, beside the record), and the
 * stitch that names those blocks reads them back (GlossCopy) — a read of a
 * block is held to the Gloss first and to the Tally's DIGESTS only for a
 * block the Gloss does not name (integrity.c, IntegrityVerify).
 * A later digest of a block stands over an earlier one's; a block given back
 * to the allocator from Fallow takes its digest out with it (GlossForget), so
 * its next owner is held to its own.
 *
 * ‼ A TREE OF THE VOLUME'S BLOCKS, AND NOTHING ASKED OF THE HEAP UNDER ITS
 * LOCK. The digests lie in nodes of 64 slots, a level of the tree for each six
 * bits of a block number — a file's run fills its nodes in a row, 8 bytes a
 * block and a little — and a node is made before the lock it is put in under:
 * a writer asks GlossRoomFor what its runs may want, with no TagFS lock held,
 * takes the nodes it can from the ones the Gloss keeps spare and the heap the
 * rest, and gives back what it did not use (GlossRoomLetGo). A node a forget
 * empties is kept spare, and spares past a few are freed with no lock held
 * (GlossTrim).
 *
 * Its lock, "gloss", is a leaf (lock_rank.h): taken alone by a read on any
 * core, and under the state lock by an Ink and by Fallow's gives; nothing is
 * taken under it.
 */

/* Nodes a writer holds for what it puts in, made before the lock. */
typedef struct {
    void*    nodes;     /* chained */
    uint32_t count;
} GlossRoom;

/* The nodes putting a digest of every block of `count` runs may want: OK, or
 * ERR_NO_MEMORY with what was had kept in *room for GlossRoomLetGo. With no
 * TagFS lock held. */
error_t GlossRoomFor(const Strip* runs, uint32_t count, GlossRoom* room);
void    GlossRoomLetGo(GlossRoom* room);

/* The digest of `block` put in from `room`, over any the Gloss held for it.
 * Under the state lock or with the door shut; the room is enough by
 * GlossRoomFor's count. */
void    GlossPut(GlossRoom* room, uint64_t block, uint64_t digest);

/* A digest the mount's replay met, in its order — the latest of a block
 * standing: OK, or ERR_NO_MEMORY. With no TagFS lock held. */
error_t GlossNote(uint64_t block, uint64_t digest);

/* The digest the Gloss holds for `block`: false when it holds none. */
bool    GlossFind(uint64_t block, uint64_t* digest);

/* The digests of [start, start + count), little-endian, into `out`: false
 * when the Gloss lacks one of them. */
bool    GlossCopy(uint64_t start, uint64_t count, uint8_t* out);

/* The digests of [start, start + count) taken out: the ground went back to the
 * allocator. Under the state lock. */
void    GlossForget(uint64_t start, uint64_t count);

/* Spare nodes past the few kept freed: with no TagFS lock held. */
void    GlossTrim(void);

/* Every digest let go, and the spare nodes: with the volume. */
void    GlossLetGo(void);

#ifdef CONFIG_VOLUME_TESTS
/* The digests the Gloss holds. */
uint64_t GlossStandCount(void);
#endif

#endif /* TAGFS_INTEGRITY_GLOSS_H */
