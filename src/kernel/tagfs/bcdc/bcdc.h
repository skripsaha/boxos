#ifndef BCDC_H
#define BCDC_H

#include "../../lib/kernel/ktypes.h"
#include "../../lib/kernel/klib.h"
#include "../../core/error/error.h"
#include "../tagfs_constants.h"
#include "../../config/kernel_config.h"

/* ============================================================================
 *  Bcdc — Box Data Compression
 *
 *  Block-aligned output (4 KB), tag-aware dictionary sharing, two algorithms
 *  (LZ77 variant + RLE). Dictionaries are allocated on demand — kernel pays
 *  zero RAM for unused slots.
 *
 *  On-disk format pins dictionary_id to one byte (header.dictionary_id), so
 *  the slot space is bounded at 256. Everything else (per-dict buffer size,
 *  policy table, LZ77 hash chains) is config-tuneable and dynamic.
 * ============================================================================ */

#define BCDC_MAGIC              0x42434443  /* 'BCDC' */
#define BCDC_VERSION            1

#define BCDC_BLOCK_SIZE         4096
#define BCDC_HEADER_SIZE        24
#define BCDC_MAX_COMPRESSED     (BCDC_BLOCK_SIZE - BCDC_HEADER_SIZE)

/* Compression types */
#define BCDC_TYPE_NONE          0
#define BCDC_TYPE_LZ            1
#define BCDC_TYPE_RLE           2

/* Compression levels */
#define BCDC_LEVEL_FAST         1
#define BCDC_LEVEL_DEFAULT      5
#define BCDC_LEVEL_MAX          9

/* Slot space — capped by the 1-byte dictionary_id field in the on-disk
 * header. The runtime never allocates this many actual buffers; the slot
 * array is just 256 pointer slots (~2 KB) and dictionaries materialise on
 * demand. */
#define BCDC_MAX_DICTS          CONFIG_BCDC_MAX_DICTS

/* LZ77 token constants (encoded into the 3-byte match token — see bcdc.c). */
#define BCDC_LZ_MIN_MATCH       3
#define BCDC_LZ_MAX_MATCH       258
#define BCDC_LZ_WINDOW_SIZE     4096

/* RLE constants */
#define BCDC_RLE_MIN_RUN        3
#define BCDC_RLE_ESCAPE         0xFE

/* On-disk block header (24 bytes) */
typedef struct __packed {
    uint32_t magic;
    uint8_t  version;
    uint8_t  compression_type;
    uint16_t flags;

    uint16_t original_size;
    uint16_t compressed_size;

    uint32_t checksum;
    uint32_t original_checksum;

    uint8_t  dictionary_id;
    uint8_t  reserved[3];
} BcdcBlockHeader;

STATIC_ASSERT(sizeof(BcdcBlockHeader) == 24, "BcdcBlockHeader must be 24 bytes");

/*
 * Runtime dictionary record. The buffer (`data`) is sized at creation time
 * (defaults to CONFIG_BCDC_DEFAULT_DICT_SIZE) and may differ between dicts.
 *
 * `lock` covers data updates (slide, pattern preservation). `usage_count`
 * and `last_used` are atomic so the hot compress path doesn't need to take
 * the lock just to bump them.
 */
typedef struct {
    uint16_t            dictionary_id;
    uint16_t            tag_id;
    _Atomic uint32_t    usage_count;
    _Atomic uint64_t    last_used;
    bool                active;
    spinlock_t          lock;
    uint8_t            *data;
    uint16_t            data_size;
} BcdcDictionary;

/* LZ77 match (used internally during compression). */
typedef struct {
    uint16_t offset;
    uint8_t  length;
    uint8_t  literal;
} BcdcLZMatch;

/* Atomic stats — readers see consistent values without taking the global
 * lock. */
typedef struct {
    _Atomic uint64_t blocks_compressed;
    _Atomic uint64_t blocks_decompressed;
    _Atomic uint64_t bytes_before;
    _Atomic uint64_t bytes_after;
    _Atomic uint64_t lz_compressions;
    _Atomic uint64_t rle_compressions;
    _Atomic uint64_t uncompressed_blocks;
    _Atomic uint64_t compression_failures;
    _Atomic uint64_t dict_hits;
    _Atomic uint64_t dict_misses;
} BcdcStats;

/* Per-tag policy. */
typedef struct {
    uint16_t tag_id;
    uint8_t  compression_type;
    uint8_t  compression_level;
    uint8_t  dictionary_sharing;
    uint8_t  reserved;
} BcdcPolicy;

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

error_t BcdcInit(void);
void    BcdcShutdown(void);

error_t BcdcCompress(const void* input, uint16_t input_size,
                     void* output, uint16_t* output_size,
                     uint8_t compression_type, uint8_t level,
                     unsigned int dictionary_id);

error_t BcdcDecompress(const void* input, uint16_t input_size,
                       void* output, uint16_t* output_size,
                       unsigned int dictionary_id);

error_t BcdcCreateDictionary(uint8_t* dict_id, uint16_t tag_id);
error_t BcdcGetDictionary(unsigned int dict_id, BcdcDictionary** out);
error_t BcdcEvictDictionary(unsigned int dict_id);
void    BcdcUpdateDictionaryUsage(unsigned int dict_id);

error_t BcdcSetPolicy(const BcdcPolicy* policy);
error_t BcdcGetPolicy(uint16_t tag_id, BcdcPolicy* out);

void BcdcGetStats(BcdcStats* stats);
void BcdcResetStats(void);

error_t BcdcLZ_Compress(const void* input, uint16_t input_size,
                        void* output, uint16_t* output_size,
                        uint8_t level, const uint8_t* dictionary,
                        uint16_t dictionary_size);

error_t BcdcLZ_Decompress(const void* input, uint16_t input_size,
                          void* output, uint16_t* output_size,
                          const uint8_t* dictionary,
                          uint16_t dictionary_size);

error_t BcdcRLE_Compress(const void* input, uint16_t input_size,
                         void* output, uint16_t* output_size);

error_t BcdcRLE_Decompress(const void* input, uint16_t input_size,
                           void* output, uint16_t* output_size);

uint32_t BcdcComputeChecksum(const void* data, uint16_t size);
bool     BcdcVerifyChecksum(const void* data, uint16_t size, uint32_t expected);

#endif /* BCDC_H */
