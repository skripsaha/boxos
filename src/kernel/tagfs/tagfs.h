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


#include "disk_book/disk_book.h"
#include "cow/cow.h"
#include "dedup/dedup.h"
#include "self_heal/self_heal.h"


#define TAGFS_VERSION               1
#define TAGFS_BLOCK_SIZE            4096


#define TAGFS_INVALID_TAG_ID        0xFFFF
#define TAGFS_MAX_TAG_ID            0x7FFE

#define TAGFS_FILE_ACTIVE           (1 << 0)
#define TAGFS_FILE_TRASHED          (1 << 1)
#define TAGFS_FILE_HIDDEN           (1 << 2)
#define TAGFS_FILE_COW              (1 << 3)

#define TAGFS_HANDLE_READ           (1 << 0)
#define TAGFS_HANDLE_WRITE          (1 << 1)

#define TAGFS_REG_BUCKETS           512
#define TAGFS_KEY_BUCKETS           128
#define TAGFS_REGISTRY_DATA_SIZE    4080
#define TAGFS_MPOOL_DATA_SIZE       4080
#define TAGFS_FTABLE_PER_BLOCK      510

#define TAGFS_MAX_SNAPSHOTS         64
#define TAGFS_SNAPSHOT_NAME_LEN     32


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

typedef struct __packed {
    uint32_t       magic;
    uint32_t       next_block;
    uint32_t       entry_count;
    uint32_t       reserved;
    FileTableEntry entries[TAGFS_FTABLE_PER_BLOCK];
} FileTableBlock;

STATIC_ASSERT(sizeof(FileTableBlock) == 4096, "FileTableBlock must be 4096 bytes");

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

typedef struct __packed {
    uint32_t start_block;
    uint16_t block_count;
} FileExtent;

STATIC_ASSERT(sizeof(FileExtent) == 6, "FileExtent must be 6 bytes");


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

#define QUERY_CACHE_SLOTS 16

typedef struct {
    uint32_t  hash;
    uint64_t  generation;
    uint32_t* file_ids;
    uint32_t  count;
    uint16_t* tag_key;
    uint16_t  tag_count;
} QueryCacheEntry;

typedef struct {
    TagBitmap** bitmaps;
    uint32_t    bitmap_capacity;
    uint32_t    max_file_id;

    TagIdList*  file_to_tags;
    uint32_t    file_capacity;

    uint64_t    generation;
    QueryCacheEntry cache[QUERY_CACHE_SLOTS];

    spinlock_t  lock;
} TagBitmapIndex;


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


#define OPEN_FILE_BUCKETS 32

struct WriteJob;

typedef struct OpenFileEntry {
    uint32_t              file_id;
    uint32_t              ref_count;
    spinlock_t            write_lock;
    struct WriteJob *     async_write_owner;
    struct WriteJob *     async_pending_head;
    spinlock_t            async_token_lock;
    struct OpenFileEntry* next;
} OpenFileEntry;

typedef struct {
    uint32_t    file_id;
    uint32_t    flags;
    uint64_t    offset;
    uint64_t    file_size;
    FileExtent* extents;
    uint16_t    extent_count;
    OpenFileEntry* ofe;

    uint32_t    mount_epoch;
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


typedef struct {
    uint8_t          uuid[16];
    VolumeLayout     layout;
    VolumeGeometry   geometry;
    VolumeLedger     ledger;

    TagRegistry*     registry;
    TagBitmapIndex*  bitmap_index;
    BlockBitmap      block_bitmap;
    bool             initialized;
    spinlock_t       lock;
} TagFSState;

typedef struct {
    uint64_t trashed;
    uint64_t hidden;
} WellKnownTags;

WellKnownTags* tagfs_get_well_known_tags(void);


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


int  file_table_init(uint32_t first_block);
void file_table_shutdown(bool write_back);
int  file_table_lookup(uint32_t file_id, uint32_t* out_block, uint32_t* out_offset);
int  file_table_update(uint32_t file_id, uint32_t meta_block, uint32_t meta_offset);
int  file_table_delete(uint32_t file_id);
int  file_table_flush(void);


int      meta_pool_init(uint32_t first_block);
void     meta_pool_shutdown(bool write_back);
int      meta_pool_read(uint32_t block, uint32_t offset, TagFSMetadata* out);
int      meta_pool_write(const TagFSMetadata* meta, uint32_t* out_block, uint32_t* out_offset);
int      meta_pool_delete(uint32_t block, uint32_t offset);
int      meta_pool_mirror_init(uint32_t max_file_id);
int      meta_pool_read_cached(uint32_t file_id, TagFSMetadata* out);
void     tagfs_metadata_free(TagFSMetadata* meta);
uint32_t meta_pool_record_size(const TagFSMetadata* meta);
int      meta_pool_flush(void);


error_t  tagfs_init(void);
uint8_t  tagfs_get_seat(void);

bool     tagfs_enter(void);
void     tagfs_leave(void);

uint32_t tagfs_mount_epoch(void);
bool     tagfs_handle_is_of_this_mount(const TagFSFileHandle* handle);

void     TagFSServiceIfPending(void);

uint64_t tagfs_get_volume_base(void);

void     TagFSWatchSeats(void);

void     TagFSAttendArrival(void);

void     TagFSNoteMediumGone(void);

void     TagFSBootMountSettled(void);
void tagfs_shutdown(void);
void tagfs_sync(void);

error_t  tagfs_flush_cache(void);

uint64_t tagfs_block_to_sector(uint32_t block);


int  tagfs_create_file(const char* filename, const uint16_t* tag_ids, uint16_t tag_count,
                       uint32_t* out_file_id);
int  tagfs_delete_file(uint32_t file_id);
int  tagfs_rename_file(uint32_t file_id, const char* new_filename);

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
uint32_t tagfs_file_ceiling(void);

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


error_t  tagfs_read_block(uint32_t block, void* buffer);
error_t  tagfs_write_block(uint32_t block, const void* buffer);

void     tagfs_readahead_invalidate(uint32_t block);

error_t  tagfs_write_ledger(void);

error_t  tagfs_remember_use_context(const char *list, size_t len, bool *remembered);
size_t   tagfs_recall_use_context(char *buf, size_t cap);
bool     tagfs_ledger_peek(uint32_t copy, VolumeLedger *out, char *tail, uint16_t *tail_len);

int      tagfs_volume_read (uint64_t vlba, uint32_t count, void* buffer);
int      tagfs_volume_write(uint64_t vlba, uint32_t count, const void* buffer);


void tagfs_format_tag(char* dest, size_t dest_size, const char* key, const char* value);
int  tagfs_parse_tag(const char* tag_string, char* key, size_t key_size,
                     char* value, size_t value_size);

uint16_t tagfs_tag_lookup(const char* tag);
uint16_t tagfs_tag_intern(const char* tag);
bool     tagfs_tag_key (uint16_t tag_id, char* out, size_t out_size);
bool     tagfs_tag_text(uint16_t tag_id, char* out, size_t out_size);
bool     tagfs_tag_parts(uint16_t tag_id, char* key, size_t key_size,
                         char* value, size_t value_size, bool* is_system);



WellKnownTags* tagfs_get_well_known_tags(void);
TagFSState* tagfs_get_state(void);


#include "tests/tests.h"

error_t TagFS_RunTests(void);

#endif