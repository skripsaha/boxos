#ifndef TAGFS_H
#define TAGFS_H

#include "../../lib/kernel/ktypes.h"

/* A mounted volume, whole (volume/tagfs_volume.h): handed to every function
 * that touches one. The house holds every volume of the machine (volume/
 * house.c): TagFSHome is the one this kernel was booted from, number 0, and
 * the rest follow it in the order of their numbers (TagFSVolumeNext, from
 * NULL). TagFSHouseBegin begins the house before anything of TagFS runs. */
typedef struct TagFSVolume TagFSVolume;
void         TagFSHouseBegin(void);
TagFSVolume* TagFSHome(void);
TagFSVolume* TagFSVolumeNext(TagFSVolume* vol);
TagFSVolume* TagFSVolumeNumbered(uint32_t number);
uint32_t     TagFSVolumeNumber(const TagFSVolume* vol);
bool         TagFSVolumeMounted(const TagFSVolume* vol);

/* A file's id as the house names it — what every program holds: its volume's
 * number above the volume's own id of it (tagfs_constants.h,
 * TAGFS_VOLUME_FILE_BITS), so the home volume's files keep the ids their
 * volume gives. TagFSHouseId: the house's id of file `fid` of `vol`.
 * TagFSVolumeOfId: the mounted volume a house id names, and the volume's own
 * id in *fid — NULL when the house numbers no such volume, it is not
 * mounted, or the id is no file's (0, or a passport). */
uint64_t     TagFSHouseId(const TagFSVolume* vol, uint64_t fid);
TagFSVolume* TagFSVolumeOfId(uint64_t id, uint64_t* fid);

/* The volume of the house that calls itself `uuid` (its Deed's), mounted or
 * not; NULL when the house holds none. */
TagFSVolume* TagFSVolumeOfUuid(const uint8_t uuid[16]);

/* What the house says of one of its volumes (volume/house.c): its number;
 * whether it is the home volume, mounted, taking no change — under glass, or
 * its medium refusing writes —, away — its medium left, its book held open
 * (mount/away.h) —, on a removable medium; the sixteen bytes it calls itself;
 * while mounted how many bytes it is, how many of them no file holds, and
 * how many files it has; and its medium's own name, NUL-ended. */
typedef struct {
    uint32_t number;
    bool     home;
    bool     mounted;
    bool     read_only;
    bool     away;
    bool     removable;
    uint8_t  uuid[16];
    uint64_t bytes;
    uint64_t free_bytes;
    uint64_t files;
    char     medium[48];
} TagFSVolumeFacts;
void         TagFSVolumeFactsOf(TagFSVolume* vol, TagFSVolumeFacts* out);

/* The volumes of the house that stand on the disk in `seat` now — its present
 * occupant: each one's number and the sector its ground begins at, at most
 * `max` of them; how many there are. What a change of the disk's table may
 * not touch (DiskInUse, disk_table.h). */
uint32_t     TagFSVolumesOn(uint8_t seat, uint32_t* numbers, uint64_t* grounds, uint32_t max);

/* The sixteen bytes volume `vol` calls itself (its Deed's); zeros for the home
 * volume before it stands on one. */
void         TagFSVolumeUuid(const TagFSVolume* vol, uint8_t out[16]);

#include "../../lib/kernel/ktypes.h"
#include "../../lib/kernel/klib.h"
#include "tagfs_constants.h"
#include "plot/strip.h"
#include "../../include/boxos_magic.h"
#include "../../include/boxos_limits.h"
#include "../../include/volume_deed.h"
#include "../core/error/error.h"
#include "lock_rank/lock_rank.h"
#include "scribe/scribe.h"
#include "tag_spelling/tag_spelling.h"

// ============================================================================
// Include Submodule Headers (production implementations)
// ============================================================================

#include "block_map/block_map.h"
#include "deed/deed.h"
#include "mount/under_glass.h"

/*
 * Every constant of the volume is in tagfs_constants.h, once — this header
 * used to repeat a dozen of them beside it, so a change to one copy left the
 * other standing.
 *
 * Where the volume is, and what it is made of, is not written here either.
 *
 * It was: a superblock at absolute sector 1034, its backup at 1035, the
 * DiskBook's head at 1036 — three numbers spelled out in this header, in
 * tagfs_constants.h, in the UEFI loader and in stage2's assembly, each copy
 * holding the others in place by comment alone. A volume could exist in
 * exactly one place on any medium.
 *
 * Now the medium's partition table says where the ground is (ground.h), and
 * the Deed at the head of that ground says what is on it (volume_deed.h).
 * Every number the volume holds is counted from its start, so the volume can
 * be anywhere and can be moved.
 */

// ----------------------------------------------------------------------------
// In-Memory Structures: Tag Registry
// ----------------------------------------------------------------------------

/*
 * ‼ A TAG IS ITS TEXT AND WHOSE IT IS.
 *
 * Every tag has an owner. The person's tags are the labels people put on
 * files to find them again — and the language Touch, the processes and the
 * Use Context speak, which the person's Use Context is matched against. A
 * program's own tags are its labels for itself: a game's `save` is not the
 * person's `save`, and the two are two tags with two ids, so neither ever
 * meets the other's files. A program is who it is as no other program can
 * be: a signed one its passport — its author and its name, as its box's
 * signature proves them (box_trust.h), the same program in every file and
 * every version of it —, any other the file it was started from (its fid),
 * the one name of it the volume vouches for. A passport carries bit 63
 * (TAGFS_PASSPORT_BIT), which no file's id does. TAG_OWNER_PROGRAM is the
 * book's TALLY_OWNER_PROGRAM.
 */
#define TAG_OWNER_PERSON   0u
#define TAG_OWNER_PROGRAM  1u

typedef struct {
    uint8_t  kind;   /* TAG_OWNER_PERSON or TAG_OWNER_PROGRAM */
    uint64_t id;     /* the program's passport, or its fid; 0 for the person */
} TagOwner;

static inline TagOwner TagOwnerPerson(void)
{
    return (TagOwner){ TAG_OWNER_PERSON, 0 };
}

static inline TagOwner TagOwnerProgram(uint64_t fid)
{
    return (TagOwner){ TAG_OWNER_PROGRAM, fid };
}

static inline bool TagOwnerSame(TagOwner a, TagOwner b)
{
    return a.kind == b.kind && a.id == b.id;
}

/* The owner as the deck's answers name it (boxos_decks.h): 0 the person, a
 * program the file it is named by (TagFSProgramCard) — who it is, while it is
 * no more on the volume and its tags not yet retired. */
uint64_t TagOwnerWord(TagFSVolume* vol, TagOwner owner);

