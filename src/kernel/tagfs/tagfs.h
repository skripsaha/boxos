#ifndef TAGFS_H
#define TAGFS_H

#include "ktypes.h"
#include "boxos_magic.h"
#include "boxos_limits.h"
#include "boxos_sizes.h"
#include "klib.h"

#define TAGFS_METADATA_MAGIC    TAGFS_META_MAGIC
#define TAGFS_VERSION           2
#define TAGFS_BLOCK_SIZE        PAGE_SIZE
#define TAGFS_MAX_TAGS_PER_FILE 16
#define TAGFS_MAX_FILENAME      32

#define TAGFS_SUPERBLOCK_SECTOR 1034  // After kernel (sectors 10-1033)
#define TAGFS_METADATA_START    1035  // 1034 + 1
#define TAGFS_SUPERBLOCK_BACKUP 2058  // Last metadata sector (1034 + 1024)
// Journal: 2059-3085 (superblock + backup + 512 entries * 2 sectors each)
#define TAGFS_DATA_START        3086  // After journal (2061 + 512*2)

#define TAGFS_TAG_USER          0
#define TAGFS_TAG_SYSTEM        1

#define TAGFS_FILE_ACTIVE       (1 << 0)
#define TAGFS_FILE_TRASHED      (1 << 1)
#define TAGFS_FILE_HIDDEN       (1 << 2)

#define TAGFS_HANDLE_READ       (1 << 0)
#define TAGFS_HANDLE_WRITE      (1 << 1)

typedef struct __packed {
    uint8_t type;           // 0=user, 1=system
    char key[11];           // Tag key (null-terminated)
    char value[12];         // Tag value (null-terminated)
} TagFSTag;

STATIC_ASSERT(sizeof(TagFSTag) == 24, "TagFSTag must be 24 bytes");

typedef struct __packed {
    uint32_t magic;                     // 0x54414746 ("TAGF")
    uint32_t version;                   // 2
    uint32_t block_size;                // 4096
    uint32_t total_blocks;
    uint32_t free_blocks;
    uint32_t total_files;
    uint32_t max_files;                 // 1024
    uint32_t metadata_start_sector;     // 401
    uint32_t data_start_sector;         // 1425
    uint32_t tag_index_block;
    uint64_t fs_created_time;
    uint64_t fs_modified_time;
    uint8_t  fs_uuid[16];
    uint8_t  reserved[440];
} TagFSSuperblock;

STATIC_ASSERT(sizeof(TagFSSuperblock) == 512, "TagFSSuperblock must be 512 bytes");

typedef struct __packed {
    uint32_t magic;                 // Offset 0: TAGFS_METADATA_MAGIC
    uint32_t file_id;               // Offset 4
    uint32_t flags;                 // Offset 8
    uint64_t size;                  // Offset 12
    uint32_t start_block;           // Offset 20
    uint32_t block_count;           // Offset 24
    uint64_t created_time;          // Offset 28
    uint64_t modified_time;         // Offset 36
    uint8_t  tag_count;             // Offset 44: total tags (inline only for v2)
    uint8_t  ext_tag_count;         // Offset 45: tags in extended block (0 for v2)
    uint8_t  reserved1[2];          // Offset 46
    char filename[32];              // Offset 48
    TagFSTag tags[16];              // Offset 80: inline tags (v2 compatible)
    uint32_t extended_block;        // Offset 464: block# for overflow tags (0=none, v2 compat)
    uint8_t  reserved2[44];         // Offset 468: padding
} TagFSMetadata;

STATIC_ASSERT(sizeof(TagFSMetadata) == 512, "TagFSMetadata must be 512 bytes");

// Extended metadata block — lives in data region, 4096 bytes (1 block)
// Stores overflow tags with variable-length keys and values.
// Backward compatible: v2 files have extended_block=0 (no overflow).
#define TAGFS_EXT_MAGIC     0x54455854  // "TEXT"
#define TAGFS_EXT_DATA_SIZE 4080

typedef struct __packed {
    uint32_t magic;             // TAGFS_EXT_MAGIC
    uint32_t file_id;           // Cross-reference
    uint16_t tag_count;         // Tags in this block
    uint16_t used_bytes;        // Bytes used in data[]
    uint32_t next_block;        // Chain to next extended block (0=end)
    uint8_t  data[TAGFS_EXT_DATA_SIZE];  // Variable-length tag entries
} TagFSExtendedMeta;

STATIC_ASSERT(sizeof(TagFSExtendedMeta) == 4096, "TagFSExtendedMeta must be 4096 bytes");

// Variable-length tag entry format in extended block data[]:
// [1 byte type] [1 byte key_len] [2 bytes value_len] [key_len bytes key] [value_len bytes value]
// Key: up to 255 chars, Value: up to 65535 chars

typedef struct TagIndexEntry {
    char tag_string[64];                // "key:value"
    uint32_t file_count;
    uint32_t* file_ids;                 // Dynamic array
    struct TagIndexEntry* next;
} TagIndexEntry;

typedef struct {
    TagIndexEntry* buckets[256];
    uint32_t total_tags;
} TagIndex;

