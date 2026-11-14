#ifndef TAGFS_H
#define TAGFS_H

#include "../../lib/kernel/ktypes.h"
#include "../../lib/kernel/klib.h"
#include "tagfs_constants.h"
#include "plot/plot_width.h"
#include "plot/strip.h"
#include "../../include/boxos_magic.h"
#include "../../include/boxos_limits.h"
#include "../../include/volume_deed.h"
#include "../../include/volume_ledger.h"
#include "../core/error/error.h"
#include "lock_rank/lock_rank.h"
#include "scribe/scribe.h"
#include "tag_spelling/tag_spelling.h"

// ============================================================================
// Include Submodule Headers (production implementations)
// ============================================================================

#include "ledger/ledger.h"
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
 * Now the medium's partition table says where the ground is (ground.h), the
 * Deed at the head of that ground says what is on it (volume_deed.h), and the
 * Ledger beside it says what is true of it today (volume_ledger.h). Every
 * number in both is counted from the start of the volume, so the volume can
 * be anywhere and can be moved.
 */

// ============================================================================
// On-Disk Structures
// ============================================================================

// Header: 4+4+2+2+4 = 16 bytes. data[4080] = 4096 total.
// Entries are packed variable-length records in data[]:
//   uint16_t tag_id | uint8_t flags | uint8_t key_len | uint16_t value_len
//   char key[key_len] | char value[value_len]  (value absent if value_len==0)
typedef struct __packed {
    uint32_t magic;
    uint32_t next_block;
    uint16_t entry_count;
    uint16_t used_bytes;
    uint8_t  reserved[4];
    uint8_t  data[TAGFS_REGISTRY_DATA_SIZE];
} TagRegistryBlock;

STATIC_ASSERT(sizeof(TagRegistryBlock) == 4096, "TagRegistryBlock must be 4096 bytes");

typedef struct __packed {
    uint32_t meta_block;
    uint32_t meta_offset;
} FileTableEntry;

STATIC_ASSERT(sizeof(FileTableEntry) == 8, "FileTableEntry must be 8 bytes");

// Header: 4+4+4+4 = 16 bytes. entries[510] * 8 = 4080. Total = 4096.
typedef struct __packed {
    uint32_t       magic;
    uint32_t       next_block;
    uint32_t       entry_count;
    uint32_t       reserved;
    FileTableEntry entries[TAGFS_FTABLE_PER_BLOCK];
} FileTableBlock;

STATIC_ASSERT(sizeof(FileTableBlock) == 4096, "FileTableBlock must be 4096 bytes");

// Header: 4+4+2+2+2+2 = 16 bytes. payload[4080] = 4096 total.
// Records are packed variable-length in payload[] — their widths and their
// packing are Plot's (plot/plot_width.h, PackV1).
//
// `record_count` counts every record ever appended here; `kept` counts the ones
// that carry a file id. The two words are a pair on purpose — Keep is already
// this project's word for what is still held (Chit: Give / Due / Keep) — and a
// kernel before the pool took only appends retired a record by zeroing its
// file_id in place, so the two drifted apart as files were deleted. What a
// block still holds of the live files is the map of live files' to say, not
// the header's: Glean (glean/) lays those records again at the tail and takes
// the block off the chain, and a file's row follows its record.
//
// It sits in two of the four bytes this header set aside and kept for nothing,
// so the block is the same 4096 bytes it always was and mkfs, the kernel and
// tools/volume-read.py all still find the payload at byte 16. The count is
// never believed on its own: the mount walk scans the records and uses what it
// counts, exactly as the free-block count is derived from the bitmap rather
// than remembered. An old volume carries zero here and is not harmed by it.
typedef struct __packed {
    uint32_t magic;
    uint32_t next_block;
    uint16_t used_bytes;
    uint16_t record_count;
    uint16_t kept;
    uint8_t  reserved[2];
    uint8_t  payload[TAGFS_MPOOL_DATA_SIZE];
} MetaPoolBlock;

STATIC_ASSERT(sizeof(MetaPoolBlock) == 4096, "MetaPoolBlock must be 4096 bytes");

// ----------------------------------------------------------------------------
// In-Memory Structures: Tag Registry
// ----------------------------------------------------------------------------

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
} TagRegistryEntry;

typedef struct TagRegistryNode {
    uint32_t                tag_id;
    struct TagRegistryNode* next;
} TagRegistryNode;

/* Every tag of one key. Its inside is tag_registry.c's alone: a key group's
 * array is freed by the next tag of its key, so nothing outside the registry's
 * lock may hold a pointer into one (MP-D11) — TagRegistryKeyIds copies. */
typedef struct TagKeyGroup TagKeyGroup;