/*
 * Whose tags a list lets in. One owner's (`all` false: `owner`), or every
 * owner its asker may see (`all`): the asker's own — `reader`, its program's
 * file, 0 for none —, those of the programs that show their tags to it
 * (`shown`, rising, `nshown` of them), and with `everyone` the person's and
 * every program's: the person's eyes (storage_tags.c).
 */
typedef struct {
    bool            all;
    bool            everyone;
    TagOwner        owner;
    uint64_t        reader;
    const uint64_t* shown;
    uint32_t        nshown;
} TagView;

static inline TagView TagViewOf(TagOwner owner)
{
    return (TagView){ .all = false, .owner = owner };
}

/* Whether `view` lets `owner`'s tags in. */
static inline bool TagViewLets(const TagView* view, TagOwner owner)
{
    if (!view->all)
        return TagOwnerSame(owner, view->owner);
    if (view->everyone)
        return true;
    if (owner.kind != TAG_OWNER_PROGRAM)
        return false;
    if (view->reader && owner.id == view->reader)
        return true;
    uint32_t lo = 0, hi = view->nshown;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (view->shown[mid] < owner.id) lo = mid + 1u;
        else                             hi = mid;
    }
    return lo < view->nshown && view->shown[lo] == owner.id;
}

/* key and value are NUL-terminated copies, their lengths kept beside them so
 * a record is packed from what was measured once, never cut by a cast. */
typedef struct TagRegistryEntry {
    char*                    key;
    char*                    value;
    struct TagRegistryEntry* id_next;    /* the next entry in its bucket of `by_id` */
    uint16_t                 value_len;
    uint8_t                  key_len;
    uint8_t                  flags;
    uint32_t                 tag_id;
    TagOwner                 owner;
    uint64_t                 retired_gen;   /* RETIRED: the Ink the book holds it retired from — 0 for one
                                             * the book already holds retired, TAG_RETIRING_OPEN while the
                                             * files are still being cleared of it */
} TagRegistryEntry;

/* A tag whose retirement has begun and is not yet laid in a stitch the book
 * settled: retired_gen is TAG_RETIRING_OPEN until its files are clear of it. */
#define TAG_RETIRING_OPEN  UINT64_MAX

typedef struct TagRegistryNode {
    uint32_t                tag_id;
    struct TagRegistryNode* next;
} TagRegistryNode;

/* Every tag of one key and one owner. Its inside is tag_registry.c's alone: a
 * key group's array is freed by the next tag of its key, so nothing outside
 * the registry's lock may hold a pointer into one (MP-D11) — TagRegistryKeyIds
 * copies. */
typedef struct TagKeyGroup TagKeyGroup;

/* The three hashes — tags by their owner and text, tags by their id, and an
 * owner's key's every tag — start at TAGFS_REG_BUCKETS and TAGFS_KEY_BUCKETS buckets and double as
 * they fill: a new table is built outside the lock and swapped in under it.
 * The id's hash holds the entries that exist and nothing for an id no tag has,
 * so it costs what the registry holds, never the highest id it ever gave; it
 * has as many buckets as the text's, both holding every tag. */
typedef struct {
    TagRegistryNode** buckets;
    uint32_t          bucket_count;

    TagRegistryEntry** by_id;

    uint32_t total_tags;     /* every id the registry knows: the tags it holds and the ones retired */
    uint64_t text_bytes;     /* the bytes every tag the book has not yet laid retired carries past its fixed
                              * fields: key, value, owner */
    uint32_t program_tags;   /* the tags of programs whose retirement is not yet finished */
    uint32_t next_id;

    TagKeyGroup** key_buckets;
    uint32_t      key_bucket_count;
    uint32_t      key_groups;

    TagFSLock lock;

    /* Every id below coined_next is in the book — its Tally's TAGS or a settled stitch's Coined — and a
     * cycle lays the ones above down as Coineds (settle/sewing.c), coined_text bytes. Under `lock`. */
    uint32_t          coined_next;
    uint64_t          coined_text;    /* what the ribbon's room holds for them (settle/ribbon.c, RibbonHeld) */
    bool              said_full;      /* every id issued: said once, the first time a tag is refused */

    /* The ids retired since the last stitch that settled their Coined RETIRED, each wearing its
     * retired_gen: a stitch lays one when the Ink it was retired at is among its own (settle/sewing.c).
     * Under `lock`. */
    uint32_t*         retiring;
    uint32_t          retiring_count;
    uint32_t          retiring_cap;
} TagRegistry;

// ----------------------------------------------------------------------------
// In-Memory Structures: the Indexes of the Live Files
// ----------------------------------------------------------------------------

typedef struct {
    uint32_t* ids;
    uint32_t  count;
    uint32_t  capacity;
} TagIdList;

/* Who wears each tag (tag_bitmap/tag_bitmap.c), and the files by id with the
 * newest record of each (file_map/). Both hold the live files and nothing for
 * an id no live file has. */
typedef struct TagBitmapIndex TagBitmapIndex;
typedef struct FileMap        FileMap;

/* One page of the live files a question matches, in file id order. The ids
 * go into the caller's room and no further; nothing is cut silently: `next`
 * says where the rest begin, and `total` how many there are. */
typedef struct {
    uint32_t delivered;   /* ids written into the caller's room */
    uint32_t total;       /* every match from the page's first id on — counted
                           * only when the caller asked for it, 0 otherwise */
    uint64_t next;        /* the first match past the room: ask again from it;
                           * 0 when there is none */
} TagFSPage;

#include "tagfs_handle.h"   /* the Helm, the open-file row and the handle */

// ----------------------------------------------------------------------------
// In-Memory Structures: Global State
// ----------------------------------------------------------------------------

/*
 * What the mounted volume is, split the way the medium splits it.
 *
 * `geometry` comes out of the Deed and never changes while the volume is
 * mounted; its book — FRAME, Spine and Tally — is the mount's (mount/
 * book_mount.h). `free_blocks` and `total_files` are what the book says of it
 * today, kept in step by every Ink and every claim of the block map; the next
 * file id is the book's own (vol->book.next_fid).
 *
 * ‼ A BLOCK NUMBER IS A BLOCK OF THE VOLUME
 *
 * Every number the book or a file's record names is a block of the VOLUME
 * (volume_deed.h, VolumeFrame), up to `block_end` — where the volume's far
 * Deed sits, the end of what a file may name. One conversion takes it to a
 * sector of the medium, and only one: tagfs_block_to_sector().
 */
