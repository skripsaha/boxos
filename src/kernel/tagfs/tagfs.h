#ifndef TAGFS_H
#define TAGFS_H

#include "../../lib/kernel/ktypes.h"
#include "../../lib/kernel/klib.h"
#include "tagfs_constants.h"
#include "plot/plot_width.h"
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

#include "disk_book/disk_book.h"
#include "cow/cow.h"
#include "ledger/ledger.h"
#include "block_map/block_map.h"

// ============================================================================
// TagFS Constants — Production Configuration
// ============================================================================

#define TAGFS_VERSION               1
#define TAGFS_BLOCK_SIZE            4096

/*
 * Where the volume is, and what it is made of, is no longer written here.
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

// Tag constants
#define TAGFS_INVALID_TAG_ID        0xFFFF
/* Volume tag ids stop one short of bit 15. Bit 15 belongs to the kernel's
 * Logbook (core/touch/logbook.h) — the vocabulary of occurrences, which must
 * exist on a machine that never mounted a volume. Capping here is what makes
 * the split an invariant instead of a convention: the volume cannot issue an
 * id that collides with an occurrence even in principle. 32766 distinct tags
 * per volume; the largest volume seen carries ~120. */
#define TAGFS_MAX_TAG_ID            0x7FFE

// File flags
#define TAGFS_FILE_ACTIVE           (1 << 0)
#define TAGFS_FILE_TRASHED          (1 << 1)
#define TAGFS_FILE_HIDDEN           (1 << 2)
/* Bit 3 is spent on v1: kernels before Quill step 1.1 took it for copy-on-write, so it is never set and never trusted. */

// Handle flags
#define TAGFS_HANDLE_READ           (1 << 0)
#define TAGFS_HANDLE_WRITE          (1 << 1)
#define TAGFS_HANDLE_APPEND         (1 << 2)   /* every write lands at the file's size */

// Registry constants
#define TAGFS_REG_BUCKETS           512
#define TAGFS_KEY_BUCKETS           128
#define TAGFS_REGISTRY_DATA_SIZE    4080
#define TAGFS_FTABLE_PER_BLOCK      510

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
// still held by a file. The two words are a pair on purpose — one is what was
// ever written here, the other is what is still held — and Keep is already this
// project's word for it (Chit: Give / Due / Keep). A record is retired by
// zeroing its file_id in place — the pool never moves bytes, because a file
// table entry is a byte offset into this block and moving a record would strand
// it — so the two numbers drift apart as files are deleted, and a block whose
// kept count falls to zero is 4080 bytes of nothing that can be handed back to
// the bitmap.
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
typedef struct {
    char*    key;
    char*    value;
    uint16_t value_len;
    uint8_t  key_len;
    uint8_t  flags;
    uint16_t tag_id;
} TagRegistryEntry;

typedef struct TagRegistryNode {
    uint16_t                tag_id;
    struct TagRegistryNode* next;
} TagRegistryNode;

/* Every tag of one key. Its inside is tag_registry.c's alone: a key group's
 * array is freed by the next tag of its key, so nothing outside the registry's
 * lock may hold a pointer into one (MP-D11) — TagRegistryKeyIds copies. */
typedef struct TagKeyGroup TagKeyGroup;

/* The two hashes — tags by their text, and a key's every tag — start at
 * TAGFS_REG_BUCKETS and TAGFS_KEY_BUCKETS buckets and double as they fill:
 * a new table is built outside the lock and swapped in under it. */
typedef struct {
    TagRegistryNode** buckets;
    uint32_t          bucket_count;

    TagRegistryEntry** by_id;
    uint32_t           by_id_capacity;

    uint32_t total_tags;
    uint16_t next_id;

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
    uint16_t          written_next;
    uint32_t          tail_block;
    TagRegistryBlock* tail_image;
    uint32_t          spare_block;
    TagRegistryBlock* spare_image;
    uint64_t          spare_need;
    uint64_t          written_need;   /* the flush need of the records last written: a cycle's
                                       * first flush covers it before any record names them */
    bool              said_failing;   /* a refusal is said once, until a flush succeeds */
    bool              said_full;      /* every id issued: said once, the first time a tag is refused */
    Scribe            scribe;
} TagRegistry;