typedef struct {
    uint32_t file_id;
    uint32_t flags;
    uint64_t offset;
    TagFSMetadata metadata;                // Copy of metadata (not pointer — safe from LRU eviction)
} TagFSFileHandle;

typedef struct {
    uint32_t pid;
    char** active_tags;                     // Dynamically allocated array of tag strings
    uint32_t tag_count;
    uint32_t tag_capacity;                  // Current capacity (grows x2)
} TagFSContext;

// Free extent node — sorted linked list by start address
// Used for O(k) block allocation instead of O(n) bitmap scan
typedef struct FreeExtent {
    uint32_t start;           // Starting block number
    uint32_t count;           // Number of contiguous free blocks
    struct FreeExtent* next;  // Next extent (higher address)
} FreeExtent;

typedef struct {
    uint8_t* bitmap;          // Bitmap of block usage (1 bit per block)
    uint32_t total_blocks;    // Total data blocks
    FreeExtent* free_list;    // Sorted free extent list for O(k) allocation
    uint32_t extent_count;    // Number of free extents (for diagnostics)
} BlockBitmap;

// TAGFS_BITMAP_SIZE is now computed dynamically from max_files:
// bitmap_size = (max_files + 7) / 8
// The old fixed value of 128 (1024 bits) is no longer used.

// Compact per-file summary — always in RAM for fast iteration
typedef struct {
    uint32_t file_id;
    uint32_t flags;
    uint32_t start_block;
    uint32_t block_count;
} TagFSFileSummary;  // 16 bytes per file

// Forward declaration for LRU metadata cache
struct MetadataLRU;

typedef struct TagBitmapEntry {
    char tag_string[64];
    uint8_t* bitmap;                        // Dynamically allocated: (max_files + 7) / 8 bytes
    uint32_t bitmap_size;                   // Size in bytes
    uint32_t file_count;
    struct TagBitmapEntry* next;
} TagBitmapEntry;

typedef struct {
    char** tag_strings;
    uint32_t count;
} TagFileTagList;

typedef struct TagBitmapIndex {
    TagBitmapEntry* buckets[256];
    uint32_t total_tags;
    uint32_t max_files;                     // Dynamic limit from superblock

    // Reverse index: file_id → tags (dynamically allocated array)
    TagFileTagList* file_tags;
} TagBitmapIndex;

typedef struct {
    TagFSSuperblock superblock;
    TagBitmapIndex* tag_index;
    struct MetadataLRU* mcache;            // LRU cache for full metadata
    TagFSFileSummary* file_summaries;      // Compact summary for all files (16 bytes each)
    BlockBitmap block_bitmap;
    uint32_t max_files;                    // Dynamic limit from superblock
    bool initialized;
    spinlock_t lock;
} TagFSState;

int tagfs_init(void);
void tagfs_shutdown(void);
void tagfs_sync(void);
int tagfs_read_superblock(TagFSSuperblock* sb);
int tagfs_write_superblock(const TagFSSuperblock* sb);
int tagfs_read_metadata(uint32_t file_id, TagFSMetadata* metadata);
int tagfs_write_metadata(uint32_t file_id, const TagFSMetadata* metadata);

int tagfs_write_metadata_journaled(uint32_t file_id, const TagFSMetadata* metadata);
int tagfs_write_superblock_journaled(const TagFSSuperblock* sb);

int tagfs_alloc_blocks(uint32_t count, uint32_t* start_block);
int tagfs_free_blocks(uint32_t start_block, uint32_t count);

int tagfs_query_files(const char* query_tags[], uint32_t tag_count,
                      uint32_t* file_ids, uint32_t max_files);

int tagfs_list_all_files(uint32_t* file_ids, uint32_t max_files);

TagFSFileHandle* tagfs_open(uint32_t file_id, uint32_t flags);
void tagfs_close(TagFSFileHandle* handle);
int tagfs_read(TagFSFileHandle* handle, void* buffer, uint64_t size);
int tagfs_write(TagFSFileHandle* handle, const void* buffer, uint64_t size);
int tagfs_create_file(const char* filename, const char* tags[], uint32_t tag_count, uint32_t* file_id);
int tagfs_delete_file(uint32_t file_id);
int tagfs_rename_file(uint32_t file_id, const char* new_filename);

int tagfs_add_tag(uint32_t file_id, const char* key, const char* value, uint8_t type);
int tagfs_remove_tag(uint32_t file_id, const char* key);
bool tagfs_has_tag(uint32_t file_id, const char* key, const char* value);
int tagfs_get_tags(uint32_t file_id, TagFSTag* tags, uint32_t max_tags);

// Extended metadata: overflow tags beyond inline 16
int tagfs_add_tag_extended(uint32_t file_id, const char* key, const char* value, uint8_t type);
int tagfs_read_extended_tags(uint32_t file_id, TagFSExtendedMeta* ext);

TagFSMetadata* tagfs_get_metadata(uint32_t file_id);
TagFSState* tagfs_get_state(void);

void tagfs_format_tag(char* dest, const char* key, const char* value);

int tagfs_parse_tag(const char* tag_string, char* key, char* value, uint8_t* type);

int tagfs_defrag_file(uint32_t file_id, uint32_t target_block);

uint32_t tagfs_get_fragmentation_score(void);

#endif // TAGFS_H