typedef struct {
    uint8_t          uuid[16];      /* which volume this is */
    char             volume_name[16]; /* Deed's short maker/name, NUL-padded */
    uint64_t         volume_bytes;  /* whole mounted volume capacity */
    uint64_t         block_end;     /* the first block no record may name */
    VolumeGeometry   geometry;      /* what it was laid out for */
    VolumeBoot       boot;          /* where the loader finds the kernel; zero when the Deed says nothing */
    uint64_t         free_blocks;   /* blocks of the volume no owner holds */
    uint64_t         total_files;   /* live files */

    TagRegistry*     registry;
    TagBitmapIndex*  bitmap_index;
    FileMap*         files;

    /* The Braid the volume is assembled in, which the mount names — the only
     * thing that sends a block of the volume through Braid (braid/braid.h). */
    struct BraidState* braid;

    bool             initialized;
    TagFSLock        lock;
} TagFSState;

// ----------------------------------------------------------------------------
// Tag Registry API
// ----------------------------------------------------------------------------

int          tag_registry_init(TagRegistry* reg);
void         tag_registry_destroy(TagRegistry* reg);

/* The id of `owner`'s tag, issued when the registry has none yet. The widths
 * are judged before anything is allocated (ERR_TAGFS_TAG_TOO_LONG); a registry
 * that has issued every id refuses with ERR_TAGFS_REGISTRY_FULL and says so
 * once; ERR_NO_MEMORY when memory ran out. */
error_t      TagRegistryIntern(TagRegistry* reg, TagOwner owner, const TagSpelling* tag, uint32_t* out_id);
uint32_t     TagRegistryLookup(TagRegistry* reg, TagOwner owner, const TagSpelling* tag);
void         tag_registry_mark_system(TagRegistry* reg, uint32_t tag_id);

/* A tag's key and value as spans into the registry's own copies, whether the
 * volume calls it its own, and whose it is (`owner` when not NULL). A tag's
 * text never changes and is freed only with the registry, so the spans hold
 * for as long as the registry does. */
bool         TagRegistrySpelling(TagRegistry* reg, uint32_t tag_id, TagSpelling* out,
                                 bool* is_system, TagOwner* owner);

/* The id of every tag of `owner`'s whose key is the key_len bytes at `key`,
 * COPIED: out->ids is the caller's to kfree, never the registry's own array.
 * No such key: NULL and 0. */
error_t      TagRegistryKeyIds(TagRegistry* reg, TagOwner owner, const char* key, size_t key_len,
                               TagIdList* out);

// ----------------------------------------------------------------------------
// Bitmap Index API
// ----------------------------------------------------------------------------

/* An empty index; NULL when there is no memory. */
TagBitmapIndex* tag_bitmap_create(void);
void            tag_bitmap_destroy(TagBitmapIndex* idx);
/* The file put under the tag: 0, or -1 — said once, with the index's numbers
 * — when the index had no memory for it, and then nothing changed. */
int             tag_bitmap_set(TagBitmapIndex* idx, uint32_t tag_id, uint64_t file_id);
bool            TagBitmapHas(TagBitmapIndex* idx, uint32_t tag_id, uint64_t file_id);

/* An add of `file_id` under each of `count` distinct tags, in two halves so the
 * volume's state lock never waits on the heap. TagBitmapStakeFor takes, with
 * no TagFS lock held, every piece of memory the adds lack now: OK (*out NULL
 * when they lack nothing), or ERR_NO_MEMORY — said — with nothing taken. Under
 * the state lock, TagBitmapStakeCovers puts the pieces in and answers whether
 * the adds now lack nothing — false when the index changed shape since the
 * stake, and then nothing is added and the caller stakes again — and
 * TagBitmapAddCovered makes the adds, which cannot fail once covered. What was
 * not spent, and what the pieces replaced, go with TagBitmapStakeLetGo, after
 * the lock. */
typedef struct TagBitmapStake TagBitmapStake;
error_t         TagBitmapStakeFor(TagBitmapIndex* idx, const uint32_t* tags, uint32_t count,
                                  uint64_t file_id, TagBitmapStake** out);
bool            TagBitmapStakeCovers(TagBitmapIndex* idx, const uint32_t* tags, uint32_t count,
                                     uint64_t file_id, TagBitmapStake* stake);
void            TagBitmapAddCovered(TagBitmapIndex* idx, const uint32_t* tags, uint32_t count,
                                    uint64_t file_id);
void            TagBitmapStakeLetGo(TagBitmapStake* stake);

/* The file out from under the tag with nothing allocated, for a caller under
 * the volume's state lock: a window that falls to where an array would be
 * lighter stays a bitmap until TagBitmapShrink. */
void            TagBitmapLeave(TagBitmapIndex* idx, uint32_t tag_id, uint64_t file_id);
/* After a TagBitmapLeave of `file_id` from each of the `count` tags, with no
 * TagFS lock held: each window the file left that fell to where an array is
 * lighter made one again, and the table of tags made smaller when the tags
 * with members left fill too little of it — the storage taken with no lock
 * held and put in under `state`, the volume's state lock, under which alone
 * the index changes shape. What the heap gives nothing for stays as it was,
 * which costs room and nothing else, until the next leave. */
void            TagBitmapShrink(TagBitmapIndex* idx, TagFSLock* state, const uint32_t* tags,
                                uint32_t count, uint64_t file_id);
/* The kernel heap the index holds. */
uint64_t        TagBitmapBytes(TagBitmapIndex* idx);
/* The files from `from` on that wear every one of `tag_ids`, at least one
 * tag of each of `groups`, and none of `unless`, in id order: up to `room`
 * into `out`, and the page says the rest (TagFSPage). 0, or -1 for no index. */
int             TagBitmapQueryPage(TagBitmapIndex* idx,
                    const uint32_t* tag_ids, uint32_t tag_count,
                    const TagIdList* groups, uint32_t group_count,
                    const uint32_t* unless, uint32_t unless_count,
                    uint64_t from, uint64_t* out, uint32_t room,
                    bool want_total, TagFSPage* page);
#ifdef CONFIG_VOLUME_TESTS
/* The most steps one hold of the index's lock gave a query, since it was last
 * asked — which starts it again; and the most a hold may give one whose
 * `unless_count` tags exclude. */
uint32_t        TagBitmapQueryMost(TagBitmapIndex* idx);
uint32_t        TagBitmapQueryHoldBound(uint32_t unless_count);
#endif

// ----------------------------------------------------------------------------
// Main TagFS API
// ----------------------------------------------------------------------------

error_t  tagfs_init(TagFSVolume* vol);
uint8_t  tagfs_get_seat(TagFSVolume* vol);       /* Boardroom seat the volume lives on */