// ----------------------------------------------------------------------------
// In-Memory Structures: the Indexes of the Live Files
// ----------------------------------------------------------------------------

typedef struct {
    uint16_t* ids;
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
    uint32_t next;        /* the first match past the room: ask again from it;
                           * 0 when there is none */
} TagFSPage;

// ----------------------------------------------------------------------------
// In-Memory Structures: File Handles and Metadata
// ----------------------------------------------------------------------------

/* A place in a file's Helm (helm/helm.h): the FIFO while it waits, then the
 * ready list when its work is run at a service point. */
typedef struct HelmWaiter {
    struct HelmWaiter* next;
    void (*hand)(struct HelmWaiter* w);   /* the Helm is w's now; no TagFS lock held */
    void (*run)(struct HelmWaiter* w);    /* a waiter HelmReady listed is run here */
} HelmWaiter;

/* One holder of a file's extents at a time, and who waits for it. */
typedef struct {
    TagFSLock     lock;       /* leaf: the three below */
    HelmWaiter*   holder;     /* NULL when nobody holds it */
    HelmWaiter*   first;      /* the FIFO, oldest first */
    HelmWaiter*   last;
    volatile bool vacated;    /* a Quitclaim gave the file up */
} FileHelm;

struct TagFSFileHandle;

/* A file some handle has open: its Helm, and every open handle's view of it,
 * under the open-file table's lock (helm/helm.c). */
typedef struct OpenFileEntry {
    uint32_t                file_id;
    uint32_t                ref_count;
    FileHelm                helm;
    struct TagFSFileHandle* views;
    struct OpenFileEntry*   next;
} OpenFileEntry;

/* A file opened for one operation. A handle opened to read carries the record
 * as the open found it — `extents` and `file_size` — and only a read walks
 * them; one opened only to write carries no extents: its work loads the record
 * under the file's Helm (plot.h, PlotLoad) and leaves here only where it ended
 * — `offset`, and the size it left. `view_gen` is the newest Ink when the open
 * looked: ground an Ink after it frees from the file rests until the handle is
 * closed (helm.h). */
typedef struct TagFSFileHandle {
    uint32_t    file_id;
    uint32_t    flags;
    uint64_t    offset;
    uint64_t    file_size;
    FileExtent* extents;
    uint32_t    extent_count;
    OpenFileEntry* ofe;

    /* Which mounting of the volume the extents above were copied out of.
     *
     * A medium that leaves and comes back is READ AGAIN, so the block numbers
     * a handle is carrying describe the volume as it was before it left. On
     * the volume as it is now they are somebody else's blocks, and following
     * them is not a failed read — it is a successful read of the wrong file.
     * See tagfs_mount_epoch(). */
    uint32_t    mount_epoch;

    uint64_t                view_gen;
    struct TagFSFileHandle* view_prev;   /* the file's views (OpenFileEntry.views) */
    struct TagFSFileHandle* view_next;
} TagFSFileHandle;

typedef struct {
    uint32_t    file_id;
    uint32_t    flags;
    uint64_t    size;
    uint64_t    created_time;
    uint64_t    modified_time;
    char*       filename;
    uint16_t*   tag_ids;
    uint32_t    tag_count;
    FileExtent* extents;
    uint32_t    extent_count;
} TagFSMetadata;

// ----------------------------------------------------------------------------
// In-Memory Structures: Global State
// ----------------------------------------------------------------------------