/* The three hashes — tags by their text, tags by their id, and a key's every
 * tag — start at TAGFS_REG_BUCKETS and TAGFS_KEY_BUCKETS buckets and double as
 * they fill: a new table is built outside the lock and swapped in under it.
 * The id's hash holds the entries that exist and nothing for an id no tag has,
 * so it costs what the registry holds, never the highest id it ever gave; it
 * has as many buckets as the text's, both holding every tag. */
typedef struct {
    TagRegistryNode** buckets;
    uint32_t          bucket_count;

    TagRegistryEntry** by_id;

    uint32_t total_tags;
    uint64_t text_bytes;     /* the keys' and values' bytes of every tag held */
    uint32_t next_id;
    /* The highest id the book this registry is written to can name: the
     * kernel's TAGFS_MAX_TAG_ID, or format v1's TAGFS_V1_TAG_ID_MOST for one
     * read off a v1 volume (tag_registry_load). No id past it is issued. */
    uint32_t id_most;

    TagKeyGroup** key_buckets;
    uint32_t      key_bucket_count;
    uint32_t      key_groups;

    TagFSLock lock;

    /*
     * What the medium holds of this registry: the record of every id below
     * written_next, in the chain that ends at tail_block. tail_image is what
     * the next write of the tail puts there: after a load, the medium's tail
     * cut to the records the walk read; after a refused write, the tail as it
     * was before that write — which the refused write may have overwritten
     * all the same. written_next is changed under `lock`; the rest belong to
     * whoever holds `scribe` — one flush of this registry at a time — and to
     * the load before anyone can see the registry.
     *
     * spare_block is the block the chain grows by next: stamped, proved on
     * the medium by the flush carrying spare_need, and kept across a flush
     * that failed until a link names it. 0 is none — block 0 is the chain's
     * head and never a Spare. spare_image is the buffer it is written from.
     */
    uint32_t          written_next;
    uint32_t          tail_block;
    TagRegistryBlock* tail_image;
    uint32_t          spare_block;
    TagRegistryBlock* spare_image;
    uint64_t          spare_need;
    uint64_t          written_need;   /* the flush need of the records last written: a cycle's
                                       * first flush covers it before any record names them */
    bool              said_failing;   /* a refusal is said once, until a flush succeeds */
    /* Format v2: every id below coined_next is in the book — its Tally's TAGS or a settled stitch's Coined —
     * and a cycle lays the ones above down as Coineds (settle/sewing.c), coined_text bytes. Under `lock`. */
    uint32_t          coined_next;
    uint64_t          coined_text;    /* what the ribbon's room holds for them (settle/ribbon.c, RibbonHeld) */
    bool              said_full;      /* every id issued: said once, the first time a tag is refused */
    Scribe            scribe;

    /* What the medium's last block holds of the registry as the last write
     * (or the load) left it — the block, the bytes of its records and their
     * CRC32 — for the seal of a Settle cycle (volume_ledger.h). Under `lock`.
     * `tail_owed`: the medium's last block holds other than tail_image (a
     * load cut it back to what a seal bears out), and the next flush writes
     * it whole even with nothing new; the Scribe's holder's. */
    uint32_t          landed_block;
    uint32_t          landed_bytes;
    uint32_t          landed_crc;
    bool              tail_owed;

    /* The chain's blocks as the load read them, for the mount's census; let
     * go once they are claimed (TagRegistryClaimGround). */
    uint32_t*         chain_blocks;
    uint32_t          chain_count;
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
 * file table's row and the pool's newest record of each (file_map/). Both hold
 * the live files and nothing for an id no live file has. */
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
 * `format` is the book its Deed says it is written in (deed.h, DeedFormatOf):
 * format v1's LAYOUT, Ledger and chains, or format v2's FRAME, Spine and
 * Tally. `geometry` comes out of the Deed and never changes while the volume
 * is mounted; on format v1 so does `layout`, and `ledger` is the part that
 * does, written back to the medium in two alternating copies
 * (volume_ledger.h). A format-v2 volume has neither: its LAYOUT is never
 * filled, and code of format v1 that asks for it on such a volume is a
 * defect, stopped at in a STRICT kernel (TagFSLayout).
 *
 * ‼ WHERE A BLOCK NUMBER POINTS IS TWO NUMBERS, WHATEVER THE FORMAT
 *
 * A block number a file's record or the volume's book carries is counted
 * from `block_base`, in blocks of the volume, and runs up to `block_end`. On
 * format v1 that is its DATA RUN — `block_base` is LAYOUT's data_block and
 * `block_end` its data_blocks: everything TagFS v1 allocates is counted from
 * zero in it, because that is what its records carry and its bitmap indexes.
 * On format v2 every number the book or a file names is a block of the
 * VOLUME (volume_deed.h, VolumeFrame) — `block_base` is 0 and `block_end` is
 * where the volume's far Deed sits, the end of what a file may name. One
 * conversion applies them, and only one: tagfs_block_to_sector().
 */
typedef struct {
    uint8_t          uuid[16];      /* which volume this is */
    char             volume_name[16]; /* Deed's short maker/name, NUL-padded */
    uint64_t         volume_bytes;  /* whole mounted volume capacity */
    DeedFormat       format;        /* the book the mounted volume is written in */
    uint64_t         block_base;    /* the volume block a record's block 0 is */
    uint64_t         block_end;     /* the first block, so counted, no record may name */
    VolumeLayout     layout;        /* format v1: where its parts are, in volume blocks */
    VolumeGeometry   geometry;      /* what it was laid out for */
    VolumeBoot       boot;          /* where the loader finds the kernel; zero when the Deed says nothing */
    VolumeLedger     ledger;        /* what is true of it today */

    TagRegistry*     registry;
    TagBitmapIndex*  bitmap_index;
    FileMap*         files;

    /* The Braid the volume is assembled in, which the mount names — the only
     * thing that sends a block of the volume through Braid. Format v1 names
     * no assembly, so a v1 volume has none (braid/braid.h). */
    struct BraidState* braid;

    bool             initialized;
    TagFSLock        lock;
} TagFSState;

// ----------------------------------------------------------------------------
// Tag Registry API
// ----------------------------------------------------------------------------

int          tag_registry_init(TagRegistry* reg);
void         tag_registry_destroy(TagRegistry* reg);

/* The id of a tag, issued when the registry has none yet. The widths are
 * judged before anything is allocated (ERR_TAGFS_TAG_TOO_LONG); a registry
 * that has issued every id refuses with ERR_TAGFS_REGISTRY_FULL and says so
 * once; ERR_NO_MEMORY when memory ran out. */
error_t      TagRegistryIntern(TagRegistry* reg, const TagSpelling* tag, uint32_t* out_id);
uint32_t     TagRegistryLookup(TagRegistry* reg, const TagSpelling* tag);
void         tag_registry_mark_system(TagRegistry* reg, uint32_t tag_id);

/* A tag's key and value as spans into the registry's own copies, and whether
 * the volume calls it its own. A tag's text never changes and is freed only
 * with the registry, so the spans hold for as long as the registry does. */
bool         TagRegistrySpelling(TagRegistry* reg, uint32_t tag_id, TagSpelling* out,
                                 bool* is_system);

/* The id of every tag whose key is the key_len bytes at `key`, COPIED:
 * out->ids is the caller's to kfree, never the registry's own array. No such
 * key: NULL and 0. */
error_t      TagRegistryKeyIds(TagRegistry* reg, const char* key, size_t key_len, TagIdList* out);

/* Every tag interned before the call written to the medium; made durable by a
 * flush after it — the Settle cycle's first. OK at once, with no I/O, when
 * nothing is owed.
 * May wait for another core's flush of the same registry, so never under a
 * TagFS lock. */
error_t      tag_registry_flush(TagRegistry* reg);

/* The registry read off the medium from its head; the mount's, once. Refuses
 * — and says why — a chain it cannot walk or a registry it cannot hold whole,
 * and the code tells which: ERR_NO_MEMORY when the machine had no memory to
 * hold it, the read's own refusal when a block would not read, and
 * ERR_TAGFS_CORRUPTED when what the medium holds is no registry. Under a seal
 * (`newest`, and the copy before it, `older`; both NULL for a volume whose
 * newest Ledger carries none) it is read as far as the seal says, and a last
 * block that does not bear the seal out is rolled back to the blocks before it
 * (tag_registry.c). */
error_t      tag_registry_load(TagRegistry* reg, uint32_t first_block,
                               const SettleMark* newest, const SettleMark* older);

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
// File Table API
// ----------------------------------------------------------------------------

error_t file_table_init(uint32_t first_block, bool sealed);
void file_table_shutdown(void);
int  file_table_lookup(uint64_t file_id, uint32_t* out_block, uint32_t* out_offset);
int  file_table_update(uint64_t file_id, uint32_t meta_block, uint32_t meta_offset);
int  file_table_delete(uint64_t file_id);
error_t file_table_flush(void);

/* What tagfs_get_metadata filled in, let go. The pool itself is
 * metadata_pool/meta_pool.h's. */
void     tagfs_metadata_free(TagFSMetadata* meta);

// ----------------------------------------------------------------------------
// Main TagFS API
// ----------------------------------------------------------------------------

error_t  tagfs_init(void);
uint8_t  tagfs_get_seat(void);       /* Boardroom seat the volume lives on */

/*
 * ‼ THE DOOR. Step inside the mounted volume before touching anything it
 * brought up, and step out afterwards.
 *
 * A volume can be taken down under a running machine — its medium leaves, and
 * what is held about it is dropped and read again — and everything the mount
 * allocated goes back to the allocator at that moment: the tag registry, the
 * tag index, the block map, the file table, the metadata pool. Nothing is
 * freed while a caller is inside, and no caller gets in once the volume is on
 * its way out. False means there is no volume to be inside; every caller that
 * gets true MUST leave.
 *
 * Every tagfs_* operation below takes this door itself. It is public because
 * one thing more is reachable from outside — a caller that wants to hold
 * something of the volume's across several calls.
 */
bool     tagfs_enter(void);
void     tagfs_leave(void);

/*
 * Which mounting of a volume this is. Bumped every time one is taken up, so
 * anything remembered across a re-read — a file handle, an async read in
 * flight — can be told that what it remembers is from a different volume.
 */
uint32_t tagfs_mount_epoch(void);
bool     tagfs_handle_is_of_this_mount(const TagFSFileHandle* handle);

/*
 * The service point, called from the guide loop and the idle loop — one atomic
 * load per duty when there is nothing to do. Here the work of a Helm handed to
 * a waiter that does it synchronously is run (HelmRunReady), and a volume that
 * has been taken down, which leaves its memory alone until the last caller is
 * out, is finished.
 */
void     TagFSServiceIfPending(void);

/*
 * The volume's share of the loops (settle/cadence.c). Whether this core's loop
 * owes the volume a turn before it sleeps: Helm work listed for a service
 * point, or the Settle's step. On one core, whether the idle context is owed
 * the next quantum (RT-10) — Helm work listed, or a cycle owed now — read by
 * the scheduler from the tick, lock-free.
 */
bool     TagFSStepOwedHere(void);
bool     TagFSIdleOwed(void);

/* Nightwatch's look at the Settle, from the PIT tick (RT-11). */
void     TagFSWatchLook(void);

/*
 * A program's own changes of the volume (settle/cadence.c). An Ink's place is
 * its gen in one order across every mount of the boot, so a place of a volume
 * mounted before this one is told apart. TagFSInkedHere: the place of the
 * newest Ink this core made. TagFSNoteInkerSince: a call of the program was
 * just served on this core, with TagFSInkedHere read before it — the Ink it
 * made, if it made one, is noted; a call that read made none. TagFSInkPlace:
 * the place of this mount's Ink `gen`, inside the door. TagFSNoteInker: the
 * program made the Ink at `place` — the newest is kept. TagFSInkerLeaves: the
 * program is leaving (settle.h) — with a change of its own on this volume no
 * cycle settled, the next cycle is demanded rather than left to Lull.
 */
struct process_t;
uint64_t TagFSInkedHere(void);
void     TagFSNoteInkerSince(struct process_t* proc, uint64_t inked_before);
uint64_t TagFSInkPlace(uint64_t gen);
void     TagFSNoteInker(struct process_t* proc, uint64_t place);
void     TagFSInkerLeaves(struct process_t* proc);

/* Memory came back to the machine — a cabin's, torn down: a Settle cycle
 * refused for want of it is looked at again (settle/settle_wait.c). From any
 * context. */
void     TagFSHeapCameBack(void);

/* The same event for everything on the volume's road that waits for it: the
 * Settle's cycle (TagFSHeapCameBack), and a mount the machine had no memory
 * for, whose service pass is knocked awake. From any context. */
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
uint64_t tagfs_get_volume_base(void);

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

void     TagFSAttendArrival(void);

/* A read of the volume the medium refused — said by the mount's readers, so a
 * mount that fails on a medium that did not answer is asked again when the
 * seat answers (seat:ready), and one refused for what it holds is not. */
void     TagFSNoteReadRefused(void);

/* The medium under the volume may have left; ask now rather than at the next
 * read, because by then the seat can hold something else. */
void     TagFSNoteMediumGone(void);

/* Whether the medium under the mounted volume has left (RT-13): from then on no
 * Ink changes the volume — ERR_DEVICE_NOT_READY — nobody new waits in a Helm's
 * FIFO, and the volume's book is held open until an event decides it
 * (mount/away.h): taken up when the same volume comes back as it was left,
 * else given up and the volume let go of. TagFSMediumGone asks the room first
 * — an empty chair, or one holding another medium than the volume was mounted
 * from — and latches a departure it finds: asked before every run of blocks
 * the mounted volume reads or writes (run/run.c), and by a caller whose medium
 * command was refused, who must tell a medium that left from one that failed. */
bool     TagFSMediumLeft(void);
bool     TagFSMediumGone(void);

/* Whether the mounted volume takes no write: its medium refused one as
 * protected (RT-18) — DATA PROTECT on USB, an ATA abort on AHCI — or the
 * volume stands under glass (TagFSIsUnderGlass). From then on no Ink changes
 * the volume — ERR_TAGFS_READ_ONLY — and every waiter for a cycle hears the
 * same. A protection lasts until the volume is let go of, or its book is
 * taken up, the medium seated again and asked anew: TagFSWriteRefused asks
 * the seat after a write the medium refused, and latches it. Glass lasts
 * for the whole mount. */
bool     TagFSReadOnly(void);
void     TagFSWriteRefused(void);

/* Said once, by whoever tried the boot mount, whether or not it succeeded.
 * Before it, an arriving medium belongs to the boot that is still happening. */
void     TagFSBootMountSettled(void);
/* The machine is stopping, and its last Settle cycle has run (settle.h,
 * TagFSSettleForHalt): the door shut, and what that cycle answered. */
error_t tagfs_shutdown(void);

/* Flush the volatile write cache of the disk that holds the TagFS volume
 * (routes to the probed AHCI port / ATA drive, not a hardcoded device). The
 * one place a flush ticket is taken and answered (scribe/scribe.h). */
error_t  tagfs_flush_cache(void);

/* Convert a TagFS logical block number to its absolute disk-sector LBA.
 * Used by the storage deck's reads and writes nobody stands over, which reach
 * the seat (BoardroomReadAsync, BoardroomWriteAsync) outside the block
 * helpers. */
uint64_t tagfs_block_to_sector(uint64_t block);

/* There is no tagfs_format() here. A volume is made by the tool that lays it
 * out on ground it read from a partition table (tools/create_tagfs_ground.c),
 * and by nothing else. The kernel used to carry a second formatter that
 * nothing called — so nothing ever ran it, nothing ever checked it, and it
 * went on writing the layout the rest of the kernel had already left behind. */

/* Each change of a file below answers -ERR_WOULD_BLOCK, nothing of it done —
 * an untag by text, nothing of what its text still names — while the volume
 * is short of ground a Settle cycle gives back: a block for the registry to
 * grow by, a Spare for the pool's tail. No cycle is run beneath the call: its
 * caller waits for one its own way and asks again (settle_ops.h,
 * SettleOpForRoom). */

/* 0, or a negative -ERR_*: a name whose tag the registry refuses (too long,
 * the registry full) is refused with the registry's code, no file is made
 * without the tag its name gives it, a record wider than format v1 carries
 * is refused ERR_TAGFS_RECORD_FULL, and a volume that has issued every file id
 * it can is refused ERR_TAGFS_FID_EXHAUSTED (LedgerIssueFileId). */
int  tagfs_create_file(const char* filename, const uint32_t* tag_ids, uint32_t tag_count,
                       uint64_t* out_file_id);

/* Whether a file named `filename` wearing `tags` — each counted once, its
 * name's own tag with them — has a record format v1 can hold: OK, or
 * ERR_TAGFS_RECORD_FULL; the name's refusal (ERR_TAGFS_TAG_TOO_LONG), or
 * ERR_NO_MEMORY. Asked before a single tag is interned, so a create refused
 * for its width spends none of the volume's numbers. *out_worn (optional):
 * how many tags the record would list. */
error_t TagFSCreateFits(const char* filename, const TagSpelling* tags, uint32_t count,
                        uint32_t* out_worn);

/* The file given up, or renamed — its old name's tag off, the new one's on.
 * 0, or a negative -ERR_*: a file the system keeps in a role is given up only
 * with the warrant (-ERR_PERMISSION_DENIED); the file holding the kernel the
 * Deed points the loader at is given up by nobody. A rename asks none: its
 * name's tag is a tag like any other. */
int  tagfs_delete_file(uint64_t file_id, InkWarrant warrant);
/* A file deleted for a term (keep/keep.h): kept `term_seconds` from now, no
 * query meeting it, and let go by the volume when the term ends — or, for a
 * term of 0, given up at once, as tagfs_delete_file does. The term is the
 * caller's: the volume has none of its own. A file the system keeps is
 * deleted only with the warrant, as it is given up. -ERR_UNSUPPORTED on
 * format v1 for a term above 0. tagfs_restore_file brings a deleted file back
 * until its term ends (-ERR_FILE_NOT_FOUND after, or for a file the volume
 * does not hold), and is no change to a file that is not deleted. */
int  tagfs_keep_file(uint64_t file_id, uint64_t term_seconds, InkWarrant warrant);
int  tagfs_restore_file(uint64_t file_id, InkWarrant warrant);
int  tagfs_rename_file(uint64_t file_id, const char* new_filename);

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
error_t tagfs_span_file(uint64_t file_id, uint64_t size);

/* A tag put on a file, or taken off: 0, or the Ink's refusal as -ERR_*. No
 * tag asks a warrant: a file's tags are its user's. */
int  tagfs_add_tag(uint64_t file_id, uint32_t tag_id);
int  tagfs_remove_tag(uint64_t file_id, uint32_t tag_id);
bool tagfs_has_tag(uint64_t file_id, uint32_t tag_id);

/* The tag a spelling names, interned and put on the file, both inside one
 * mounting of the volume: OK, or the real refusal — the registry's (the tag
 * too long, the registry full), or the Ink's: ERR_FILE_NOT_FOUND,
 * ERR_TAGFS_RECORD_FULL, ERR_DEVICE_NOT_READY, ERR_TAGFS_READ_ONLY,
 * ERR_NO_MEMORY, a Spare's refusal; ERR_TAGFS_NOT_INITIALIZED with no volume. */
error_t tagfs_add_tag_string(uint64_t file_id, const TagSpelling* tag);
/* What the spelling names comes off the file: that tag when it has a value,
 * every tag of its key when it is bare. 0 once one of them came off, or a
 * negative -ERR_*: -ERR_TAGFS_TAG_NOT_FOUND when the file wore none of them,
 * -ERR_FILE_NOT_FOUND when there is no such file, or the first refusal of
 * taking one off — -ERR_DEVICE_NOT_READY, -ERR_TAGFS_READ_ONLY,
 * -ERR_NO_MEMORY, or a Spare's refusal. */
int  tagfs_remove_tag_string(uint64_t file_id, const TagSpelling* tag);

bool tagfs_key_is_reserved(const char *key, size_t key_len);

/*
 * The live files a question matches, a page at a time and in file id order
 * (query/query.c): every file wearing all of `tags` and none of `without`
 * ("key:..." is any tag of the key, in either list) — a question of no tags is
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
error_t  TagFSQueryPage(const TagSpelling* tags, uint32_t count, const TagSpelling* without,
                        uint32_t without_count, uint64_t from, uint64_t* out, uint32_t room,
                        bool want_total, TagFSPage* page);
error_t  TagFSListPage(uint64_t from, uint64_t* out, uint32_t room, bool want_total,
                       TagFSPage* page);
/* The same question asked of the files kept deleted alone: those that wear all
 * of `tags` and none of `without`, a question of neither every one of them. */
error_t  TagFSQueryKeptPage(const TagSpelling* tags, uint32_t count, const TagSpelling* without,
                            uint32_t without_count, uint64_t from, uint64_t* out, uint32_t room,
                            bool want_total, TagFSPage* page);
/*
 * The tags files wear, a page at a time and in the order of their numbers
 * (query/query.c): from the tag numbered `from` on, every tag at least one
 * live file wears — one of the files that wear all of `within` and none of
 * `without`, as TagFSQueryPage asks — with how many of them do, written into
 * `out` as STORAGE_TAG_LIST records (boxos_decks.h) while they fit its `room`
 * bytes. A tag no such file wears — a process's, a Touch's, a Bay's, which the
 * registry names too — is passed over. *used the bytes written, *delivered the
 * records, *next the number to ask from for the rest, 0 when there are none.
 * OK, or the refusal: ERR_TAGFS_NOT_INITIALIZED with no volume, ERR_NO_MEMORY,
 * or ERR_BUFFER_TOO_SMALL when not even the first record fits.
 */
error_t  TagFSTagPage(const TagSpelling* within, uint32_t within_count, const TagSpelling* without,
                      uint32_t without_count, uint32_t from, uint8_t* out, uint32_t room, uint32_t* used,
                      uint32_t* delivered, uint32_t* next);
/* The file id the next create takes — no id of this volume has reached it.
 * 0 when no volume is mounted. */
uint64_t tagfs_file_ceiling(void);

/* The file opened for one operation: NULL when it is not there, when it has
 * no record in memory, when it is locked (book_mount.h, TagFSFileLocked), or when the handle
 * or its place among the file's open handles could not be had. A handle
 * opened to read holds the record by reference; one opened only to write
 * holds none (TagFSFileHandle). */
TagFSFileHandle* tagfs_open(uint64_t file_id, uint32_t flags);
void             tagfs_close(TagFSFileHandle* handle);

/* Up to `size` bytes from the handle's offset into the kernel's `buffer`: OK
 * and *out_read — 0 at or past the file's size — or the refusal with
 * *out_read the bytes that landed before it. The refusal is the real one: a
 * block the medium would not give (ERR_READ_FAILED), a medium that left
 * (ERR_DEVICE_NOT_READY), a block that does not hold what was written to it
 * (ERR_CHECKSUM), a part of the file inside its size that no extent covers
 * (ERR_METADATA_CORRUPTED). The offset moves by what landed. */
error_t          tagfs_read(TagFSFileHandle* handle, void* buffer, uint64_t size,
                            uint64_t* out_read);

/* The same read into wherever `sink` says — a program's pages, a block at a
 * time as the read reaches them (read/read.h) — up to the sink's room. */
struct ReadSink;
error_t          TagFSReadInto(TagFSFileHandle* handle, const struct ReadSink* sink,
                               uint64_t* out_read);

/* The source's bytes written at the handle's offset — at the file's size when
 * the handle was opened TAGFS_HANDLE_APPEND. An offset past the size is
 * refused ERR_TAGFS_BEYOND_SIZE: a file grows by being written at its end, or
 * by span, never across a gap. OK with *out_written the whole source, or the
 * refusal with *out_written the bytes the file now holds of it — a prefix is
 * kept when the medium stopped taking the rest. The offset moves by what was
 * written, and the handle's file_size is the size the write left. */
struct PlotSource;
error_t          tagfs_write(TagFSFileHandle* handle, const struct PlotSource* src,
                             uint64_t* out_written);

/* One run of exactly `count` blocks, the lowest that is free: 0, or -1 when
 * there is none (block_map.h, BlockMapTake). */
int      tagfs_alloc_blocks(uint32_t count, uint64_t* out_start_block);
/* The run back: 0, or a negative -ERR_*. A run holding a block that is already
 * free is refused whole (-ERR_TAGFS_EXTENT_ERROR), said, and the map is left
 * as it is. */
int      tagfs_free_blocks(uint64_t start_block, uint32_t count);

#ifdef CONFIG_VOLUME_TESTS
/* The free count derived again from the block map, as a mount derives it;
 * returns how far the Ledger's count had drifted from it. For a startup test
 * that watches a block be given back twice — see the definition. */
int64_t  TagFSAllocatorRederive(void);
#endif

int         tagfs_get_metadata(uint64_t file_id, TagFSMetadata* out);
TagFSState* tagfs_get_state(void);

/* The mounted volume's LAYOUT, for the code of format v1 that walks what it
 * names. A volume of format v2 has none — its LAYOUT is never filled — and
 * code of format v1 asking for it there is a defect: said once a mount, and
 * stopped at in a STRICT kernel. */
VolumeLayout* TagFSLayout(void);

/* The file laid in one run, taken near where it begins, its record naming the
 * run in as few extents as a record can say it. OK, or the refusal: the file
 * is left where it was. A file the system keeps — in a role by its record, or
 * holding the kernel the Deed points at — is not moved, warrant or none
 * (plot.h, PlotGather): ERR_PERMISSION_DENIED. */
error_t  tagfs_defrag_file(uint64_t file_id);
/* How fragmented the volume's files are: the share, in percent, of the steps
 * from one of a file's blocks to its next that are a gap — the next run not
 * beginning where the one before it ends — rounded up, so one gap never reads
 * as none. The live files counted and the gaps found are said through
 * out_files and out_gaps when they are not NULL. */
uint32_t tagfs_get_fragmentation_score(uint32_t* out_files, uint32_t* out_gaps);
#ifdef CONFIG_VOLUME_TESTS
/* One record's share of that score, as the walk of the live files counts it:
 * the gaps a read of it seeks across, and the steps between its blocks of
 * ground — for the stand, whose records with holes no volume of format v1
 * holds. */
void     TagFSFragOfRecord(const struct FileRecord* rec, uint32_t* gaps, uint64_t* steps);
#endif

// ----------------------------------------------------------------------------
// Block I/O (for subsystem use) — a run of blocks in as few commands as the
// seat takes is run/run.c's (TagFSReadRun below, TagFSWriteRun there)
// ----------------------------------------------------------------------------

/* One block: OK, or the real refusal — ERR_DEVICE_NOT_READY once the medium
 * left, ERR_READ_FAILED when it would not give the block, ERR_BAD_SECTOR when
 * it answered that a sector of it cannot be read, ERR_CHECKSUM when
 * what it gave does not match the digest recorded when the block was written
 * (integrity/integrity.h, which names the block and publishes it); a write
 * ERR_DEVICE_NOT_READY or ERR_WRITE_FAILED, its digest recorded once it lands. */
error_t  tagfs_read_block(uint64_t block, void* buffer);
error_t  tagfs_write_block(uint64_t block, const void* buffer);

/* One block of the volume's own records — a chain's link, a stamp, the pool's
 * head or a block of it, a page of the integrity map — through the same doors
 * with the integrity map left out, which covers none of them (RT-22). The same
 * refusals, but never ERR_CHECKSUM. */
error_t  TagFSMetaBlockRead(uint64_t block, void* buffer);
error_t  TagFSMetaBlockWrite(uint64_t block, const void* buffer);

/* `count` blocks from `first` on, in as few commands as the seat takes and
 * checked against nothing: for a reader that checks each block where it hands
 * it out — the read-ahead's fetch, a defrag's copy. A volume assembled in a
 * Braid gives them a block at a time. OK, ERR_DEVICE_NOT_READY or
 * ERR_READ_FAILED. */
error_t  TagFSReadRun(uint64_t first, uint32_t count, void* buffer);

/* A write landed on `block`: no read-ahead window hands out what it held
 * before (read-after-write). tagfs_write_block says so itself; a writer that
 * goes to the medium another way — the asynchronous WriteJob, the Use
 * Context's chain — says so after each block. */
void     tagfs_readahead_invalidate(uint64_t block);

/*
 * Read one sector run of the mounted volume, counted from the start of its
 * ground rather than from the start of the medium.
 *
 * This is the door TagFS and everything under it uses to reach the medium —
 * the DiskBook included — and a block of the data run goes by
 * tagfs_block_to_sector. The base is applied in those two and nowhere else,
 * which is what makes "no absolute addresses inside a volume" an invariant
 * instead of a convention.
 */
int      tagfs_volume_read (uint64_t vlba, uint32_t count, void* buffer);
int      tagfs_volume_write(uint64_t vlba, uint32_t count, const void* buffer);

/*
 * Tags by name, for everything outside TagFS.
 *
 * The registry is the volume's, it is freed when the volume is read again, and
 * it used to be handed out as a pointer to seven other files. These are what
 * those files actually wanted: an id for a name, a name for an id, and a copy
 * of the name rather than a pointer into the volume. Each takes the door for
 * the length of its answer, so none of them can be holding the registry when
 * it goes. TAGFS_INVALID_TAG_ID / false is the answer when there is no volume.
 * A tag's text is read by the one spelling (tag_spelling.h).
 */
uint32_t tagfs_tag_lookup(const char* tag);
uint32_t TagFSLookupSpelling(const TagSpelling* tag);

/* The id, issued when the volume has none: OK, the spelling's or the
 * registry's refusal, or ERR_TAGFS_NOT_INITIALIZED when there is no volume. */
error_t  tagfs_tag_intern(const char* tag, uint32_t* out_id);
error_t  TagFSInternSpelling(const TagSpelling* tag, uint32_t* out_id);

/* The key of a tag id — "app" out of "app:editor" — copied whole into `out`,
 * or not at all: false when the volume has no such tag or the key would not
 * fit, never a shorter key. */
bool     tagfs_tag_key (uint32_t tag_id, char* out, size_t out_size);

/* The whole text of a tag id — "app:editor" — and its length. It is written
 * into `out` only when it fits whole, NUL and all, and `out` is left empty
 * otherwise, so a caller is never handed a shorter tag than the one it asked
 * for: a caller that must have it measures first (out NULL, size 0). 0 when
 * the volume has no such tag, or there is no volume. */
size_t   tagfs_tag_text(uint32_t tag_id, char* out, size_t out_size);

/* Whether tag `tag_id` is one a pattern names (TagPatternMatches), read in
 * place in the volume's registry. */
bool     TagFSTagMatches(uint32_t tag_id, const TagSpelling* pattern);

/* What a process started from a file wears: the keys of the file's `count`
 * tags, comma-joined in a heap string the caller kfree's, whole however long —
 * but for the keys system and god, which a file's tags never give: a file is
 * its user's to tag with any word, and a program is given the system's
 * authority only by its record, whose `flags` carry the role
 * (TAGFS_FILE_ROLE_SYSTEM adds the key system). An id the volume does not know
 * is passed over. NULL when there is no memory, or no volume. */
char*    TagFSProgramKeys(const uint32_t* tag_ids, uint32_t count, uint32_t flags);

/* Whether one of `count` tags has the key `key` — "raw" is the key of "raw" and
 * of "raw:anything" — read in place in the volume's registry. False when none
 * has, and when there is no volume. */
bool     TagFSKeyAmong(const uint32_t* tag_ids, uint32_t count, const char* key);

/* A tag's key and value as spans into the volume's registry, and whether the
 * volume calls the tag its own — no copy, for a caller that is INSIDE THE
 * DOOR (tagfs_enter) and reads them before it leaves: the registry is freed
 * only once the last caller is out. */
bool     TagFSTagSpelling(uint32_t tag_id, TagSpelling* out, bool* is_system);

// Snapshots: a format-v1 volume answers every request for one ERR_UNSUPPORTED
// (TagFS_SnapshotRefused, tagfs/cow/cow.h); they arrive with format v2.

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