/*
 * ‼ THE DOOR. Step inside the mounted volume before touching anything it
 * brought up, and step out afterwards.
 *
 * A volume can be taken down under a running machine — its medium leaves, and
 * what is held about it is dropped and read again — and everything the mount
 * allocated goes back to the allocator at that moment: the tag registry, the
 * tag index, the block map, the book's memory. Nothing is
 * freed while a caller is inside, and no caller gets in once the volume is on
 * its way out. False means there is no volume to be inside; every caller that
 * gets true MUST leave.
 *
 * Every tagfs_* operation below takes this door itself. It is public because
 * one thing more is reachable from outside — a caller that wants to hold
 * something of the volume's across several calls.
 */
bool     tagfs_enter(TagFSVolume* vol);
void     tagfs_leave(TagFSVolume* vol);

/*
 * Which mounting of a volume this is. Bumped every time one is taken up, so
 * anything remembered across a re-read — a file handle, an async read in
 * flight — can be told that what it remembers is from a different volume.
 */
uint32_t tagfs_mount_epoch(TagFSVolume* vol);
bool     tagfs_handle_is_of_this_mount(TagFSVolume* vol, const TagFSFileHandle* handle);

/*
 * The service point, called from the guide loop and the idle loop — one atomic
 * load per duty when there is nothing to do. Here the work of a Helm handed to
 * a waiter that does it synchronously is run (HelmRunReady), and a volume that
 * has been taken down, which leaves its memory alone until the last caller is
 * out, is finished — for every volume of the house, and a medium that brought
 * volumes the house did not hold is searched for them (volume/house.c).
 */
void     TagFSServiceIfPending(void);

/*
 * The volumes' share of the loops (settle/cadence.c). Whether this core's loop
 * owes any volume of the house a turn before it sleeps: Helm work listed for
 * a service point, or a Settle's step. On one core, whether the idle context
 * is owed the next quantum (RT-10) — Helm work listed, or a cycle owed now —
 * read by the scheduler from the tick, lock-free; one volume's, for the
 * stand.
 */
bool     TagFSStepOwedHere(void);
bool     TagFSIdleOwed(void);
bool     TagFSVolumeIdleOwed(TagFSVolume* vol);

/* Nightwatch's look at the Settle, from the PIT tick (RT-11). */
void     TagFSWatchLook(void);

/*
 * A program's own changes of the volumes (settle/cadence.c). An Ink's place
 * is its gen in one order across every mount of its volume this boot, so a
 * place of a mount before this one is told apart; a program keeps the newest
 * place of its own on each volume it changed (VolumeInkDebt). TagFSCallBegins:
 * a call of a program is about to be served on this core — the guide serves
 * it without giving the core away — and its number is returned; every Ink the
 * core makes from here is marked with it, on its volume. TagFSCallEnds: that
 * call served — the newest Ink it made on each volume, if it made any, is the
 * program's; a call that read made none. TagFSInkPlace: the place of a
 * mount's Ink `gen`, inside the door. TagFSNoteInker: the program made the
 * Ink at `place` of `vol` — the newest is kept. TagFSInkerLeaves: the program
 * is leaving (settle.h) — on each volume where a change of its own is not yet
 * settled, the next cycle is demanded rather than left to Lull.
 */
struct process_t;
typedef struct VolumeInkDebt {
    uint32_t              volume;   /* its number in the house; never the home volume's, kept in the strand */
    volatile uint64_t     place;
    struct VolumeInkDebt* next;
} VolumeInkDebt;
uint64_t TagFSCallBegins(void);
void     TagFSCallEnds(struct process_t* proc, uint64_t call);
uint64_t TagFSInkPlace(TagFSVolume* vol, uint64_t gen);
void     TagFSNoteInker(struct process_t* proc, TagFSVolume* vol, uint64_t place);
void     TagFSInkerLeaves(struct process_t* proc);

/* Memory came back to the machine — a cabin's, torn down: a Settle cycle
 * refused for want of it is looked at again (settle/settle_wait.c). From any
 * context. */
void     TagFSHeapCameBack(TagFSVolume* vol);

/* The same event for everything on every volume's road that waits for it:
 * the Settle's cycle (TagFSHeapCameBack), and a mount the machine had no
 * memory for, whose service pass is knocked awake. From any context. */
void     TagFSMemoryCameBack(void);

/*
 * Where this volume's ground begins on that medium, in the medium's own
 * sectors. Zero when nothing is mounted.
 *
 * The one absolute address TagFS holds. It is not written down inside the
 * volume — it is discovered from the medium's partition table each time the
 * volume is met — so anybody who needs to talk about the SAME run of sectors
 * from outside has to be told it. The boot survey is the caller: it walks every
 * ground on every medium, and this is how it recognises the one that has
 * already been read, checked and described by the mount.
 */
uint64_t tagfs_get_volume_base(TagFSVolume* vol);

/*
 * A medium has arrived somewhere in the Boardroom.
 *
 * Either the volume this kernel had is back — the same identity, and its
 * open book taken up when it is as this kernel left it or given up when it is
 * not (mount/away.h), both read off the medium rather than assumed — or
 * nothing was ever mounted and this is the first medium that could carry a
 * volume. Called from a context that is allowed to wait, because deciding
 * takes reads.
 */
/*
 * Listen for media arriving and leaving, instead of being called about them.
 * Set up once, before the boot mount is attempted — a medium that turns up
 * while that mount is running is announced to whoever is subscribed at that
 * moment. Idempotent.
 */
void     TagFSWatchSeats(void);

/* The arrivals the room announced while the boot mount ran, attended once it
 * is settled: by every volume of the house, and the media searched for the
 * volumes it does not hold. */
void     TagFSAttendArrival(void);

/* A read of the volume the medium refused — said by the mount's readers, so a
 * mount that fails on a medium that did not answer is asked again when the
 * seat answers (seat:ready), and one refused for what it holds is not. */
void     TagFSNoteReadRefused(TagFSVolume* vol);

/* The medium under the volume may have left; ask now rather than at the next
 * read, because by then the seat can hold something else. */
void     TagFSNoteMediumGone(TagFSVolume* vol);

/* Whether the medium under the mounted volume has left (RT-13): from then on no
 * Ink changes the volume — ERR_DEVICE_NOT_READY — nobody new waits in a Helm's
 * FIFO, and the volume's book is held open until an event decides it
 * (mount/away.h): taken up when the same volume comes back as it was left,
 * else given up and the volume let go of. TagFSMediumGone asks the room first
 * — an empty chair, or one holding another medium than the volume was mounted
 * from — and latches a departure it finds: asked before every run of blocks
 * the mounted volume reads or writes (run/run.c), and by a caller whose medium
 * command was refused, who must tell a medium that left from one that failed. */
bool     TagFSMediumLeft(TagFSVolume* vol);
bool     TagFSMediumGone(TagFSVolume* vol);

