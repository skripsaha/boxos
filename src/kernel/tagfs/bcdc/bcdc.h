#ifndef BCDC_H
#define BCDC_H

#include "../../lib/kernel/ktypes.h"
#include "../../lib/kernel/klib.h"
#include "../../core/error/error.h"
#include "../tagfs_constants.h"

// ============================================================================
// Bcdc — Box Data Compression
// Unique Features:
//   • Block-aligned output (no fragmentation)
//   • Tag-aware dictionary sharing
//   • Two algorithms: Bcdc-LZ (general) and Bcdc-RLE (repetitive)
//   • Fixed 4KB compressed blocks
// ============================================================================

#define BCDC_MAGIC              0x42434443  // 'BCDC'
#define BCDC_VERSION            1

// Block size constants
#define BCDC_BLOCK_SIZE         4096        // Fixed block size
#define BCDC_HEADER_SIZE        24          // Block header size
#define BCDC_MAX_COMPRESSED     (BCDC_BLOCK_SIZE - BCDC_HEADER_SIZE)

// Compression types
#define BCDC_TYPE_NONE          0           // Uncompressed
#define BCDC_TYPE_LZ            1           // Bcdc-LZ (LZ77 variant)
#define BCDC_TYPE_RLE           2           // Bcdc-RLE (run-length)

// Compression levels
#define BCDC_LEVEL_FAST         1           // Fast compression
#define BCDC_LEVEL_DEFAULT      5           // Default compression
#define BCDC_LEVEL_MAX          9           // Maximum compression

// Dictionary constants
#define BCDC_DICT_SIZE          8192        // 8KB dictionary
#define BCDC_MAX_DICTS          256         // Max dictionaries
#define BCDC_DICT_LRU_THRESHOLD 1000        // LRU eviction threshold

// LZ77 constants
#define BCDC_LZ_MIN_MATCH       3           // Minimum match length
#define BCDC_LZ_MAX_MATCH       258         // Maximum match length
#define BCDC_LZ_WINDOW_SIZE     4096        // Search window

// RLE constants
#define BCDC_RLE_MIN_RUN        3           // Minimum run length
#define BCDC_RLE_ESCAPE         0xFE        // Escape byte

// Block header (24 bytes)
typedef struct __packed {
    uint32_t magic;              // 'BCDC'
    uint8_t  version;            // Version 1
    uint8_t  compression_type;   // BCDC_TYPE_*
    uint16_t flags;              // Bit flags
    
    uint16_t original_size;      // 1-4096 bytes
    uint16_t compressed_size;    // 1-4080 (0 = uncompressed)
    
    uint32_t checksum;           // BoxHash-CRC32 of compressed data
    uint32_t original_checksum;  // BoxHash-CRC32 of original data
    
    uint8_t  dictionary_id;      // Dictionary ID (0-255)
    uint8_t  reserved[3];        // Alignment
} BcdcBlockHeader;

STATIC_ASSERT(sizeof(BcdcBlockHeader) == 24, "BcdcBlockHeader must be 24 bytes");

// Compression dictionary (shared for same tag)
typedef struct {
    uint8_t  dictionary_id;
    uint8_t  tag_id[2];          // Tag that owns this dictionary
    uint32_t usage_count;        // How many blocks used this dict
    uint8_t  dictionary[BCDC_DICT_SIZE];
    uint64_t last_used;          // For LRU eviction
    bool     active;             // Dictionary is active
} BcdcDictionary;

// LZ77 match structure
typedef struct {
    uint16_t offset;             // Distance back in window
    uint8_t  length;             // Match length (3-258)
    uint8_t  literal;            // Literal byte after match
} BcdcLZMatch;

// Compression statistics
typedef struct {
    uint64_t blocks_compressed;
    uint64_t blocks_decompressed;
    uint64_t bytes_before;
    uint64_t bytes_after;
    uint64_t lz_compressions;
    uint64_t rle_compressions;
    uint64_t uncompressed_blocks;
    uint64_t compression_failures;
    uint64_t dict_hits;
    uint64_t dict_misses;
} BcdcStats;

// Compression policy (per-tag)
typedef struct {
    uint16_t tag_id;
    uint8_t  compression_type;   // BCDC_TYPE_*
    uint8_t  compression_level;  // 1-9
    uint8_t  dictionary_sharing; // 0=off, 1=on
    uint8_t  reserved;
} BcdcPolicy;

// ============================================================================
// Public API
// ============================================================================

// Initialization
error_t BcdcInit(void);
void BcdcShutdown(void);

// Core compression/decompression
error_t BcdcCompress(const void* input, uint16_t input_size,
                     void* output, uint16_t* output_size,
                     uint8_t compression_type, uint8_t level,
                     unsigned int dictionary_id);

error_t BcdcDecompress(const void* input, uint16_t input_size,
                       void* output, uint16_t* output_size,
                       unsigned int dictionary_id);

// Dictionary management
error_t BcdcCreateDictionary(uint8_t* dict_id, uint16_t tag_id);
error_t BcdcGetDictionary(unsigned int dict_id, BcdcDictionary** out);
error_t BcdcEvictDictionary(unsigned int dict_id);
void BcdcUpdateDictionaryUsage(unsigned int dict_id);

// Policy management
error_t BcdcSetPolicy(const BcdcPolicy* policy);
error_t BcdcGetPolicy(uint16_t tag_id, BcdcPolicy* out);

// Statistics
void BcdcGetStats(BcdcStats* stats);
void BcdcResetStats(void);

// Algorithm-specific functions
error_t BcdcLZ_Compress(const void* input, uint16_t input_size,
                        void* output, uint16_t* output_size,
                        uint8_t level, const uint8_t* dictionary);

error_t BcdcLZ_Decompress(const void* input, uint16_t input_size,
                          void* output, uint16_t* output_size,
                          const uint8_t* dictionary);

error_t BcdcRLE_Compress(const void* input, uint16_t input_size,
                         void* output, uint16_t* output_size);

error_t BcdcRLE_Decompress(const void* input, uint16_t input_size,
                           void* output, uint16_t* output_size);

// Utility functions
uint32_t BcdcComputeChecksum(const void* data, uint16_t size);
bool BcdcVerifyChecksum(const void* data, uint16_t size, uint32_t expected);

#endif // BCDC_H
