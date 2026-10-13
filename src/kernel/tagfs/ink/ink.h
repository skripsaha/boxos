#ifndef TAGFS_INK_H
#define TAGFS_INK_H

#include "../tagfs.h"
#include "../plot/plot.h"
#include "../file_map/file_map.h"

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
 * and each change leaves the pool its floor of the reserve (meta_pool.h) —
 * the whole of it while the volume has a free block; with none, a record that
 * frees nothing still the whole of it, a Quitclaim of a file with no ground
 * Glean's share, a change that frees ground the close its ground needs — and
 * below Glean's share none takes room the close or Glean's next step needs. An
 * Ink that has no Spare it may take makes one itself, with no state lock held
 * (PoolSpareMake) — or, where it cannot (an asynchronous write's
 * continuation), answers ERR_WOULD_BLOCK and the caller makes one its own way.
 * It never waits for a cycle to make one for it: a volume with no ground for
 * one while a Settle cycle would give some back answers ERR_WOULD_BLOCK as
 * well, nothing changed, and the caller waits for that cycle its own way.
 *
 * ‼ A FILE THE SYSTEM KEEPS IS CHANGED ONLY WITH THE WARRANT, AND WHAT THE
 * LOADER READS BY ITS BLOCKS BY NOBODY (RT-09); THIS IS THE ONE PLACE THAT
 * SAYS SO. A file wearing a tag whose key is system, boot or god — system:foo
 * as much as system — has its extents changed, is given up, and has one of
 * those tags taken off only by a call that carries the warrant (InkWarrant,
 * tagfs.h: its program's cabin holds system or god); and only such a call puts
 * one of those tags on a file, any file — put on by anybody, it would lock the
 * file away from its own owner. Any other tag goes on any file, one the system
 * keeps among them, warrant or none. A file holding the kernel
 * the Deed points the loader at is changed by no call at all, warrant or none.
 * Either refusal is ERR_PERMISSION_DENIED, said. A write, a span and a defrag
 * ask the same guard when they load the file (PlotLoad), before a byte of
 * theirs moves; nobody above the volume asks it again.
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
 * the file's Helm, which loaded the Plot under it, and guarded with the
 * warrant it was loaded with. OK — and the Plot stands for the record
 * committed — or the refusal, the file as it was and the Stakes still the
 * Plot's: ERR_FILE_NOT_FOUND (the file was given up meanwhile), ERR_BUSY (its
 * extents were changed since the load, which only a caller that did not hold
 * the Helm can do — said), ERR_PERMISSION_DENIED, ERR_DEVICE_NOT_READY,
 * ERR_TAGFS_READ_ONLY, ERR_TAGFS_RECORD_FULL, ERR_NO_MEMORY, or a Spare's
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
 * off (TAGFS_INVALID_TAG_ID for neither). A tag of key system, boot or god
 * goes on — a name's tag too — or comes off only with the warrant. OK — a
 * change that changes nothing is OK with no Ink — or the refusal. */
error_t InkFields(uint64_t fid, const char* name, uint32_t add, uint32_t drop, InkWarrant warrant,
                  InkReceipt* out);

/* A new file named `name`, wearing the `count` tags in `tags` (the name's
 * first, no two alike). OK and its id, or the refusal with no file made and no
 * trace of one — the id it would have taken is taken by the next. */
error_t InkNew(const char* name, const uint32_t* tags, uint32_t count, uint64_t* out_fid,
               InkReceipt* out);

/* The file given up: a Quitclaim laid for it, its extents to Fallow, and it
 * leaves the map, the table and the tag index. OK, or the refusal — the guard's
 * among them, asked with `warrant` — and ERR_WOULD_BLOCK, nothing changed,
 * while the room for its Quitclaim is on its way: ground a cycle gives back,
 * or a sweep of Glean's (glean.h), which a volume with no block to make a
 * Spare from waits for rather than refuse the delete. */
error_t InkQuitclaim(uint64_t fid, InkWarrant warrant, InkReceipt* out);

/*
 * Glean's move (glean.h), under the state lock: the record at `from` — its
 * file's newest, as the caller saw under this same hold of the lock — laid
 * again at the tail's end, its bytes `packed` as they are, by an Ink of its
 * own: the map names it there, the file's row and the links' counts follow,
 * and its shape is the file's still. The tail has the room. Nothing refuses.
 */
void    InkGleanLocked(uint64_t fid, const RecordWhere* from, const uint8_t* packed);

/* Under the state lock: the newest Ink's gen, and the flush need its data
 * holds — the one a cycle's first flush must be issued past. */
uint64_t InkedGen(void);
uint64_t InkDataNeed(void);

/* The Ink that laid file `fid`'s newest record — every change the file has had
 * is in it — or the newest Ink of all for a file no longer in the map, whose
 * Quitclaim is among them. With no lock; inside the door. */
uint64_t InkGenOf(uint64_t fid);

/* A mount's first word: no Ink yet. With the door shut. */
void     InkMountReset(void);

/* The volume takes changes again: its book taken up, the medium seated anew —
 * whether it refuses writes is the next refused write's to ask — or the stand
 * undoing a protected medium it armed itself (BlockFailProtect). */
void     InkReadOnlyLift(void);

/* The stem of a name as a key, read in place: OK with out->key NULL for a
 * name with no stem, ERR_TAGFS_TAG_TOO_LONG for one longer than a key. */
error_t InkNameStem(const char* name, TagSpelling* out);

/* The tag a file's name gives it — the stem, up to the last dot, as a key —
 * interned, and on the medium before any file wears it: OK and the id
 * (TAGFS_INVALID_TAG_ID for a name with no stem), or the registry's refusal.
 * With no TagFS lock held: a new tag is written to the medium. */
error_t InkNameTagIntern(const char* name, uint32_t* out_tag);

/* The tag a name gives, when the registry already has it; else invalid. */
uint32_t InkNameTagLookup(const char* name);

/* The one guard of a change to a file (see above), asked with the tags the file
 * wears, the runs it names and the caller's warrant: OK, ERR_DEVICE_NOT_READY
 * once the medium left or a stopping machine's halt shut the volume to changes
 * not begun (TagFSShutForHalt), ERR_TAGFS_READ_ONLY once it refused a write as
 * protected, or ERR_PERMISSION_DENIED — said — for a file the system keeps and
 * a caller with no warrant, and for the kernel the loader reads by its blocks
 * and any caller. */
error_t  InkMayChange(uint64_t fid, const uint32_t* tags, uint32_t tag_count,
                      const Strip* runs, uint32_t run_count, InkWarrant warrant);

/* The guard's two questions on their own, for a mover of blocks that asks them
 * whatever the warrant (PlotGather): whether any of the tags has the key
 * system, boot or god, and whether any of the runs holds a block of the kernel
 * the Deed points the loader at. */
bool     InkWearsGuarded(const uint32_t* tags, uint32_t count);
bool     InkTouchesBoot(const Strip* runs, uint32_t count);

#endif /* TAGFS_INK_H */
