#ifndef TAGFS_H
#define TAGFS_H

#include "../../lib/kernel/ktypes.h"
#include "../../lib/kernel/klib.h"
#include "tagfs_constants.h"
#include "../../include/boxos_magic.h"
#include "../../include/boxos_limits.h"
#include "../../include/volume_deed.h"
#include "../../include/volume_ledger.h"
#include "../core/error/error.h"

// ============================================================================
// Include Submodule Headers (production implementations)
// ============================================================================

#include "disk_book/disk_book.h"
#include "cow/cow.h"
#include "dedup/dedup.h"
#include "self_heal/self_heal.h"

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
#define TAGFS_FILE_COW              (1 << 3)  // Copy-on-Write enabled

// Handle flags
#define TAGFS_HANDLE_READ           (1 << 0)
#define TAGFS_HANDLE_WRITE          (1 << 1)

// Registry constants
#define TAGFS_REG_BUCKETS           512
#define TAGFS_KEY_BUCKETS           128
#define TAGFS_REGISTRY_DATA_SIZE    4080
#define TAGFS_MPOOL_DATA_SIZE       4080
#define TAGFS_FTABLE_PER_BLOCK      510

// Snapshot limits (production)
#define TAGFS_MAX_SNAPSHOTS         64
#define TAGFS_SNAPSHOT_NAME_LEN     32

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

// Header: 4+4+2+2+4 = 16 bytes. payload[4080] = 4096 total.
// Records are packed variable-length in payload[]:
//   uint16_t record_len | uint32_t file_id | uint32_t flags
//   uint64_t size | uint64_t created_time | uint64_t modified_time
//   uint16_t tag_count | uint16_t extent_count | uint16_t name_len
//   uint16_t tag_ids[tag_count] | FileExtent extents[extent_count]
//   char filename[name_len]
typedef struct __packed {
    uint32_t magic;
    uint32_t next_block;
    uint16_t used_bytes;
    uint16_t record_count;
    uint8_t  reserved[4];
    uint8_t  payload[TAGFS_MPOOL_DATA_SIZE];
} MetaPoolBlock;

STATIC_ASSERT(sizeof(MetaPoolBlock) == 4096, "MetaPoolBlock must be 4096 bytes");

typedef struct __packed {
    uint32_t start_block;
    uint16_t block_count;
} FileExtent;

STATIC_ASSERT(sizeof(FileExtent) == 6, "FileExtent must be 6 bytes");

// ----------------------------------------------------------------------------
// In-Memory Structures: Tag Registry
// ----------------------------------------------------------------------------

typedef struct {
    char*    key;
    char*    value;
    uint8_t  flags;
    uint16_t tag_id;
} TagRegistryEntry;

typedef struct TagRegistryNode {
    uint16_t                tag_id;
    struct TagRegistryNode* next;
} TagRegistryNode;

typedef struct TagKeyGroup {
    char*               key;
    uint16_t*           tag_ids;
    uint32_t            count;
    uint32_t            capacity;
    struct TagKeyGroup* next;
} TagKeyGroup;

typedef struct {
    TagRegistryNode** buckets;
    uint32_t          bucket_count;

    TagRegistryEntry** by_id;
    uint32_t           by_id_capacity;

    uint32_t total_tags;
    uint16_t next_id;

    TagKeyGroup** key_buckets;
    uint32_t      key_bucket_count;

    spinlock_t lock;
} TagRegistry;

// ----------------------------------------------------------------------------
// In-Memory Structures: Bitmap Index
// ----------------------------------------------------------------------------

typedef struct {
    uint8_t* bits;
    uint32_t bit_count;
    uint32_t file_count;
} TagBitmap;

typedef struct {
    uint16_t* ids;
    uint16_t  count;
    uint16_t  capacity;
} TagIdList;

// Query cache: stores recent query results, invalidated by generation counter
#define QUERY_CACHE_SLOTS 16

typedef struct {
    uint32_t  hash;         // hash of sorted tag_ids
    uint64_t  generation;   // generation when cached
    uint32_t* file_ids;     // cached result array
    uint32_t  count;        // number of results
    uint16_t* tag_key;      // copy of sorted tag_ids (for validation)
    uint16_t  tag_count;    // number of tags in key
} QueryCacheEntry;

typedef struct {
    TagBitmap** bitmaps;
    uint32_t    bitmap_capacity;
    uint32_t    max_file_id;

    TagIdList*  file_to_tags;
    uint32_t    file_capacity;

    uint64_t    generation;     // incremented on every mutation
    QueryCacheEntry cache[QUERY_CACHE_SLOTS];

    spinlock_t  lock;
} TagBitmapIndex;

// ----------------------------------------------------------------------------
// In-Memory Structures: Block Allocator
// ----------------------------------------------------------------------------

typedef struct FreeExtent {
    uint32_t           start;
    uint32_t           count;
    struct FreeExtent* next;
} FreeExtent;

typedef struct {
    uint8_t*    bitmap;
    uint32_t    total_blocks;
    FreeExtent* free_list;
    uint32_t    extent_count;
} BlockBitmap;

