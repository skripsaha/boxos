#ifndef TAGFS_H
#define TAGFS_H

#include "ktypes.h"
#include "boxos_magic.h"
#include "boxos_limits.h"
#include "boxos_sizes.h"

// TagFS Magic Number and Version
#define TAGFS_MAGIC             BOXOS_TAGFS_MAGIC        // "TAGF"
#define TAGFS_METADATA_MAGIC    BOXOS_TAGFS_META_MAGIC   // "TMET" in little-endian
#define TAGFS_VERSION           2
#define TAGFS_BLOCK_SIZE        BOXOS_PAGE_SIZE
#define TAGFS_MAX_FILES         BOXOS_TAGFS_MAX_FILES
#define TAGFS_MAX_TAGS_PER_FILE 16
#define TAGFS_MAX_FILENAME      32

// Disk layout constants
#define TAGFS_SUPERBLOCK_SECTOR 1034  // After kernel (sectors 10-1033)
#define TAGFS_METADATA_START    1035  // 1034 + 1
#define TAGFS_SUPERBLOCK_BACKUP 2058  // Last metadata sector (1034 + 1024)
// Journal: 2059-3085 (superblock + backup + 512 entries * 2 sectors each)
#define TAGFS_DATA_START        3086  // After journal (2061 + 512*2)

// Compile-time verification that data start doesn't overlap with journal
// Note: JOURNAL_ENTRIES_START and related macros are defined in journal.h
// This assertion will be checked when both headers are included together

// Tag types
#define TAGFS_TAG_USER          0
#define TAGFS_TAG_SYSTEM        1

// File flags
#define TAGFS_FILE_ACTIVE       (1 << 0)
#define TAGFS_FILE_TRASHED      (1 << 1)
#define TAGFS_FILE_HIDDEN       (1 << 2)

// File handle flags
#define TAGFS_HANDLE_READ       (1 << 0)
#define TAGFS_HANDLE_WRITE      (1 << 1)

// ON-DISK STRUCTURES

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
    uint32_t file_id;               // Offset 4: (shifted from 0)
    uint32_t flags;                 // Offset 8: (shifted from 4)
    uint64_t size;                  // Offset 12: (shifted from 8)
    uint32_t start_block;           // Offset 20: (shifted from 16)
    uint32_t block_count;           // Offset 24: (shifted from 20)
    uint64_t created_time;          // Offset 28: (shifted from 24)
    uint64_t modified_time;         // Offset 36: (shifted from 32)
    uint8_t  tag_count;             // Offset 44: (shifted from 40)
    uint8_t  reserved1[3];          // Offset 45: (shifted from 41)
    char filename[32];              // Offset 48: (shifted from 44)
    TagFSTag tags[16];              // Offset 80: (shifted from 76)
    uint8_t  reserved2[48];         // Offset 464: (adjusted size - was 52)
} TagFSMetadata;

STATIC_ASSERT(sizeof(TagFSMetadata) == 512, "TagFSMetadata must be 512 bytes");

// IN-MEMORY STRUCTURES

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
    TagFSMetadata* metadata;
} TagFSFileHandle;

typedef struct {
    uint32_t pid;
    char active_tags[16][64];
    uint32_t tag_count;
} TagFSContext;

typedef struct {
    uint8_t* bitmap;        // Bitmap of block usage (1 bit per block)
    uint32_t total_blocks;  // Total data blocks
} BlockBitmap;

// Bitmap index structures
#define TAGFS_BITMAP_SIZE 128  // 1024 bits / 8 = 128 bytes

typedef struct TagBitmapEntry {
    char tag_string[64];
    uint8_t bitmap[TAGFS_BITMAP_SIZE];
    uint32_t file_count;
    struct TagBitmapEntry* next;
} TagBitmapEntry;

typedef struct TagBitmapIndex {
    TagBitmapEntry* buckets[256];
    uint32_t total_tags;

    // Reverse index: file_id → tags
    struct {
        char** tag_strings;
        uint32_t count;
    } file_tags[TAGFS_MAX_FILES];
} TagBitmapIndex;

typedef struct {
    TagFSSuperblock superblock;
    TagBitmapIndex* tag_index;
    TagFSMetadata* metadata_cache;         // Array of max_files entries
    BlockBitmap block_bitmap;
    bool initialized;
} TagFSState;

// CORE API

int tagfs_init(void);
void tagfs_shutdown(void);
void tagfs_sync(void);
int tagfs_read_superblock(TagFSSuperblock* sb);
int tagfs_write_superblock(const TagFSSuperblock* sb);
int tagfs_read_metadata(uint32_t file_id, TagFSMetadata* metadata);
int tagfs_write_metadata(uint32_t file_id, const TagFSMetadata* metadata);

// Journaled write operations
int tagfs_write_metadata_journaled(uint32_t file_id, const TagFSMetadata* metadata);
int tagfs_write_superblock_journaled(const TagFSSuperblock* sb);

int tagfs_alloc_blocks(uint32_t count, uint32_t* start_block);
int tagfs_free_blocks(uint32_t start_block, uint32_t count);

// FILE OPERATIONS API

// Query files by tags (returns array of file_ids)
int tagfs_query_files(const char* query_tags[], uint32_t tag_count,
                      uint32_t* file_ids, uint32_t max_files);

// Get all active files (no filtering)
int tagfs_list_all_files(uint32_t* file_ids, uint32_t max_files);

TagFSFileHandle* tagfs_open(uint32_t file_id, uint32_t flags);
void tagfs_close(TagFSFileHandle* handle);
int tagfs_read(TagFSFileHandle* handle, void* buffer, uint64_t size);
int tagfs_write(TagFSFileHandle* handle, const void* buffer, uint64_t size);
int tagfs_create_file(const char* filename, const char* tags[], uint32_t tag_count, uint32_t* file_id);
int tagfs_delete_file(uint32_t file_id);
int tagfs_rename_file(uint32_t file_id, const char* new_filename);

// TAG OPERATIONS API

int tagfs_add_tag(uint32_t file_id, const char* key, const char* value, uint8_t type);
int tagfs_remove_tag(uint32_t file_id, const char* key);
bool tagfs_has_tag(uint32_t file_id, const char* key, const char* value);
int tagfs_get_tags(uint32_t file_id, TagFSTag* tags, uint32_t max_tags);

// UTILITY FUNCTIONS

TagFSMetadata* tagfs_get_metadata(uint32_t file_id);
TagFSState* tagfs_get_state(void);

// Format tag string as "key:value"
void tagfs_format_tag(char* dest, const char* key, const char* value);

// Parse "key:value" or single-word system tag
int tagfs_parse_tag(const char* tag_string, char* key, char* value, uint8_t* type);

// DEFRAGMENTATION API

// Defragment file by moving to contiguous blocks
int tagfs_defrag_file(uint32_t file_id, uint32_t target_block);

// Get filesystem fragmentation score (0-100)
uint32_t tagfs_get_fragmentation_score(void);

void tagfs_run_defrag_tests(void);

#endif // TAGFS_H