/* Whether the mounted volume takes no write: its medium refused one as
 * protected (RT-18) — DATA PROTECT on USB, an ATA abort on AHCI — or the
 * volume stands under glass (TagFSIsUnderGlass). From then on no Ink changes
 * the volume — ERR_TAGFS_READ_ONLY — and every waiter for a cycle hears the
 * same. A protection lasts until the volume is let go of, or its book is
 * taken up, the medium seated again and asked anew: TagFSWriteRefused asks
 * the seat after a write the medium refused, and latches it. Glass lasts
 * for the whole mount. */
bool     TagFSReadOnly(TagFSVolume* vol);
void     TagFSWriteRefused(TagFSVolume* vol);

/* Said once, by whoever tried the boot mount, whether or not it succeeded.
 * Before it, an arriving medium belongs to the boot that is still happening. */
void     TagFSBootMountSettled(void);
/* The machine is stopping, and its last Settle cycle has run (settle.h,
 * TagFSSettleForHalt): the door shut, and what that cycle answered. */
error_t tagfs_shutdown(TagFSVolume* vol);

/* Flush the volatile write cache of the disk that holds the TagFS volume
 * (routes to the probed AHCI port / ATA drive, not a hardcoded device). The
 * one place a flush ticket is taken and answered (scribe/scribe.h). */
error_t  tagfs_flush_cache(TagFSVolume* vol);

/* Convert a TagFS logical block number to its absolute disk-sector LBA.
 * Used by the storage deck's reads and writes nobody stands over, which reach
 * the seat (BoardroomReadAsync, BoardroomWriteAsync) outside the block
 * helpers. */
uint64_t tagfs_block_to_sector(TagFSVolume* vol, uint64_t block);

/* There is no tagfs_format() here. A volume is made by the tool that lays it
 * out on ground it read from a partition table (tools/create_tagfs_ground.c),
 * and by nothing else. The kernel used to carry a second formatter that
 * nothing called — so nothing ever ran it, nothing ever checked it, and it
 * went on writing the layout the rest of the kernel had already left behind. */

/* Each change of a file below answers -ERR_WOULD_BLOCK, nothing of it done —
 * an untag by text, nothing of what its text still names — while the volume
 * is short of room a Settle cycle gives back: room on the ribbon for the
 * change's stitch. No cycle is run beneath the call: its caller waits for one
 * its own way and asks again (settle_ops.h, SettleOpForRoom). */

/* 0, or a negative -ERR_*: a name whose tag the registry refuses (too long,
 * the registry full) is refused with the registry's code, no file is made
 * without the tag its name gives it, and a volume that has issued every file
 * id it can is refused ERR_TAGFS_FID_EXHAUSTED (ink_book.c). `made_by` is the
 * volume's id of the file of the program that makes the file — the program
 * of the cabin that asked, CabinProgramFile — or 0 when none does (the
 * kernel itself, a program started from bytes): the file's record remembers
 * it for good (TAGFS_FILE_MADE_BY). */
int  tagfs_create_file(TagFSVolume* vol, const char* filename, const uint32_t* tag_ids, uint32_t tag_count, uint64_t made_by,
                       uint64_t* out_file_id);

/* The file given up, or renamed — its old name's tag off, the new one's on.
 * 0, or a negative -ERR_*: a file the system keeps in a role is given up only
 * with the warrant (-ERR_PERMISSION_DENIED); the file holding the kernel the
 * Deed points the loader at is given up by nobody. A rename asks none: its
 * name's tag is a tag like any other. */
int  tagfs_delete_file(TagFSVolume* vol, uint64_t file_id, InkWarrant warrant);
/* A file deleted for a term (keep/keep.h): kept `term_seconds` from now, no
 * query meeting it, and let go by the volume when the term ends — or, for a
 * term of 0, given up at once, as tagfs_delete_file does. The term is the
 * caller's: the volume has none of its own. A file the system keeps is
 * deleted only with the warrant, as it is given up. tagfs_restore_file brings a deleted file back
 * until its term ends (-ERR_FILE_NOT_FOUND after, or for a file the volume
 * does not hold), and is no change to a file that is not deleted. */
int  tagfs_keep_file(TagFSVolume* vol, uint64_t file_id, uint64_t term_seconds, InkWarrant warrant);
int  tagfs_restore_file(TagFSVolume* vol, uint64_t file_id, InkWarrant warrant);
int  tagfs_rename_file(TagFSVolume* vol, uint64_t file_id, const char* new_filename);

/*
 * The three changes of a file's extents — a write, span and defrag — are made
 * by the holder of the file's Helm (helm/helm.h). A program reaches them
 * through the storage deck, which parks it while another holds the Helm
 * (write_job.c). These are for a caller in the kernel, which cannot be parked
 * and does not spin: each takes the Helm only when it is free, and answers
 * ERR_BUSY, said, when another holds it. A caller in the kernel carries no
 * warrant (InkWarrant): a file the system keeps is refused it.
 */

/* span — the file made exactly `size` bytes long. Shorter: the tail is cut and
 * its blocks given back. Longer: the bytes from the old size to the new one
 * read as zeros, written to blocks taken for them — never a previous owner's
 * bytes — a window at a time, each inked. The one way a file grows past its
 * size other than by being written at its end. OK, or the refusal: one before
 * the first window changes nothing of the file, one part way keeps the windows
 * inked before it; a growth past every block the volume could give is refused
 * ERR_TAGFS_NO_SPACE before it takes one. */
error_t tagfs_span_file(TagFSVolume* vol, uint64_t file_id, uint64_t size);

/* A tag put on a file, or taken off: 0, or the Ink's refusal as -ERR_*. No
 * tag asks a warrant: a file's tags are its user's. */
int  tagfs_add_tag(TagFSVolume* vol, uint64_t file_id, uint32_t tag_id);
int  tagfs_remove_tag(TagFSVolume* vol, uint64_t file_id, uint32_t tag_id);
bool tagfs_has_tag(TagFSVolume* vol, uint64_t file_id, uint32_t tag_id);

/* `owner`'s tag a spelling names, interned and put on the file, both inside
 * one mounting of the volume: OK, or the real refusal — the registry's (the
 * tag too long, the registry full), or the Ink's: ERR_FILE_NOT_FOUND,
 * ERR_DEVICE_NOT_READY, ERR_TAGFS_READ_ONLY, ERR_NO_MEMORY;
 * ERR_TAGFS_NOT_INITIALIZED with no volume. */
error_t tagfs_add_tag_string(TagFSVolume* vol, TagOwner owner, uint64_t file_id, const TagSpelling* tag);
/* What `owner`'s spelling names comes off the file: that tag when it has a
 * value, every tag of its key when it is bare — `owner`'s, never another's of
 * the same text. 0 once one of them came off, or a
 * negative -ERR_*: -ERR_TAGFS_TAG_NOT_FOUND when the file wore none of them,
 * -ERR_FILE_NOT_FOUND when there is no such file, or the first refusal of
 * taking one off — -ERR_DEVICE_NOT_READY, -ERR_TAGFS_READ_ONLY or
 * -ERR_NO_MEMORY. */
