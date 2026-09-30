#ifndef TAGFS_INK_H
#define TAGFS_INK_H

#include "../tagfs.h"
#include "../plot/plot.h"

/*
 * Ink — the one way a file of the volume changes.
 *
 * Every change of a file's record — a write's new size and extents, a span, a
 * defrag, a new name or tag, a new file, a file given up — is one Ink, and an
 * Ink is a change of MEMORY: the record is laid in the pool's tail, the map of
 * live files names it (through the same door the mount puts records through,
 * PoolMirrorPut), the file table and the tag index follow, and what the file
 * stopped naming comes to rest in Fallow. Nothing reaches the medium here; a
 * Settle cycle (settle/) takes what was inked to it.
 *
 * ‼ UNDER THE VOLUME'S STATE LOCK, AND NOTHING THERE THAT CAN WAIT OR FAIL.
 * An Ink is made in two halves. Before the lock, everything it could need is
 * taken: the record built and packed, the memory the map and the tag index
 * lack for it, the node its freed ground rests in. Under the lock it checks
 * that the file is still what the first half read (its record's gen), that
 * the stakes still cover, and that the tail has room — or a Spare to close
 * onto — and only then, with nothing left that can refuse, does it change
 * anything. A refusal before that point changes nothing anywhere.
 *
 * ‼ AN UNSETTLED RECORD OF THE SAME FILE IS REPLACED, NOT FOLLOWED. A file
 * written a thousand times between two cycles leaves one record in the tail,
 * not a thousand: its record no cycle has taken is taken back (PoolTakeBack)
 * and the new one laid after the rest.
 *
 * ‼ NO SPARE IS WAITED FOR. A tail with no room needs a Spare to close onto,
 * and the last SPARE_RESERVE of them are a delete's. An Ink that has none it
 * may take makes one itself, with no state lock held (PoolSpareMake) — or,
 * where it cannot (an asynchronous write's continuation), answers
 * ERR_WOULD_BLOCK and the caller makes one its own way. It never waits for a
 * cycle to make one for it.
 *
 * ‼ WHAT THE LOADER READS BY ITS BLOCKS IS NOT TOUCHED (RT-09), AND THIS IS THE
 * ONE PLACE THAT SAYS SO. No Ink changes the extents of a file wearing a tag
 * whose key is system, boot or god, or holding the kernel the Deed points the
 * loader at, or gives such a file up, and no Ink takes one of those tags off a
 * file: ERR_PERMISSION_DENIED. A write, a span and a defrag ask the same guard
 * when they load the file (PlotLoad), before a byte of theirs moves; nobody
 * above the volume asks it again.
 *
 * ‼ AND NOTHING CHANGES A VOLUME WHOSE MEDIUM LEFT (RT-13). From the moment the
 * volume sees its medium gone every Ink is refused ERR_DEVICE_NOT_READY, and
 * so is every load for a change: memory that no cycle can take to the medium
 * is not written further.
 */

/* What an Ink leaves: `gen` is its place in the order of Inks, and a Settle
 * cycle that takes it — settled_gen >= gen — has put it on the medium. */
typedef struct {
    uint64_t gen;
} InkReceipt;

/* How an Ink meets a tail with no room and no Spare it may take: it makes one
 * itself (a caller holding no state lock), or answers ERR_WOULD_BLOCK (a
 * continuation, which may not stand over the medium) and the caller does. */
typedef enum {
    INK_MAKES_SPARES = 0,
    INK_ASKS_SPARES  = 1,
} InkSpares;

/* The Plot's size and extents become the file's record, with its name and tags
 * as they are now; the ground the Plot let go of rests in Fallow, every Stake
 * is the record's, and the room it claimed is let go. Called by the holder of
 * the file's Helm, which loaded the Plot under it. OK — and the Plot stands
 * for the record committed — or the refusal, the file as it was and the Stakes
 * still the Plot's: ERR_FILE_NOT_FOUND (the file was given up meanwhile),
 * ERR_BUSY (its extents were changed since the load, which only a caller that
 * did not hold the Helm can do — said), ERR_PERMISSION_DENIED,
 * ERR_DEVICE_NOT_READY, ERR_TAGFS_RECORD_FULL, ERR_NO_MEMORY, or a Spare's
 * refusal. */
error_t TagFSInkPlot(Plot* p, InkSpares spares, InkReceipt* out);

/*
 * RT-19. The room in its record the Plot's extents need, claimed before a byte
 * of the call moves: the record with its name and tags as they are now and the
 * Plot's extents must fit, or the call is refused ERR_TAGFS_RECORD_FULL — said
 * — with nothing written; and from here until the Plot is inked or let go, a
 * name or a tag inked for the file (InkFields) must leave that room, or is
 * refused itself. A write's record is full because of a tag, never after its
 * data landed. Nothing is claimed for a Plot loaded with no Helm, or one that
 * names no more extents than its record. Called with no TagFS lock held.
 */
error_t InkRoomClaim(Plot* p);

/* The room the Plot claimed let go; nothing when it claimed none. */
void    InkRoomLetGo(Plot* p);

/* A file's name and tags. `name` non-NULL renames it, and the name's tag
 * moves with it: the tag of the name it had comes off, `add` (the new name's,
 * already interned) goes on first. Otherwise `add` goes on and `drop` comes
 * off (TAGFS_INVALID_TAG_ID for neither). OK — a change that changes nothing
 * is OK with no Ink — or the refusal. */
error_t InkFields(uint32_t fid, const char* name, uint16_t add, uint16_t drop, InkReceipt* out);

/* A new file named `name`, wearing the `count` tags in `tags` (the name's
 * first, no two alike). OK and its id, or the refusal with no file made and no
 * trace of one — the id it would have taken is taken by the next. */
error_t InkNew(const char* name, const uint16_t* tags, uint32_t count, uint32_t* out_fid,
               InkReceipt* out);

/* The file given up: a Quitclaim laid for it, its extents to Fallow, and it
 * leaves the map, the table and the tag index. OK, or the refusal. */
error_t InkQuitclaim(uint32_t fid, InkReceipt* out);

/* Under the state lock: the newest Ink's gen, and the flush need its data
 * holds — the one a cycle's first flush must be issued past. */
uint64_t InkedGen(void);
uint64_t InkDataNeed(void);

/* A mount's first word: no Ink yet. With the door shut. */
void     InkMountReset(void);

/* The stem of a name as a key, read in place: OK with out->key NULL for a
 * name with no stem, ERR_TAGFS_TAG_TOO_LONG for one longer than a key. */
error_t InkNameStem(const char* name, TagSpelling* out);

/* The tag a file's name gives it — the stem, up to the last dot, as a key —
 * interned, and on the medium before any file wears it: OK and the id
 * (TAGFS_INVALID_TAG_ID for a name with no stem), or the registry's refusal.
 * With no TagFS lock held: a new tag is written to the medium. */
error_t InkNameTagIntern(const char* name, uint16_t* out_tag);

/* The tag a name gives, when the registry already has it; else invalid. */
uint16_t InkNameTagLookup(const char* name);

/* The one guard of a change to a file (see above), asked with the tags the file
 * wears and the runs it names: OK, ERR_DEVICE_NOT_READY once the medium left,
 * or ERR_PERMISSION_DENIED — said — for a file the loader reads by its blocks. */
error_t  InkMayChange(uint32_t fid, const uint16_t* tags, uint32_t tag_count,
                      const FileExtent* runs, uint32_t run_count);

#endif /* TAGFS_INK_H */