// ----------------------------------------------------------------------------
// In-Memory Structures: File Handles and Metadata
// ----------------------------------------------------------------------------

// Per-file lock entry in the open file table
#define OPEN_FILE_BUCKETS 32

/* Forward decl: WriteJob lives in storage/async/write_job.h. The OFE
 * carries a token-handoff slot so concurrent async writers serialize
 * without holding write_lock across an IO yield. */
struct WriteJob;

typedef struct OpenFileEntry {
    uint32_t              file_id;
    uint32_t              ref_count;
    spinlock_t            write_lock;        /* sync path only */
    struct WriteJob *     async_write_owner; /* token (NULL = free) */
    struct WriteJob *     async_pending_head;/* FIFO of waiters */
    spinlock_t            async_token_lock;  /* guards above two fields */
    struct OpenFileEntry* next;
} OpenFileEntry;

typedef struct {
    uint32_t    file_id;
    uint32_t    flags;
    uint64_t    offset;
    uint64_t    file_size;
    FileExtent* extents;
    uint16_t    extent_count;
    OpenFileEntry* ofe;   // back-pointer to open file entry (for locking)
} TagFSFileHandle;

typedef struct {
    uint32_t    file_id;
    uint32_t    flags;
    uint64_t    size;
    uint64_t    created_time;
    uint64_t    modified_time;
    char*       filename;
    uint16_t*   tag_ids;
    uint16_t    tag_count;
    FileExtent* extents;
    uint16_t    extent_count;
} TagFSMetadata;

// ----------------------------------------------------------------------------
// In-Memory Structures: Process Context and Global State
// ----------------------------------------------------------------------------

typedef struct {
    uint32_t  pid;
    uint64_t  context_bits;
    uint16_t* overflow_ids;
    uint16_t  overflow_count;
    uint16_t  overflow_capacity;
} TagFSContext;

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
    VolumeLedger     ledger;        /* what is true of it today */

    TagRegistry*     registry;
    TagBitmapIndex*  bitmap_index;
    BlockBitmap      block_bitmap;
    bool             initialized;
    spinlock_t       lock;
} TagFSState;

// trashed/hidden membership masks (populated by tagfs_init_well_known_tags).
// Each field stores (1ULL << tag_id) for the file-listing post-filter; zero
// means the tag was not found in the registry. These are membership filters,
// NOT security. The 7 auth-privilege tags moved to cabin_t.auth_bits (fixed
// bits, auth_tags.h) and no longer depend on the registry id.
typedef struct {
    uint64_t trashed;
    uint64_t hidden;
} WellKnownTags;

// Accessor functions (no extern needed)
WellKnownTags* tagfs_get_well_known_tags(void);

// ----------------------------------------------------------------------------
// Tag Registry API
// ----------------------------------------------------------------------------

int          tag_registry_init(TagRegistry* reg);
void         tag_registry_destroy(TagRegistry* reg);
uint16_t     tag_registry_intern(TagRegistry* reg, const char* key, const char* value);
uint16_t     tag_registry_lookup(TagRegistry* reg, const char* key, const char* value);
void         tag_registry_mark_system(TagRegistry* reg, uint16_t tag_id);
bool         tag_registry_is_system(TagRegistry* reg, uint16_t tag_id);
const char*  tag_registry_key(TagRegistry* reg, uint16_t tag_id);
const char*  tag_registry_value(TagRegistry* reg, uint16_t tag_id);
TagKeyGroup* tag_registry_key_group(TagRegistry* reg, const char* key);
int          tag_registry_flush(TagRegistry* reg);
bool         tag_registry_is_dirty(void);
int          tag_registry_load(TagRegistry* reg, uint32_t first_block);

// ----------------------------------------------------------------------------
// Bitmap Index API
// ----------------------------------------------------------------------------

TagBitmapIndex* tag_bitmap_create(uint32_t initial_tag_cap, uint32_t initial_file_cap);
void            tag_bitmap_destroy(TagBitmapIndex* idx);
int             tag_bitmap_set(TagBitmapIndex* idx, uint16_t tag_id, uint32_t file_id);
int             tag_bitmap_clear(TagBitmapIndex* idx, uint16_t tag_id, uint32_t file_id);
void            tag_bitmap_remove_file(TagBitmapIndex* idx, uint32_t file_id);
int             tag_bitmap_query(TagBitmapIndex* idx,
                    const uint16_t* tag_ids, uint32_t tag_count,
                    TagKeyGroup** groups, uint32_t group_count,
                    uint32_t* out_file_ids, uint32_t max_results);
int             tag_bitmap_tags_for_file(TagBitmapIndex* idx, uint32_t file_id,
                    uint16_t* out_ids, uint32_t max_ids);
int             tag_bitmap_tag_count_for_file(TagBitmapIndex* idx, uint32_t file_id);

// ----------------------------------------------------------------------------
// File Table API
// ----------------------------------------------------------------------------

int  file_table_init(uint32_t first_block, uint32_t block_count);
void file_table_shutdown(bool write_back);
int  file_table_lookup(uint32_t file_id, uint32_t* out_block, uint32_t* out_offset);
int  file_table_update(uint32_t file_id, uint32_t meta_block, uint32_t meta_offset);
int  file_table_delete(uint32_t file_id);
int  file_table_flush(void);