int  tagfs_remove_tag_string(TagFSVolume* vol, TagOwner owner, uint64_t file_id, const TagSpelling* tag);

bool tagfs_key_is_reserved(const char *key, size_t key_len);

/*
 * The live files a question matches, a page at a time and in file id order
 * (query/query.c): every file wearing all of `tags` and none of `without`
 * ("key:..." is any tag of the key, in either list), both of them `owner`'s
 * tags and never another's of the same text — a question of no tags is
 * every file but those `without` names, and of neither every file,
 * TagFSListPage. Every tag is a tag like any other: which files are set aside
 * is the asker's to say. The one files the volume leaves out of its own accord
 * are those it keeps deleted (TAGFS_FILE_DELETED): a question never meets one,
 * and TagFSQueryKeptPage is the question that asks for them alone.
 * From file id `from` on, up to `room` ids into `out`; the page says where the
 * rest begin and, when `want_total`, how many match from `from` on. OK, or the
 * refusal: ERR_TAGFS_NOT_INITIALIZED with no volume, ERR_NO_MEMORY when the
 * question could not be put to the index.
 */
error_t  TagFSQueryPage(TagFSVolume* vol, TagOwner owner, const TagSpelling* tags, uint32_t count, const TagSpelling* without,
                        uint32_t without_count, uint64_t from, uint64_t* out, uint32_t room,
                        bool want_total, TagFSPage* page);
error_t  TagFSListPage(TagFSVolume* vol, uint64_t from, uint64_t* out, uint32_t room, bool want_total,
                       TagFSPage* page);
/* The same question asked of the files kept deleted alone: those that wear all
 * of `tags` and none of `without`, a question of neither every one of them. */
error_t  TagFSQueryKeptPage(TagFSVolume* vol, TagOwner owner, const TagSpelling* tags, uint32_t count, const TagSpelling* without,
                            uint32_t without_count, uint64_t from, uint64_t* out, uint32_t room,
                            bool want_total, TagFSPage* page);
/*
 * The live files called `word` — its `length` bytes are the whole of the
 * file's name or its stem, the name up to its last dot — that wear all of
 * `tags` and none of `without`, `owner`'s tags (as TagFSQueryPage reads them;
 * none asked: every file so called), a page at a time and in file id order,
 * as TagFSQueryPage gives them. A name is no tag and no one's: every asker
 * meets every file so called, and the question is answered by the name index
 * (file_map/name_index.h), kept exact by every change to a file. The files
 * kept deleted are not met. ERR_INVALID_ARGUMENT for a word of no bytes; the
 * rest as TagFSQueryPage.
 */
error_t  TagFSNamedPage(TagFSVolume* vol, TagOwner owner, const char* word, size_t length, const TagSpelling* tags, uint32_t count,
                        const TagSpelling* without, uint32_t without_count, uint64_t from, uint64_t* out,
                        uint32_t room, bool want_total, TagFSPage* page);
/* The live files whose first bytes begin with `bytes`, `length` of them (1 to
 * TAGFS_HEAD_MOST) — what a file is is told by them, and each file's record
 * keeps them exact (FileRecord.head) — a page at a time in file id order, as
 * TagFSQueryPage gives them; a file kept deleted is not met. No file's bytes
 * are read: the records answer. ERR_INVALID_ARGUMENT for no bytes or more
 * than a record keeps; ERR_TAGFS_NOT_INITIALIZED with no volume. */
error_t  TagFSStartingWithPage(TagFSVolume* vol, const uint8_t* bytes, uint32_t length, uint64_t from, uint64_t* out, uint32_t room,
                               bool want_total, TagFSPage* page);
/* The words a name answers to, as the name index takes them: the whole name
 * (lengths[0], its `length`) and, when the name has a dot with something
 * before it, its stem — the bytes up to the last dot (lengths[1]). How many
 * words there are: 2, 1 for a name with no stem (`kernel`, `.profile`), 0 for
 * a name of no bytes. Every word starts at `name`. */
uint32_t TagFSNameWords(const char* name, size_t length, size_t lengths[2]);
/*
 * The tags of `view` files wear, a page at a time and in the order of their
 * numbers (query/query.c): from the tag numbered `from` on, every tag `view`
 * lets in at least one live file wears — of one owner's, one of the files
 * that wear all of `within` and none of `without`, that owner's tags, as
 * TagFSQueryPage asks; a view of every owner asks neither
 * (ERR_INVALID_ARGUMENT) — with how many of them do and whose it is, written into
 * `out` as STORAGE_TAG_LIST records (boxos_decks.h) while they fit its `room`
 * bytes. A tag no such file wears — a process's, a Touch's, a Bay's, which the
 * registry names too — is passed over. *used the bytes written, *delivered the
 * records, *next the number to ask from for the rest, 0 when there are none.
 * OK, or the refusal: ERR_TAGFS_NOT_INITIALIZED with no volume, ERR_NO_MEMORY,
 * or ERR_BUFFER_TOO_SMALL when not even the first record fits.
 */
error_t  TagFSTagPage(TagFSVolume* vol, const TagView* view, const TagSpelling* within, uint32_t within_count,
                      const TagSpelling* without, uint32_t without_count, uint32_t from, uint8_t* out,
                      uint32_t room, uint32_t* used, uint32_t* delivered, uint32_t* next);
/* The file id the next create takes — no id of this volume has reached it.
 * 0 when no volume is mounted. */
uint64_t tagfs_file_ceiling(TagFSVolume* vol);

/* The file opened for one operation: NULL when it is not there, when it has
 * no record in memory, when it is locked (book_mount.h, TagFSFileLocked), or when the handle
 * or its place among the file's open handles could not be had. A handle
 * opened to read holds the record by reference; one opened only to write
 * holds none (TagFSFileHandle). */
TagFSFileHandle* tagfs_open(TagFSVolume* vol, uint64_t file_id, uint32_t flags);
void             tagfs_close(TagFSVolume* vol, TagFSFileHandle* handle);

/* Up to `size` bytes from the handle's offset into the kernel's `buffer`: OK
 * and *out_read — 0 at or past the file's size — or the refusal with
 * *out_read the bytes that landed before it. The refusal is the real one: a
 * block the medium would not give (ERR_READ_FAILED), a medium that left
 * (ERR_DEVICE_NOT_READY), a block that does not hold what was written to it
 * (ERR_CHECKSUM), a part of the file inside its size that no extent covers
 * (ERR_METADATA_CORRUPTED). The offset moves by what landed. */
