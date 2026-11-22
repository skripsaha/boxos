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
 * Ink is a change of MEMORY: the map of live files names the new record, the
 * Docket keeps the change for the next stitch (docket.h), the tag index
 * follows, and what the file stopped naming comes to rest in Fallow. Nothing
 * reaches the medium here; a Settle cycle (settle/) takes what was inked to
 * it.
 *
 * ‼ UNDER THE VOLUME'S STATE LOCK, AND NOTHING THERE THAT CAN WAIT OR FAIL.
 * An Ink is made in two halves. Before the lock, everything it could need is
 * taken: the record built, the memory the map and the tag index lack for it,
 * the node its freed ground rests in, the room on the ribbon its stitch's
 * record takes (settle/ribbon.c, RibbonClaim). Under the lock it checks that
 * the file is still what the first half read (its record's gen) and that the
 * stakes still cover, and only then, with nothing left that can refuse, does
 * it change anything. A refusal before that point changes nothing anywhere.
 *
 * ‼ AN UNSETTLED CHANGE OF THE SAME FILE IS MERGED, NOT FOLLOWED. A file
 * written a thousand times between two cycles leaves one Jot in the Docket,
 * not a thousand (docket.h).
 *
 * ‼ NO ROOM IS WAITED FOR. A volume with no room for the change while a Settle
 * cycle would give some back answers ERR_WOULD_BLOCK, nothing changed, and the
 * caller waits for that cycle its own way.
 *
 * ‼ A FILE THE SYSTEM KEEPS IS CHANGED ONLY WITH THE WARRANT, AND WHAT THE
 * LOADER READS BY ITS BLOCKS BY NOBODY (RT-09); THIS IS THE ONE PLACE THAT
 * SAYS SO. A file whose record puts it in a role — system, boot or kernel
 * (TAGFS_FILE_ROLE_*, bits of its flags) — has its extents changed and is given
 * up only by a call that carries the warrant (InkWarrant, tagfs.h: its
 * program's cabin holds system or god). The role is the record's and no
 * tag's: a file's tags are its user's, whatever their keys, and any tag goes
 * on or comes off any file, one the system keeps among them, warrant or none.
 * A file holding the kernel the Deed points the loader at is changed by no
 * call at all, warrant or none. Either refusal is ERR_PERMISSION_DENIED, said.
 * A write, a span and a defrag ask the same guard when they load the file
 * (PlotLoad), before a byte of theirs moves; nobody above the volume asks it
 * again.
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

/* The Plot's size and extents become the file's record, with its name and tags
 * as they are now; the ground the Plot let go of rests in Fallow, and every
 * Stake is the record's. Called by the holder of
 * the file's Helm, which loaded the Plot under it, and guarded with the
 * warrant it was loaded with. OK — and the Plot stands for the record
 * committed — or the refusal, the file as it was and the Stakes still the
 * Plot's: ERR_FILE_NOT_FOUND (the file was given up meanwhile), ERR_BUSY (its
 * extents were changed since the load, which only a caller that did not hold
 * the Helm can do — said), ERR_PERMISSION_DENIED, ERR_DEVICE_NOT_READY,
 * ERR_TAGFS_READ_ONLY, ERR_NO_MEMORY, or ERR_WOULD_BLOCK while the room its
 * stitch needs is on its way. */
error_t TagFSInkPlot(Plot* p, InkReceipt* out);

/* A file's name and tags. `name` non-NULL renames it — the name is a field of
 * the file's card, and no tag moves with it. `add` goes on and `drop` comes
 * off (TAGFS_INVALID_TAG_ID for neither). No tag asks a warrant, whatever its
 * key: a file's tags are its user's, and what the system keeps a file as is
 * its record's role (TAGFS_FILE_ROLE_*), which no tag puts on or takes off.
 * OK — a change that changes nothing is OK with no Ink — or the refusal. */
error_t InkFields(uint64_t fid, const char* name, uint32_t add, uint32_t drop, InkReceipt* out);

#ifdef CONFIG_VOLUME_TESTS
/* The stand's way to keep a file in a role, as the image builder lays one
 * (tools/create_tagfs_cargo.c): the record's role bits become `role`
 * (TAGFS_FILE_ROLE_*) by an ordinary Ink, the tags as they are. The tail is
 * laid in the file's block first, under its Helm (helm/engross.h,
 * EngrossRoled), as the loader reads a file of a role by its blocks and no
 * write lays a Slip on one (plot_slip.c). No program asks it: the system has
 * no call that puts a role on a file the volume already holds. */
error_t InkRoleLaid(uint64_t fid, uint32_t role, InkReceipt* out);
#endif

/* A book's file rolled back by the mount's verdict (mount/errata.h): its
 * record marked TAGFS_FILE_UNDONE by an ordinary Ink, its name and tags as
 * they are — the system's own word on the file, never one of its tags. OK, and
 * OK with no Ink for one marked already, or the refusal an Ink of a file's
 * fields answers. */
error_t InkUndone(uint64_t fid, InkReceipt* out);

/* A book's file deleted and kept — the record marked TAGFS_FILE_DELETED, the
 * time it is kept until, `until` (a book's nanoseconds), in the record beside
 * it — by an ordinary Ink, its name and tags as they are; or, for `until` 0,
 * brought back: the mark and the time taken off. Deleting a deleted file moves
 * its term. The guard of a change asks the warrant, as a delete does: a file
 * the system keeps is not deleted without one. OK — and OK with no Ink for a
 * call that changes nothing — or the refusal. Only the system lets a kept file
 * go at its term (keep/keep.h): InkQuitclaim, as ever. */
error_t InkKeep(uint64_t fid, uint64_t until, InkWarrant warrant, InkReceipt* out);

/* The `count` tags of `tags` taken off file `fid` — live or kept deleted — in
 * one Ink, those it wears; OK, and OK with no Ink for a file that wears none
 * of them, or the refusal: ERR_FILE_NOT_FOUND, the volume's, the ribbon's room
 * (ERR_WOULD_BLOCK while it can come), ERR_NO_MEMORY. */
error_t InkTagsOff(uint64_t fid, const uint32_t* tags, uint32_t count, InkReceipt* out);

/* The `count` tags of `tags` retired — closed already (TagRegistryRetire) and
 * worn by no file: each lies in a Coined RETIRED of the next stitch. OK, or
 * ERR_BUSY while a file wears one — the Ink that took it off is yet to come —
 * or the refusal of room for the Coineds (ERR_WOULD_BLOCK while it can come),
 * of the volume, or ERR_NO_MEMORY. Inside the door. */
error_t InkTagsRetire(const uint32_t* tags, uint32_t count, InkReceipt* out);

/* A new file named `name`, wearing the `count` tags in `tags` (the name's
 * first, no two alike), made by the program whose file is `made_by` — 0 when
 * no program of the volume makes it. OK and its id, or the refusal with no
 * file made and no trace of one — the id it would have taken is taken by the
 * next. */
error_t InkNew(const char* name, const uint32_t* tags, uint32_t count, uint64_t made_by, uint64_t* out_fid,
               InkReceipt* out);

/* The file given up: a Quitclaim kept for the next stitch, its extents to
 * Fallow, and it leaves the map and the tag index. OK, or the refusal — the
 * guard's among them, asked with `warrant` — and ERR_WOULD_BLOCK, nothing
 * changed, while the room for its Quitclaim is on its way: room a cycle gives
 * back. */
error_t InkQuitclaim(uint64_t fid, InkWarrant warrant, InkReceipt* out);

/*
 * Conjugate's Ink (ink_conjugate.c; the act is dedup/conjugate.h): book file
 * `fid`'s blocks [at, at + count) — `dup`, its DATA ground there, in the
 * file's order — replaced in its record by `onto`, the ground the target file
 * `onto_fid` names over as many blocks, every Strip of it SHARED: a Shaped of
 * one splice, the file's size and times as they were. Before the lock the
 * record, the Fallow node of the ground let go and the room Commonhold's
 * change takes; under it the file held to the record the act compared (`cur`,
 * sitting at `was`) and the target to the Ink of the one it compared
 * (`onto_gen`) — the guards the act asked of both stand only while neither
 * changed — then the record put in the mirror and its Jot in the Docket, one
 * owner off every block let go, which rests when that leaves none, and one
 * owner more on every block shared (commonhold.h). OK and the receipt;
 * ERR_BUSY, nothing changed, when either file changed since the act compared
 * it, ERR_FILE_NOT_FOUND when either was given up; or the volume's refusal —
 * ERR_DEVICE_NOT_READY, ERR_TAGFS_READ_ONLY, the ribbon's room
 * (ERR_WOULD_BLOCK while it can come), ERR_NO_MEMORY.
 */
typedef struct {
    uint64_t          fid;
    const FileRecord* cur;
    RecordWhere       was;
    uint64_t          at;
    uint64_t          count;
    const Strip*      dup;
    uint32_t          ndup;
    const Strip*      onto;
    uint32_t          nonto;
    uint64_t          onto_fid;
    uint64_t          onto_gen;
} InkConjugation;

error_t InkConjugate(const InkConjugation* c, InkReceipt* out);

/* Whether the volume takes a change not begun yet (ink.c): OK, or why not —
 * a medium that left, one that refuses writes, a stopping machine's halt. */
error_t InkVolumeBegins(void);

/* Under the state lock: the newest Ink's gen, and the flush need its data
 * holds — the one a cycle's first flush must be issued past. */
uint64_t InkedGen(void);
uint64_t InkDataNeed(void);

/* The Ink that laid file `fid`'s newest record — every change the file has had
 * is in it — or the newest Ink of all for a file no longer in the map, whose
 * Quitclaim is among them. With no lock; inside the door. */
uint64_t InkGenOf(uint64_t fid);

/* A mount's first word: no Ink yet, and no change waiting for a stitch of a
 * book read before it (docket.h) — and on a book, the ground its ribbon let go
 * laid to rest in Fallow (M-1). With the door shut. */
void     InkMountReset(void);

/* The volume let go of: what Ink keeps of it — Fallow's ground at rest and
 * the Docket's changes no stitch took — let go, nothing given back. With the
 * door shut. */
void     InkDropAll(void);

/* The volume takes changes again: its book taken up, the medium seated anew —
 * whether it refuses writes is the next refused write's to ask — or the stand
 * undoing a protected medium it armed itself (BlockFailProtect). */
void     InkReadOnlyLift(void);

/* Whether a book takes the change about to be made, asked before the `count`
 * tags of `owner`'s it brings are interned: refused by a volume that takes no change (read
 * only, its medium gone, the halt begun), or where its ribbon has no room for
 * the Coineds of the ones the registry does not hold yet — ERR_WOULD_BLOCK
 * while room can come, ERR_TAGFS_NO_SPACE when none can (settle/sewing.h) —
 * so a refused change interns nothing, and two do not both take the room one
 * has: OK holds those Coineds' bytes on the ribbon (*held) until the tags hold
 * their ids and the registry counts them, InkBookHeldLetGo letting the hold go
 * after the interning, inside the same stay behind the door. */
error_t InkBookRefuses(TagOwner owner, const TagSpelling* tags, uint32_t count, uint64_t* held);
void    InkBookHeldLetGo(uint64_t held);

/* The one guard of a change to a file (see above), asked with the flags the
 * file's record carries, the runs it names and the caller's warrant: OK,
 * ERR_DEVICE_NOT_READY once the medium left or a stopping machine's halt shut
 * the volume to changes not begun (TagFSShutForHalt), ERR_TAGFS_READ_ONLY once
 * it refused a write as protected, or ERR_PERMISSION_DENIED — said — for a file
 * the system keeps in a role and a caller with no warrant, and for the kernel
 * the loader reads by its blocks and any caller. */
error_t  InkMayChange(uint64_t fid, uint32_t flags, const Strip* runs, uint32_t run_count,
                      InkWarrant warrant);

/* The guard's two questions on their own, for a mover of blocks that asks them
 * whatever the warrant (PlotGather): whether the record's flags put the file in
 * a role the system keeps — system, boot or kernel — and whether any of the
 * runs holds a block of the kernel the Deed points the loader at. */
static inline bool InkFileKept(uint32_t flags)
{
    return (flags & TAGFS_FILE_ROLE_MASK) != TAGFS_FILE_ROLE_ORDINARY;
}
bool     InkTouchesBoot(const Strip* runs, uint32_t count);

/* The second question asked of a file's record where it holds its runs —
 * walked, never laid flat, so a guard asks the heap nothing: whether any of
 * its DATA Strips holds a block of the kernel the Deed points the loader at. */
bool     InkRecordTouchesBoot(const FileRecord* rec);

#endif /* TAGFS_INK_H */