// ----------------------------------------------------------------------------
// Metadata Pool API
// ----------------------------------------------------------------------------

int      meta_pool_init(uint32_t first_block, uint32_t block_count);
void     meta_pool_shutdown(bool write_back);
int      meta_pool_read(uint32_t block, uint32_t offset, TagFSMetadata* out);
int      meta_pool_write(const TagFSMetadata* meta, uint32_t* out_block, uint32_t* out_offset);
int      meta_pool_delete(uint32_t block, uint32_t offset);
int      meta_pool_mirror_init(uint32_t max_file_id);
int      meta_pool_read_cached(uint32_t file_id, TagFSMetadata* out);
void     tagfs_metadata_free(TagFSMetadata* meta);
uint32_t meta_pool_record_size(const TagFSMetadata* meta);
int      meta_pool_flush(void);

// ----------------------------------------------------------------------------
// Main TagFS API
// ----------------------------------------------------------------------------

error_t  tagfs_init(void);
uint8_t  tagfs_get_seat(void);       /* Boardroom seat the volume lives on */

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
void tagfs_shutdown(void);
void tagfs_sync(void);

/* Flush the volatile write cache of the disk that holds the TagFS volume
 * (routes to the probed AHCI port / ATA drive, not a hardcoded device). */
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

int  tagfs_create_file(const char* filename, const uint16_t* tag_ids, uint16_t tag_count,
                       uint32_t* out_file_id);
int  tagfs_delete_file(uint32_t file_id);
int  tagfs_rename_file(uint32_t file_id, const char* new_filename);

/* Drop everything past `new_size`. SHRINK ONLY — a request to grow is
 * refused (-ERR_INVALID_ARGUMENT) because freshly allocated blocks are not
 * zeroed, and refused outright on a snapshotted file (-ERR_INVALID_OPERATION)
 * because a not-yet-copied block is still the snapshot's only copy. Returns 0
 * or a negative -ERR_*. See the comment above the definition. */
int  tagfs_truncate_file(uint32_t file_id, uint64_t new_size);

int  tagfs_add_tag(uint32_t file_id, uint16_t tag_id);
int  tagfs_remove_tag(uint32_t file_id, uint16_t tag_id);
bool tagfs_has_tag(uint32_t file_id, uint16_t tag_id);

int  tagfs_add_tag_string(uint32_t file_id, const char* key, const char* value);
int  tagfs_remove_tag_string(uint32_t file_id, const char* key);
bool tagfs_has_tag_string(uint32_t file_id, const char* key, const char* value);

bool tagfs_key_is_reserved(const char *key);

int  tagfs_query_files(const char* query_strings[], uint32_t count,
                       uint32_t* out_file_ids, uint32_t max_results);
int  tagfs_list_all_files(uint32_t* out_file_ids, uint32_t max_results);

TagFSFileHandle* tagfs_open(uint32_t file_id, uint32_t flags);
void             tagfs_close(TagFSFileHandle* handle);
int              tagfs_read(TagFSFileHandle* handle, void* buffer, uint64_t size);
int              tagfs_write(TagFSFileHandle* handle, const void* buffer, uint64_t size);

int      tagfs_alloc_blocks(uint32_t count, uint32_t* out_start_block);
int      tagfs_free_blocks(uint32_t start_block, uint32_t count);

int         tagfs_get_metadata(uint32_t file_id, TagFSMetadata* out);
TagFSState* tagfs_get_state(void);

int      tagfs_defrag_file(uint32_t file_id, uint32_t target_block);
uint32_t tagfs_get_fragmentation_score(void);

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
 * Write the Ledger back — the counters, not the identity.
 *
 * It goes to whichever of the two copies is currently older, with a sequence
 * number one higher than the newer one, so the copy that is intact is never
 * the copy being overwritten. See volume_ledger.h.
 */
error_t  tagfs_write_ledger(void);

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

// ----------------------------------------------------------------------------
// Context API
// ----------------------------------------------------------------------------

void     tagfs_context_init(void);
int      tagfs_context_add_tag(uint32_t pid, uint16_t tag_id);
int      tagfs_context_add_tag_string(uint32_t pid, const char* key, const char* value);
void     tagfs_context_clear(uint32_t pid);
bool     tagfs_context_matches_file(uint32_t pid, uint32_t file_id);
void     tagfs_context_destroy(uint32_t pid);
uint64_t tagfs_context_get_bits(uint32_t pid);
int      tagfs_context_get_tags(uint32_t pid, const char* tags[], uint32_t max_tags);

// ----------------------------------------------------------------------------
// Tag String Helpers
// ----------------------------------------------------------------------------

void tagfs_format_tag(char* dest, size_t dest_size, const char* key, const char* value);
int  tagfs_parse_tag(const char* tag_string, char* key, size_t key_size,
                     char* value, size_t value_size);

// Snapshots: see the CoW snapshot API (TagFS_Snapshot*) in tagfs/cow/cow.h.
// The former in-memory-only tagfs_snapshot_* API (snapshot.c) was unused and
// has been removed.

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