/*
 * What the mounted volume is, split the way the medium splits it.
 *
 * `layout` and `geometry` come out of the Deed and never change while the
 * volume is mounted; `ledger` is the part that does, and is written back to
 * the medium in two alternating copies (volume_ledger.h).
 *
 * ‼ TWO COUNTS OF BLOCKS LIVE HERE AND THEY ARE NOT THE SAME
 *
 * Everything in `layout` is counted in blocks of the WHOLE VOLUME, because
 * that is what a Deed states and what makes a volume movable. Everything
 * TagFS itself allocates — file extents, the bitmap, the registry, the file
 * table, the metadata pool — is counted in blocks of the DATA RUN, from zero,
 * because that is what a file's metadata carries on disk and what the bitmap
 * indexes. `layout.data_block` is the one number that converts between them,
 * and tagfs.c's block_to_vlba() is the one place that applies it.
 */
typedef struct {
    uint8_t          uuid[16];      /* which volume this is */
    VolumeLayout     layout;        /* where its parts are, in volume blocks */
    VolumeGeometry   geometry;      /* what it was laid out for */
    VolumeBoot       boot;          /* where the loader finds the kernel; zero when the Deed says nothing */
    VolumeLedger     ledger;        /* what is true of it today */

    TagRegistry*     registry;
    TagBitmapIndex*  bitmap_index;
    FileMap*         files;
    bool             initialized;
    TagFSLock        lock;
} TagFSState;

/* The ids of trashed and hidden, which listings leave out unless asked for
 * (tagfs_init_well_known_tags); TAGFS_INVALID_TAG_ID when the registry has no
 * such tag. Membership filters, not security: authority is the fixed
 * cabin_t.auth_bits (auth_tags.h). They were kept as 1ULL << id, which is 0
 * for every id past 63 — a volume that interned trashed late had no trash. */
typedef struct {
    uint16_t trashed;
    uint16_t hidden;
} WellKnownTags;

// Accessor functions (no extern needed)
WellKnownTags* tagfs_get_well_known_tags(void);

// ----------------------------------------------------------------------------
// Tag Registry API
// ----------------------------------------------------------------------------

int          tag_registry_init(TagRegistry* reg);
void         tag_registry_destroy(TagRegistry* reg);

/* The id of a tag, issued when the registry has none yet. The widths are
 * judged before anything is allocated (ERR_TAGFS_TAG_TOO_LONG); a registry
 * that has issued every id refuses with ERR_TAGFS_REGISTRY_FULL and says so
 * once; ERR_NO_MEMORY when memory ran out. */
error_t      TagRegistryIntern(TagRegistry* reg, const TagSpelling* tag, uint16_t* out_id);
uint16_t     TagRegistryLookup(TagRegistry* reg, const TagSpelling* tag);
void         tag_registry_mark_system(TagRegistry* reg, uint16_t tag_id);

/* A tag's key and value as spans into the registry's own copies, and whether
 * the volume calls it its own. A tag's text never changes and is freed only
 * with the registry, so the spans hold for as long as the registry does. */