error_t          tagfs_read(TagFSVolume* vol, TagFSFileHandle* handle, void* buffer, uint64_t size,
                            uint64_t* out_read);

/* The same read into wherever `sink` says — a program's pages, a block at a
 * time as the read reaches them (read/read.h) — up to the sink's room. */
struct ReadSink;
error_t          TagFSReadInto(TagFSVolume* vol, TagFSFileHandle* handle, const struct ReadSink* sink,
                               uint64_t* out_read);

/* The source's bytes written at the handle's offset — at the file's size when
 * the handle was opened TAGFS_HANDLE_APPEND. An offset past the size is
 * refused ERR_TAGFS_BEYOND_SIZE: a file grows by being written at its end, or
 * by span, never across a gap. OK with *out_written the whole source, or the
 * refusal with *out_written the bytes the file now holds of it — a prefix is
 * kept when the medium stopped taking the rest. The offset moves by what was
 * written, and the handle's file_size is the size the write left. */
struct PlotSource;
error_t          tagfs_write(TagFSVolume* vol, TagFSFileHandle* handle, const struct PlotSource* src,
                             uint64_t* out_written);

int         tagfs_get_metadata(TagFSVolume* vol, uint64_t file_id, TagFSMetadata* out);
/* What tagfs_get_metadata filled in, let go. */
void        tagfs_metadata_free(TagFSMetadata* meta);
TagFSState* tagfs_get_state(void);

/* The file laid in one run, taken near where it begins, its record naming the
 * run in as few extents as a record can say it. OK, or the refusal: the file
 * is left where it was. A file the system keeps — in a role by its record, or
 * holding the kernel the Deed points at — is not moved, warrant or none
 * (plot.h, PlotGather): ERR_PERMISSION_DENIED. */
error_t  tagfs_defrag_file(TagFSVolume* vol, uint64_t file_id);
/* How fragmented the volume's files are: the share, in percent, of the steps
 * from one of a file's blocks to its next that are a gap — the next run not
 * beginning where the one before it ends — rounded up, so one gap never reads
 * as none. The live files counted and the gaps found are said through
 * out_files and out_gaps when they are not NULL. */
uint32_t tagfs_get_fragmentation_score(TagFSVolume* vol, uint32_t* out_files, uint32_t* out_gaps);
#ifdef CONFIG_VOLUME_TESTS
/* One record's share of that score, as the walk of the live files counts it:
 * the gaps a read of it seeks across, and the steps between its blocks of
 * ground — for the stand, which lays records with holes of its own. */
void     TagFSFragOfRecord(const struct FileRecord* rec, uint32_t* gaps, uint64_t* steps);
#endif

// ----------------------------------------------------------------------------
// Block I/O (for subsystem use) — a run of blocks in as few commands as the
// seat takes is run/run.c's (TagFSReadRun below, TagFSWriteRun there)
// ----------------------------------------------------------------------------

/* One block: OK, or the real refusal — ERR_DEVICE_NOT_READY once the medium
 * left, ERR_READ_FAILED when it would not give the block, ERR_BAD_SECTOR when
 * it answered that a sector of it cannot be read, ERR_CHECKSUM when
 * what it gave does not match the digest the book holds for the block
 * (integrity/integrity.h, which names the block and publishes it); a write
 * ERR_DEVICE_NOT_READY, ERR_WRITE_FAILED, or ERR_TAGFS_READ_ONLY under glass. */
error_t  tagfs_read_block(TagFSVolume* vol, uint64_t block, void* buffer);
error_t  tagfs_write_block(TagFSVolume* vol, uint64_t block, const void* buffer);

/* `count` blocks from `first` on, in as few commands as the seat takes and
 * checked against nothing: for a reader that checks each block where it hands
 * it out — the read-ahead's fetch, a defrag's copy. A volume assembled in a
 * Braid gives them a block at a time. OK, ERR_DEVICE_NOT_READY or
 * ERR_READ_FAILED. */
error_t  TagFSReadRun(TagFSVolume* vol, uint64_t first, uint32_t count, void* buffer);

/* A write landed on `block`: no read-ahead window hands out what it held
 * before (read-after-write). tagfs_write_block says so itself; a writer that
 * goes to the medium another way — the asynchronous WriteJob, the Use
 * Context's chain — says so after each block. */
void     tagfs_readahead_invalidate(TagFSVolume* vol, uint64_t block);

/*
 * Read one sector run of the mounted volume, counted from the start of its
 * ground rather than from the start of the medium.
 *
 * This is the door TagFS and everything under it uses to reach the medium,
 * and a block of the volume goes by tagfs_block_to_sector. The base is applied in those two and nowhere else,
 * which is what makes "no absolute addresses inside a volume" an invariant
 * instead of a convention.
 */
int      tagfs_volume_read (TagFSVolume* vol, uint64_t vlba, uint32_t count, void* buffer);
int      tagfs_volume_write(TagFSVolume* vol, uint64_t vlba, uint32_t count, const void* buffer);

/*
 * Tags by name, for everything outside TagFS.
 *
 * The registry is the volume's, it is freed when the volume is read again, and
 * it used to be handed out as a pointer to seven other files. These are what
 * those files actually wanted: an id for a name, a name for an id, and a copy
 * of the name rather than a pointer into the volume. Each takes the door for
 * the length of its answer, so none of them can be holding the registry when
 * it goes. TAGFS_INVALID_TAG_ID / false is the answer when there is no volume.
 * A tag's text is read by the one spelling (tag_spelling.h). These speak the
 * person's tags — the language Touch, the processes, Bay, Brook and the Use
 * Context share — and never a program's own (TagOwner).
 */
uint32_t tagfs_tag_lookup(TagFSVolume* vol, const char* tag);
uint32_t TagFSLookupSpelling(TagFSVolume* vol, const TagSpelling* tag);

/* The id, issued when the volume has none: OK, the spelling's or the
 * registry's refusal, or ERR_TAGFS_NOT_INITIALIZED when there is no volume. */
error_t  tagfs_tag_intern(TagFSVolume* vol, const char* tag, uint32_t* out_id);
error_t  TagFSInternSpelling(TagFSVolume* vol, const TagSpelling* tag, uint32_t* out_id);

/* The same for `owner`'s tag — a program's own, or the person's — for the
 * storage deck, which speaks for whichever its caller names (boxos_decks.h,
 * STORAGE_TAGS_*). The person's alone are words of the Use Context, and only
 * they are bound to it. */
error_t  TagFSInternOwned(TagFSVolume* vol, TagOwner owner, const TagSpelling* tag, uint32_t* out_id);