bool         TagRegistrySpelling(TagRegistry* reg, uint16_t tag_id, TagSpelling* out,
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
 * — and says why — a chain it cannot walk or a registry it cannot hold whole. */
int          tag_registry_load(TagRegistry* reg, uint32_t first_block);

// ----------------------------------------------------------------------------
// Bitmap Index API
// ----------------------------------------------------------------------------

/* An index with room for `initial_tag_cap` tags before its table of them
 * first grows; NULL when there is no memory. */
TagBitmapIndex* tag_bitmap_create(uint32_t initial_tag_cap);
void            tag_bitmap_destroy(TagBitmapIndex* idx);
/* The file put under the tag: 0, or -1 — said once, with the index's numbers
 * — when the index had no memory for it, and then nothing changed. */
int             tag_bitmap_set(TagBitmapIndex* idx, uint16_t tag_id, uint32_t file_id);
/* The file out from under the tag: 0, or -1 when no file wears the tag. */
int             tag_bitmap_clear(TagBitmapIndex* idx, uint16_t tag_id, uint32_t file_id);
/* The file out from under each of the `tag_count` tags it wore — its record's
 * — or, with no record to say (tag_ids NULL), from under every tag. */
void            tag_bitmap_remove_file(TagBitmapIndex* idx, uint32_t file_id,
                                       const uint16_t* tag_ids, uint32_t tag_count);
bool            TagBitmapHas(TagBitmapIndex* idx, uint16_t tag_id, uint32_t file_id);

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
error_t         TagBitmapStakeFor(TagBitmapIndex* idx, const uint16_t* tags, uint32_t count,
                                  uint32_t file_id, TagBitmapStake** out);
bool            TagBitmapStakeCovers(TagBitmapIndex* idx, const uint16_t* tags, uint32_t count,
                                     uint32_t file_id, TagBitmapStake* stake);
void            TagBitmapAddCovered(TagBitmapIndex* idx, const uint16_t* tags, uint32_t count,
                                    uint32_t file_id);
void            TagBitmapStakeLetGo(TagBitmapStake* stake);

/* The file out from under the tag with nothing allocated — a window that falls
 * to where an array would be lighter stays a bitmap, which costs room and
 * nothing else — for a caller under the volume's state lock. */
void            TagBitmapLeave(TagBitmapIndex* idx, uint16_t tag_id, uint32_t file_id);
/* The kernel heap the index holds. */
uint64_t        TagBitmapBytes(TagBitmapIndex* idx);
/* The files from `from` on that wear every one of `tag_ids`, at least one
 * tag of each of `groups`, and none of `unless`, in id order: up to `room`
 * into `out`, and the page says the rest (TagFSPage). 0, or -1 for no index. */
int             TagBitmapQueryPage(TagBitmapIndex* idx,
                    const uint16_t* tag_ids, uint32_t tag_count,
                    const TagIdList* groups, uint32_t group_count,
                    const uint16_t* unless, uint32_t unless_count,
                    uint32_t from, uint32_t* out, uint32_t room,
                    bool want_total, TagFSPage* page);

// ----------------------------------------------------------------------------
// File Table API
// ----------------------------------------------------------------------------

int  file_table_init(uint32_t first_block);
void file_table_shutdown(void);
int  file_table_lookup(uint32_t file_id, uint32_t* out_block, uint32_t* out_offset);
int  file_table_update(uint32_t file_id, uint32_t meta_block, uint32_t meta_offset);
int  file_table_delete(uint32_t file_id);
int  file_table_flush(void);

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
 * Either the volume this kernel had is back — same identity, and unchanged
 * while it was out of the machine, both read off the medium rather than
 * assumed — or nothing was ever mounted and this is the first medium that
 * could carry a volume. Called from a context that is allowed to wait,
 * because deciding takes reads.
 */
/*
 * Listen for media arriving and leaving, instead of being called about them.
 * Set up once, before the boot mount is attempted — a medium that turns up
 * while that mount is running is announced to whoever is subscribed at that
 * moment. Idempotent.
 */
void     TagFSWatchSeats(void);

void     TagFSAttendArrival(void);

/* The medium under the volume may have left; ask now rather than at the next
 * read, because by then the seat can hold something else. */
void     TagFSNoteMediumGone(void);

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
 * Used by the async storage path which submits raw AHCI reads outside
 * the read_block helper. */
uint64_t tagfs_block_to_sector(uint32_t block);

/* There is no tagfs_format() here. A volume is made by the tool that lays it
 * out on ground it read from a partition table (tools/create_tagfs.c), and by
 * nothing else. The kernel used to carry a second formatter that nothing
 * called — so nothing ever ran it, nothing ever checked it, and it went on
 * writing the layout the rest of the kernel had already left behind. */

/* 0, or a negative -ERR_*: a name whose tag the registry refuses (too long,
 * the registry full) is refused with the registry's code, no file is made
 * without the tag its name gives it, a record wider than format v1 carries
 * is refused ERR_TAGFS_RECORD_FULL, and a volume that has issued every file id
 * it can is refused ERR_TAGFS_FID_EXHAUSTED (LedgerIssueFileId). */
int  tagfs_create_file(const char* filename, const uint16_t* tag_ids, uint32_t tag_count,
                       uint32_t* out_file_id);
int  tagfs_delete_file(uint32_t file_id);
int  tagfs_rename_file(uint32_t file_id, const char* new_filename);

/*
 * The three changes of a file's extents — a write, span and defrag — are made
 * by the holder of the file's Helm (helm/helm.h). A program reaches them
 * through the storage deck, which parks it while another holds the Helm
 * (write_job.c). These are for a caller in the kernel, which cannot be parked
 * and does not spin: each takes the Helm only when it is free, and answers
 * ERR_BUSY, said, when another holds it.
 */

/* span — the file made exactly `size` bytes long. Shorter: the tail is cut and
 * its blocks given back. Longer: the bytes from the old size to the new one
 * read as zeros, written to blocks taken for them — never a previous owner's
 * bytes. The one way a file grows past its size other than by being written at
 * its end. OK, or the refusal: nothing of the file is changed by one. */
error_t tagfs_span_file(uint32_t file_id, uint64_t size);

int  tagfs_add_tag(uint32_t file_id, uint16_t tag_id);
int  tagfs_remove_tag(uint32_t file_id, uint16_t tag_id);
bool tagfs_has_tag(uint32_t file_id, uint16_t tag_id);

/* The tag a spelling names, interned and put on the file: OK, the registry's
 * refusal, or ERR_INVALID_ARGUMENT when the file would not take it. */
error_t tagfs_add_tag_string(uint32_t file_id, const TagSpelling* tag);
/* What the spelling names comes off the file: that tag when it has a value,
 * every tag of its key when it is bare. -1 when the file wore none of them. */
int  tagfs_remove_tag_string(uint32_t file_id, const TagSpelling* tag);

bool tagfs_key_is_reserved(const char *key, size_t key_len);

/*
 * The live files a question matches, a page at a time and in file id order
 * (query/query.c): every file wearing all of `tags` ("key:..." is any tag of
 * the key) — a question of no tags is every file, TagFSListPage — leaving out
 * the trashed and the hidden unless the question names them. From file id
 * `from` on, up to `room` ids into `out`; the page says where the rest begin
 * and, when `want_total`, how many match from `from` on. OK, or the refusal:
 * ERR_TAGFS_NOT_INITIALIZED with no volume, ERR_NO_MEMORY when the question
 * could not be put to the index.
 */
error_t  TagFSQueryPage(const TagSpelling* tags, uint32_t count, uint32_t from,
                        uint32_t* out, uint32_t room, bool want_total, TagFSPage* page);
error_t  TagFSListPage(uint32_t from, uint32_t* out, uint32_t room, bool want_total,
                       TagFSPage* page);
/* The file id the next create takes — no id of this volume has reached it.
 * 0 when no volume is mounted. */
uint32_t tagfs_file_ceiling(void);

/* The file opened for one operation: NULL when it is not there, or when the
 * handle, its extents or its place among the file's open handles could not
 * be had. */
TagFSFileHandle* tagfs_open(uint32_t file_id, uint32_t flags);
void             tagfs_close(TagFSFileHandle* handle);

/* Up to `size` bytes from the handle's offset: OK and *out_read — 0 at or
 * past the file's size — or the refusal, with nothing counted. The offset
 * moves by what was read. */
error_t          tagfs_read(TagFSFileHandle* handle, void* buffer, uint64_t size,
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
int      tagfs_alloc_blocks(uint32_t count, uint32_t* out_start_block);
/* The run back: 0, or a negative -ERR_*. A run holding a block that is already
 * free is refused whole (-ERR_TAGFS_EXTENT_ERROR), said, and the map is left
 * as it is. */
int      tagfs_free_blocks(uint32_t start_block, uint32_t count);

#ifdef CONFIG_VOLUME_TESTS
/* The free count derived again from the block map, as a mount derives it;
 * returns how far the Ledger's count had drifted from it. For a startup test
 * that watches a block be given back twice — see the definition. */
int64_t  TagFSAllocatorRederive(void);
#endif

int         tagfs_get_metadata(uint32_t file_id, TagFSMetadata* out);
TagFSState* tagfs_get_state(void);

/* The file laid in one run near `target_block`, its record naming the run in
 * as few extents as a record can say it. OK, or the refusal: the file is left
 * where it was. A file the loader reads by its blocks — wearing system or boot,
 * or holding the kernel the Deed points at — is not moved. */
error_t  tagfs_defrag_file(uint32_t file_id, uint32_t target_block);
/* How fragmented the volume's files are: the share, in percent, of the steps
 * from one of a file's blocks to its next that are a gap — the next run not
 * beginning where the one before it ends — rounded up, so one gap never reads
 * as none. The live files counted and the gaps found are said through
 * out_files and out_gaps when they are not NULL. */
uint32_t tagfs_get_fragmentation_score(uint32_t* out_files, uint32_t* out_gaps);

// ----------------------------------------------------------------------------
// Block I/O (for subsystem use)
// ----------------------------------------------------------------------------

error_t  tagfs_read_block(uint32_t block, void* buffer);
error_t  tagfs_write_block(uint32_t block, const void* buffer);

/* Drop a block from the read-ahead cache (read-after-write consistency).
 * The sync write path calls this internally; the async path (write_job) must
 * call it after each block write since it bypasses tagfs_write_block. */
void     tagfs_readahead_invalidate(uint32_t block);

/*
 * Read one sector run of the mounted volume, counted from the start of its
 * ground rather than from the start of the medium.
 *
 * This is the only door TagFS and everything under it uses to reach the
 * medium — the DiskBook included. The base is applied here and nowhere else,
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
uint16_t tagfs_tag_lookup(const char* tag);
uint16_t TagFSLookupSpelling(const TagSpelling* tag);

/* The id, issued when the volume has none: OK, the spelling's or the
 * registry's refusal, or ERR_TAGFS_NOT_INITIALIZED when there is no volume. */
error_t  tagfs_tag_intern(const char* tag, uint16_t* out_id);
error_t  TagFSInternSpelling(const TagSpelling* tag, uint16_t* out_id);

/* The key of a tag id — "app" out of "app:editor" — copied whole into `out`,
 * or not at all: false when the volume has no such tag or the key would not
 * fit, never a shorter key. */
bool     tagfs_tag_key (uint16_t tag_id, char* out, size_t out_size);

/* The whole text of a tag id — "app:editor" — and its length. It is written
 * into `out` only when it fits whole, NUL and all, and `out` is left empty
 * otherwise, so a caller is never handed a shorter tag than the one it asked
 * for: a caller that must have it measures first (out NULL, size 0). 0 when
 * the volume has no such tag, or there is no volume. */
size_t   tagfs_tag_text(uint16_t tag_id, char* out, size_t out_size);

/* Whether tag `tag_id` is one a pattern names (TagPatternMatches), read in
 * place in the volume's registry. */
bool     TagFSTagMatches(uint16_t tag_id, const TagSpelling* pattern);

/* The keys of `count` tags, comma-joined in a heap string the caller kfree's,
 * whole however long — what a process started from a file wears (it is given
 * the keys, as it always was). An id the volume does not know is passed over.
 * NULL when there is no memory, or no volume. */
char*    TagFSKeyList(const uint16_t* tag_ids, uint32_t count);

/* A tag's key and value as spans into the volume's registry, and whether the
 * volume calls the tag its own — no copy, for a caller that is INSIDE THE
 * DOOR (tagfs_enter) and reads them before it leaves: the registry is freed
 * only once the last caller is out. */
bool     TagFSTagSpelling(uint16_t tag_id, TagSpelling* out, bool* is_system);

// Snapshots: a format-v1 volume answers every request for one ERR_UNSUPPORTED
// (TagFS_SnapshotRefused, tagfs/cow/cow.h); they arrive with format v2.

// Accessor Functions
// ============================================================================

WellKnownTags* tagfs_get_well_known_tags(void);
TagFSState* tagfs_get_state(void);

// ============================================================================
// Test Framework
// ============================================================================

#include "tests/tests.h"

// Run all tests (called from userspace)
error_t TagFS_RunTests(void);

#endif // TAGFS_H