/* The whole text of a tag id — "app:editor" — and its length. It is written
 * into `out` only when it fits whole, NUL and all, and `out` is left empty
 * otherwise, so a caller is never handed a shorter tag than the one it asked
 * for: a caller that must have it measures first (out NULL, size 0). 0 when
 * the volume has no such tag, or there is no volume. */
size_t   tagfs_tag_text(TagFSVolume* vol, uint32_t tag_id, char* out, size_t out_size);

/* Whether program `owner` (TagOwner) is on volume `vol`, so that its own
 * tags there are its: a signed program's passport — the same program on every
 * volume — while a file of it is there, live or kept deleted; a program named
 * by its file, which names it on that file's volume alone, while the volume
 * holds that file, holding no signed program since. False with the volume not
 * mounted. */
bool     TagFSHoldsProgram(TagFSVolume* vol, uint64_t owner);

/* Who a program is, as its own tags are owned (TagOwner): a signed program
 * its passport (box_trust.h, bit 63 set), any other the file it is. Programs
 * name programs by a file of them, and these speak between the two: the
 * program file `fid` holds — the passport its record keeps, else `fid`
 * itself, 0 for a file the volume has not —; the file program `owner` is
 * named by — its own, or of a passport the least live file that keeps it,
 * else the least kept deleted —; and every file of it, live or kept deleted,
 * rising: how many, the first `room` written to `out`. A program that is no
 * more on the volume has none: 0, as with no volume. */
uint64_t TagFSProgramOf(TagFSVolume* vol, uint64_t fid);
uint64_t TagFSProgramCard(TagFSVolume* vol, uint64_t owner);
uint32_t TagFSProgramFiles(TagFSVolume* vol, uint64_t owner, uint64_t* out, uint32_t room);

/* The card of file `fid` laid with who the program it holds is, once one is
 * started from it: the passport its signature gives it, or 0 for a program
 * with no signature that holds — one Ink when the card says another, none
 * when it says the same (InkPassport). */
error_t  TagFSPassportLay(TagFSVolume* vol, uint64_t fid, uint64_t passport);

/* Whether tag `tag_id` of the home volume is one of the person's
 * (TagOwner): false for a program's own, an id the volume has not, and with
 * no volume. The home volume's tag ids are the machine's language — what the
 * processes, Touch and the Use Context speak. */
bool     TagFSTagIsPersons(uint32_t tag_id);

/*
 * A program's own tags shown to other programs (show/show.c). A program
 * shows all its tags, those to come too, to another program or to every
 * program (reader 0); one shown them sees them, finds the files by them and
 * lists them, and never puts them on nor takes them off. Whom a program shows
 * them to is the cards of its files (FileRecord.shown_to) — every file of a
 * signed program holds the one list, and a card that comes to be one of its
 * takes it (InkPassport) — so the last file let go takes it with it, and a
 * reader that left is taken out of every list by the volume's duty
 * (retire/retire.c). Who may ask which is the caller's to judge
 * (storage_tags.c): a program changes its own list alone. A program is asked
 * of here as who it is (TagOwner) and named in every list by its card
 * (TagFSProgramCard), as programs name programs.
 */

/* Program `program`'s files show its tags to `reader` (0: every program), or
 * — `show` false — no longer do. OK, and OK changing nothing for what stands
 * already; ERR_INVALID_ARGUMENT for program 0 or a program shown to itself;
 * ERR_FILE_NOT_FOUND for a program, or a reader shown to, the volume has not,
 * and a program kept deleted, every file of it, that would show to one more;
 * the Ink's refusal; ERR_TAGFS_NOT_INITIALIZED with no volume. */
error_t  TagFSShowTags(TagFSVolume* vol, uint64_t program, uint64_t reader, bool show);

/* Whether `program` shows its tags to `reader` — by name, or to every
 * program. False for a program the volume has not, and with no volume. */
bool     TagFSShows(TagFSVolume* vol, uint64_t program, uint64_t reader);

/* The programs that show their tags to `reader` — by name or to every
 * program, `reader` itself left out — a page at a time in the order of their
 * cards, from `from` on (TagFSPage). ERR_TAGFS_NOT_INITIALIZED with no volume. */
error_t  TagFSShownToPage(TagFSVolume* vol, uint64_t reader, uint64_t from, uint64_t* out, uint32_t room, bool want_total,
                          TagFSPage* page);

/* The programs `program` shows its tags to, rising, 0 first for every
 * program: *out the list — the caller's to kfree, NULL for none — *count how
 * many. ERR_FILE_NOT_FOUND for a program the volume has not, ERR_NO_MEMORY. */
error_t  TagFSShownBy(TagFSVolume* vol, uint64_t program, uint64_t** out, uint32_t* count);

/* Program `asker` asks program `owner` to show it its tags: a note the
 * kernel holds (show/show.h) until `owner` answers — TagFSShowTags of the
 * asker either way, or of every program — or either leaves. *noted true for
 * a note left now; OK leaving none when one stands already, or `owner` shows
 * its tags to the asker already. ERR_INVALID_ARGUMENT for program 0 or one
 * asking itself; ERR_FILE_NOT_FOUND for a program the volume has not;
 * ERR_NO_MEMORY. */
error_t  TagFSAskToSee(TagFSVolume* vol, uint64_t asker, uint64_t owner, bool* noted);

/* The programs that asked `owner` to show them its tags and are not answered
 * yet, a page at a time in the order of their cards (TagFSPage); OK, or
 * ERR_NO_MEMORY. */
error_t  TagFSAsksPage(TagFSVolume* vol, uint64_t owner, uint64_t from, uint64_t* out, uint32_t room, bool want_total,
                       TagFSPage* page);

/* Whether tag `tag_id` of the home volume — the machine's language, as a
 * process wears its tags — is one of the person's a pattern names
 * (TagPatternMatches), read in place in its registry: a pattern is spoken in
 * the person's tags, and a program's own is never one it names. */
bool     TagFSTagMatches(uint32_t tag_id, const TagSpelling* pattern);


/* A tag's key and value as spans into the volume's registry, whether the
 * volume calls the tag its own, and whose it is (`owner` when not NULL) — no
 * copy, for a caller that is INSIDE THE DOOR (tagfs_enter) and reads them
 * before it leaves: the registry is freed only once the last caller is out. */
bool     TagFSTagSpelling(TagFSVolume* vol, uint32_t tag_id, TagSpelling* out, bool* is_system, TagOwner* owner);

// Accessor Functions
// ============================================================================

TagFSState* tagfs_get_state(void);

// ============================================================================
// Test Framework
// ============================================================================

#include "tests/tests.h"

// Run all tests (called from userspace)
error_t TagFS_RunTests(void);

#endif // TAGFS_H
