#include "bcdc.h"
#include "../tagfs.h"
#include "../../../kernel/drivers/timer/rtc.h"

// ============================================================================
// Global State
// ============================================================================

static BcdcDictionary g_bcdc_dictionaries[BCDC_MAX_DICTS];
static uint8_t g_bcdc_dict_count = 0;
static BcdcPolicy g_bcdc_policies[64];  // Max 64 tag policies
static uint8_t g_bcdc_policy_count = 0;
static BcdcStats g_bcdc_stats;
static bool g_bcdc_initialized = false;
static spinlock_t g_bcdc_lock;

// ============================================================================
// Checksum (BoxHash-CRC32 variant)
// ============================================================================

uint32_t BcdcComputeChecksum(const void* data, uint16_t size) {
    const uint32_t poly = 0xEDB88320;
    uint32_t crc = 0xFFFFFFFF;
    const uint8_t* bytes = (const uint8_t*)data;
    
    for (uint16_t i = 0; i < size; i++) {
        crc ^= bytes[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (poly & -(crc & 1));
        }
    }
    return ~crc;
}

bool BcdcVerifyChecksum(const void* data, uint16_t size, uint32_t expected) {
    return BcdcComputeChecksum(data, size) == expected;
}

// ============================================================================
// Dictionary Management
// ============================================================================

error_t BcdcInit(void) {
    if (g_bcdc_initialized)
        return ERR_ALREADY_INITIALIZED;
    
    spinlock_init(&g_bcdc_lock);
    
    // Initialize dictionaries
    memset(g_bcdc_dictionaries, 0, sizeof(g_bcdc_dictionaries));
    g_bcdc_dict_count = 0;
    
    // Initialize policies
    memset(g_bcdc_policies, 0, sizeof(g_bcdc_policies));
    g_bcdc_policy_count = 0;
    
    // Initialize stats
    memset(&g_bcdc_stats, 0, sizeof(g_bcdc_stats));
    
    // Create default dictionary (ID 0)
    g_bcdc_dictionaries[0].dictionary_id = 0;
    g_bcdc_dictionaries[0].active = true;
    g_bcdc_dict_count = 1;
    
    g_bcdc_initialized = true;
    
    debug_printf("[Bcdc] Initialized: %u dictionaries\n", g_bcdc_dict_count);
    return OK;
}

void BcdcShutdown(void) {
    if (!g_bcdc_initialized)
        return;
    
    spin_lock(&g_bcdc_lock);
    g_bcdc_initialized = false;
    spin_unlock(&g_bcdc_lock);
    
    debug_printf("[Bcdc] Shutdown complete\n");
}

error_t BcdcCreateDictionary(uint8_t* dict_id, uint16_t tag_id) {
    if (!g_bcdc_initialized || !dict_id)
        return ERR_NOT_INITIALIZED;

    spin_lock(&g_bcdc_lock);

    // Find free slot
    unsigned int slot = 0;
    for (unsigned int i = 0; i < BCDC_MAX_DICTS; i++) {
        if (!g_bcdc_dictionaries[i].active) {
            slot = i;
            break;
        }
    }

    if (slot >= BCDC_MAX_DICTS) {
        // Evict LRU dictionary
        uint64_t oldest = UINT64_MAX;
        for (unsigned int i = 0; i < BCDC_MAX_DICTS; i++) {
            if (g_bcdc_dictionaries[i].active &&
                g_bcdc_dictionaries[i].last_used < oldest) {
                oldest = g_bcdc_dictionaries[i].last_used;
                slot = i;
            }
        }
    }

    // Initialize dictionary
    BcdcDictionary* dict = &g_bcdc_dictionaries[slot];
    memset(dict, 0, sizeof(BcdcDictionary));
    dict->dictionary_id = slot;
    dict->tag_id[0] = tag_id & 0xFF;
    dict->tag_id[1] = (tag_id >> 8) & 0xFF;
    dict->active = true;
    dict->last_used = rtc_get_unix64();
    dict->usage_count = 0;

    *dict_id = slot;
    if (slot >= g_bcdc_dict_count)
        g_bcdc_dict_count = slot + 1;

    spin_unlock(&g_bcdc_lock);

    debug_printf("[Bcdc] Created dictionary %u for tag %u\n", slot, tag_id);
    return OK;
}

// Update dictionary with new data - aggressive learning from patterns
static void BcdcUpdateDictionary(const uint8_t* data, uint16_t size) {
    if (!data || size < 4)
        return;

    // Find active dictionary with most usage
    int best_dict = -1;
    uint32_t best_usage = 0;

    for (unsigned int i = 0; i < g_bcdc_dict_count; i++) {
        if (g_bcdc_dictionaries[i].active &&
            g_bcdc_dictionaries[i].usage_count > best_usage) {
            best_usage = g_bcdc_dictionaries[i].usage_count;
            best_dict = i;
        }
    }

    if (best_dict < 0)
        return;

    BcdcDictionary* dict = &g_bcdc_dictionaries[best_dict];

    // Aggressive learning: extract most frequent 4-byte patterns
    // and insert them into dictionary at strategic positions
    if (size >= BCDC_DICT_SIZE) {
        // Data larger than dictionary - use frequency analysis
        // Count 4-byte pattern frequencies
        uint32_t best_pos = 0;
        uint32_t best_freq = 0;

        for (uint16_t p = 0; p + 4 <= size; p++) {
            uint32_t freq = 0;
            for (uint16_t q = 0; q + 4 <= size; q++) {
                if (data[p] == data[q] &&
                    data[p+1] == data[q+1] &&
                    data[p+2] == data[q+2] &&
                    data[p+3] == data[q+3]) {
                    freq++;
                }
            }
            if (freq > best_freq) {
                best_freq = freq;
                best_pos = p;
            }
        }

        // Copy most frequent pattern to center of dictionary
        uint16_t pattern_size = (size - best_pos > 64) ? 64 : (size - best_pos);
        uint16_t dict_pos = (BCDC_DICT_SIZE - pattern_size) / 2;
        memcpy(dict->dictionary + dict_pos, data + best_pos, pattern_size);

        // Also copy tail data for context
        uint16_t tail_size = (size > 128) ? 128 : size;
        memcpy(dict->dictionary, data + size - tail_size, tail_size);
    } else {
        // Small data - use sliding window with pattern preservation
        uint16_t keep = BCDC_DICT_SIZE - size;

        // Before sliding, find and preserve frequent patterns
        if (keep > 4) {
            // Scan for repeated sequences in existing dictionary
            for (uint16_t p = 0; p < keep - 4; p++) {
                uint32_t freq = 0;
                for (uint16_t q = p + 4; q < keep; q++) {
                    if (dict->dictionary[p] == dict->dictionary[q] &&
                        dict->dictionary[p+1] == dict->dictionary[q+1] &&
                        dict->dictionary[p+2] == dict->dictionary[q+2] &&
                        dict->dictionary[p+3] == dict->dictionary[q+3]) {
                        freq++;
                        // Extend match
                        uint16_t len = 4;
                        while (p + len < keep && q + len < keep &&
                               dict->dictionary[p+len] == dict->dictionary[q+len] &&
                               len < 32) {
                            len++;
                        }
                        if (len > 4) {
                            // Move frequent pattern to front
                            memmove(dict->dictionary + 4, dict->dictionary, p);
                            memcpy(dict->dictionary, dict->dictionary + q, len);
                            break;
                        }
                    }
                }
            }
        }

        // Slide and append new data
        memmove(dict->dictionary, dict->dictionary + size, keep);
        memcpy(dict->dictionary + keep, data, size);
    }
}

error_t BcdcGetDictionary(unsigned int dict_id, BcdcDictionary** out) {
    if (!g_bcdc_initialized || !out || dict_id >= BCDC_MAX_DICTS)
        return ERR_INVALID_ARGUMENT;

    spin_lock(&g_bcdc_lock);

    if (!g_bcdc_dictionaries[dict_id].active) {
        spin_unlock(&g_bcdc_lock);
        return ERR_OBJECT_NOT_FOUND;
    }

    *out = &g_bcdc_dictionaries[dict_id];
    spin_unlock(&g_bcdc_lock);

    return OK;
}

error_t BcdcEvictDictionary(unsigned int dict_id) {
    if (!g_bcdc_initialized || dict_id == 0)  // Can't evict default dict
        return ERR_INVALID_ARGUMENT;

    spin_lock(&g_bcdc_lock);
    
    if (dict_id >= BCDC_MAX_DICTS || !g_bcdc_dictionaries[dict_id].active) {
        spin_unlock(&g_bcdc_lock);
        return ERR_OBJECT_NOT_FOUND;
    }
    
    memset(&g_bcdc_dictionaries[dict_id], 0, sizeof(BcdcDictionary));
    
    spin_unlock(&g_bcdc_lock);
    
    debug_printf("[Bcdc] Evicted dictionary %u\n", dict_id);
    return OK;
}

void BcdcUpdateDictionaryUsage(unsigned int dict_id) {
    if (dict_id >= BCDC_MAX_DICTS)
        return;

    spin_lock(&g_bcdc_lock);
    g_bcdc_dictionaries[dict_id].usage_count++;
    g_bcdc_dictionaries[dict_id].last_used = rtc_get_unix64();
    spin_unlock(&g_bcdc_lock);
}

// ============================================================================
// Policy Management
// ============================================================================

error_t BcdcSetPolicy(const BcdcPolicy* policy) {
    if (!g_bcdc_initialized || !policy)
        return ERR_NOT_INITIALIZED;
    
    spin_lock(&g_bcdc_lock);
    
    // Find existing policy or add new
    for (uint8_t i = 0; i < g_bcdc_policy_count; i++) {
        if (g_bcdc_policies[i].tag_id == policy->tag_id) {
            g_bcdc_policies[i] = *policy;
            spin_unlock(&g_bcdc_lock);
            return OK;
        }
    }
    
    if (g_bcdc_policy_count >= 64) {
        spin_unlock(&g_bcdc_lock);
        return ERR_NO_MEMORY;
    }
    
    g_bcdc_policies[g_bcdc_policy_count++] = *policy;
    
    spin_unlock(&g_bcdc_lock);
    return OK;
}

error_t BcdcGetPolicy(uint16_t tag_id, BcdcPolicy* out) {
    if (!g_bcdc_initialized || !out)
        return ERR_NOT_INITIALIZED;
    
    spin_lock(&g_bcdc_lock);
    
    for (uint8_t i = 0; i < g_bcdc_policy_count; i++) {
        if (g_bcdc_policies[i].tag_id == tag_id) {
            *out = g_bcdc_policies[i];
            spin_unlock(&g_bcdc_lock);
            return OK;
        }
    }
    
    // Return default policy
    out->tag_id = tag_id;
    out->compression_type = BCDC_TYPE_LZ;
    out->compression_level = BCDC_LEVEL_DEFAULT;
    out->dictionary_sharing = 1;
    out->reserved = 0;
    
    spin_unlock(&g_bcdc_lock);
    return OK;
}

// ============================================================================
// Bcdc-LZ Compression (LZ77 Variant)
// ============================================================================

static uint16_t BcdcLZ_FindMatch(const uint8_t* input, uint16_t pos, uint16_t input_size,
                                  const uint8_t* dictionary, uint16_t* match_len) {
    if (pos >= input_size || pos == 0) {
        *match_len = 0;
        return 0;
    }

    // Limit search window to ensure decompressor can handle all matches
    uint16_t max_offset = BCDC_LZ_WINDOW_SIZE;
    if (max_offset > pos)
        max_offset = pos;

    uint16_t window_start = pos - max_offset;
    const uint8_t* current = &input[pos];
    uint16_t max_possible = input_size - pos;

    uint16_t best_offset = 0;
    uint16_t best_length = 0;

    // Search in input data first
    for (uint16_t p = window_start; p < pos; p++) {
        if (input[p] != current[0] || input[p + 1] != current[1])
            continue;

        uint16_t len = 0;
        while (len < BCDC_LZ_MAX_MATCH &&
               len < max_possible &&
               len < (pos - p) &&
               input[p + len] == current[len]) {
            len++;
        }

        if (len >= BCDC_LZ_MIN_MATCH && len > best_length) {
            best_length = len;
            best_offset = pos - p;
        }
    }

    // Search in dictionary if available
    if (dictionary && best_length < BCDC_LZ_MAX_MATCH) {
        for (uint16_t p = 0; p < BCDC_DICT_SIZE - 1; p++) {
            if (dictionary[p] != current[0] || dictionary[p + 1] != current[1])
                continue;

            uint16_t len = 0;
            uint16_t dict_remaining = BCDC_DICT_SIZE - p;
            while (len < BCDC_LZ_MAX_MATCH &&
                   len < max_possible &&
                   len < dict_remaining &&
                   dictionary[p + len] == current[len]) {
                len++;
            }

            // If we exhausted dictionary, continue matching in input
            if (len >= dict_remaining && dict_remaining < max_possible) {
                uint16_t input_match = 0;
                while (len + input_match < BCDC_LZ_MAX_MATCH &&
                       len + input_match < max_possible &&
                       input[input_match] == current[len + input_match]) {
                    input_match++;
                }
                len += input_match;
            }

            if (len >= BCDC_LZ_MIN_MATCH && len > best_length) {
                best_length = len;
                // Offset includes distance through input to dictionary
                best_offset = pos + (BCDC_DICT_SIZE - p);
            }
        }
    }

    *match_len = best_length;
    return best_offset;
}

error_t BcdcLZ_Compress(const void* input, uint16_t input_size,
                        void* output, uint16_t* output_size,
                        uint8_t level, const uint8_t* dictionary) {
    if (!input || !output || !output_size || input_size == 0)
        return ERR_INVALID_ARGUMENT;

    const uint8_t* in = (const uint8_t*)input;
    uint8_t* out = (uint8_t*)output;
    uint16_t out_pos = 0;
    uint16_t in_pos = 0;
    (void)level;

    while (in_pos < input_size && out_pos < BCDC_MAX_COMPRESSED) {
        if (in_pos + 2 > input_size) {
            if (out_pos + 1 >= BCDC_MAX_COMPRESSED)
                break;
            out[out_pos++] = in[in_pos++];
            if (in_pos < input_size)
                out[out_pos++] = in[in_pos++];
            break;
        }

        uint16_t match_len = 0;
        uint16_t match_offset = BcdcLZ_FindMatch(in, in_pos, input_size, dictionary, &match_len);

        if (match_len >= BCDC_LZ_MIN_MATCH && match_offset > 0 && match_offset <= BCDC_LZ_WINDOW_SIZE + BCDC_DICT_SIZE) {
            if (out_pos + 3 > BCDC_MAX_COMPRESSED)
                break;

            uint8_t len_byte = (uint8_t)((match_len - BCDC_LZ_MIN_MATCH) & 0x1F);
            uint8_t offset_lo = match_offset & 0xFF;
            uint8_t offset_hi = (match_offset >> 8) & 0x0F;

            out[out_pos++] = 0x80 | len_byte;
            out[out_pos++] = offset_lo;
            out[out_pos++] = (offset_hi << 4) | (len_byte >> 5);

            in_pos += match_len;
        } else {
            if (out_pos + 1 >= BCDC_MAX_COMPRESSED)
                break;
            out[out_pos++] = in[in_pos++];
        }
    }

    *output_size = out_pos;

    if (in_pos < input_size)
        return ERR_BUFFER_TOO_SMALL;

    return OK;
}

error_t BcdcLZ_Decompress(const void* input, uint16_t input_size,
                          void* output, uint16_t* output_size,
                          const uint8_t* dictionary) {
    if (!input || !output || !output_size || input_size == 0)
        return ERR_INVALID_ARGUMENT;

    const uint8_t* in = (const uint8_t*)input;
    uint8_t* out = (uint8_t*)output;
    uint16_t out_pos = 0;
    uint16_t in_pos = 0;

    while (in_pos < input_size && out_pos < *output_size) {
        uint8_t token = in[in_pos++];

        if (token & 0x80) {
            if (in_pos + 2 > input_size)
                return ERR_CORRUPTED;

            uint8_t len_low = token & 0x1F;
            uint8_t offset_lo = in[in_pos++];
            uint8_t offset_hi_byte = in[in_pos++];

            uint16_t match_len = (uint16_t)len_low + BCDC_LZ_MIN_MATCH;
            uint16_t match_offset = (uint16_t)offset_lo | ((uint16_t)(offset_hi_byte >> 4) << 8);

            if (match_offset == 0)
                return ERR_CORRUPTED;

            // Check if match is in dictionary or in output
            if (match_offset > out_pos) {
                // Match is in dictionary
                if (!dictionary)
                    return ERR_CORRUPTED;

                uint16_t dict_offset = BCDC_DICT_SIZE - (match_offset - out_pos);
                if (dict_offset >= BCDC_DICT_SIZE)
                    return ERR_CORRUPTED;

                for (uint16_t i = 0; i < match_len && out_pos < *output_size; i++) {
                    uint16_t src_pos = dict_offset + i;
                    if (src_pos < BCDC_DICT_SIZE) {
                        out[out_pos] = dictionary[src_pos];
                    } else {
                        // Continue from beginning of output
                        out[out_pos] = out[src_pos - BCDC_DICT_SIZE];
                    }
                    out_pos++;
                }
            } else {
                // Match is in output buffer
                if (match_offset > out_pos)
                    return ERR_CORRUPTED;

                for (uint16_t i = 0; i < match_len && out_pos < *output_size; i++) {
                    uint16_t src_pos = out_pos - match_offset;
                    out[out_pos] = out[src_pos];
                    out_pos++;
                }
            }
        } else {
            if (out_pos >= *output_size)
                return ERR_BUFFER_TOO_SMALL;
            out[out_pos++] = token;
        }
    }

    *output_size = out_pos;
    return OK;
}

// ============================================================================
// Bcdc-RLE Compression (Run-Length Encoding)
// ============================================================================

error_t BcdcRLE_Compress(const void* input, uint16_t input_size,
                         void* output, uint16_t* output_size) {
    if (!input || !output || !output_size || input_size == 0)
        return ERR_INVALID_ARGUMENT;
    
    const uint8_t* in = (const uint8_t*)input;
    uint8_t* out = (uint8_t*)output;
    uint16_t out_pos = 0;
    uint16_t in_pos = 0;
    
    while (in_pos < input_size && out_pos < BCDC_MAX_COMPRESSED) {
        uint8_t byte = in[in_pos];
        uint16_t run_length = 1;
        
        // Count run
        while (in_pos + run_length < input_size && 
               in[in_pos + run_length] == byte &&
               run_length < 255) {
            run_length++;
        }
        
        if (run_length >= BCDC_RLE_MIN_RUN) {
            // Output run: [ESCAPE][byte][length]
            if (out_pos + 3 > BCDC_MAX_COMPRESSED)
                break;
            
            out[out_pos++] = BCDC_RLE_ESCAPE;
            out[out_pos++] = byte;
            out[out_pos++] = (uint8_t)run_length;
            in_pos += run_length;
        } else {
            // Output literal(s)
            if (byte == BCDC_RLE_ESCAPE) {
                // Escape the escape byte
                if (out_pos + 2 > BCDC_MAX_COMPRESSED)
                    break;
                out[out_pos++] = BCDC_RLE_ESCAPE;
                out[out_pos++] = BCDC_RLE_ESCAPE;
            } else {
                if (out_pos + 1 > BCDC_MAX_COMPRESSED)
                    break;
                out[out_pos++] = byte;
            }
            in_pos++;
        }
    }
    
    *output_size = out_pos;
    
    if (in_pos < input_size)
        return ERR_BUFFER_TOO_SMALL;
    
    return OK;
}

error_t BcdcRLE_Decompress(const void* input, uint16_t input_size,
                           void* output, uint16_t* output_size) {
    if (!input || !output || !output_size || input_size == 0)
        return ERR_INVALID_ARGUMENT;
    
    const uint8_t* in = (const uint8_t*)input;
    uint8_t* out = (uint8_t*)output;
    uint16_t out_pos = 0;
    uint16_t in_pos = 0;
    
    while (in_pos < input_size && out_pos < *output_size) {
        uint8_t byte = in[in_pos++];
        
        if (byte == BCDC_RLE_ESCAPE) {
            if (in_pos >= input_size)
                return ERR_CORRUPTED;
            
            uint8_t run_byte = in[in_pos++];
            
            if (run_byte == BCDC_RLE_ESCAPE) {
                // Escaped escape byte
                if (out_pos >= *output_size)
                    return ERR_BUFFER_TOO_SMALL;
                out[out_pos++] = BCDC_RLE_ESCAPE;
            } else {
                // Run
                if (in_pos >= input_size)
                    return ERR_CORRUPTED;
                
                uint8_t run_length = in[in_pos++];
                for (uint8_t i = 0; i < run_length && out_pos < *output_size; i++) {
                    out[out_pos++] = run_byte;
                }
            }
        } else {
            // Literal
            if (out_pos >= *output_size)
                return ERR_BUFFER_TOO_SMALL;
            out[out_pos++] = byte;
        }
    }
    
    *output_size = out_pos;
    return OK;
}

// ============================================================================
// Main Compression/Decompression API
// ============================================================================

error_t BcdcCompress(const void* input, uint16_t input_size,
                     void* output, uint16_t* output_size,
                     uint8_t compression_type, uint8_t level,
                     unsigned int dictionary_id) {
    if (!g_bcdc_initialized)
        return ERR_NOT_INITIALIZED;

    if (!input || !output || !output_size || input_size == 0 || input_size > BCDC_BLOCK_SIZE)
        return ERR_INVALID_ARGUMENT;

    spin_lock(&g_bcdc_lock);

    BcdcBlockHeader* header = (BcdcBlockHeader*)output;
    uint8_t* compressed_data = output + BCDC_HEADER_SIZE;
    uint16_t compressed_size = 0;
    error_t result;

    // Get dictionary if enabled
    BcdcDictionary* dict = NULL;
    if (dictionary_id < BCDC_MAX_DICTS && g_bcdc_dictionaries[dictionary_id].active) {
        dict = &g_bcdc_dictionaries[dictionary_id];
        dict->usage_count++;
        dict->last_used = rtc_get_unix64();
        g_bcdc_stats.dict_hits++;
    } else {
        g_bcdc_stats.dict_misses++;
    }

    // Compute original checksum
    header->original_checksum = BcdcComputeChecksum(input, input_size);

    // Try compression
    if (compression_type == BCDC_TYPE_LZ) {
        result = BcdcLZ_Compress(input, input_size, compressed_data,
                                  &compressed_size, level,
                                  dict ? dict->dictionary : NULL);
        if (result == OK)
            g_bcdc_stats.lz_compressions++;
    } else if (compression_type == BCDC_TYPE_RLE) {
        result = BcdcRLE_Compress(input, input_size, compressed_data, &compressed_size);
        if (result == OK)
            g_bcdc_stats.rle_compressions++;
    } else {
        result = ERR_INVALID_ARGUMENT;
    }

    // If compression failed or didn't save space, store uncompressed
    if (result != OK || compressed_size >= input_size) {
        memcpy(compressed_data, input, input_size);
        compressed_size = input_size;
        compression_type = BCDC_TYPE_NONE;
        g_bcdc_stats.uncompressed_blocks++;
    } else {
        // Update dictionary with original data for future compressions
        BcdcUpdateDictionary(input, input_size);
    }

    // Fill header
    header->magic = BCDC_MAGIC;
    header->version = BCDC_VERSION;
    header->compression_type = compression_type;
    header->flags = 0;
    header->original_size = input_size;
    header->compressed_size = (compression_type == BCDC_TYPE_NONE) ? 0 : compressed_size;
    header->checksum = BcdcComputeChecksum(compressed_data,
                        (compression_type == BCDC_TYPE_NONE) ? input_size : compressed_size);
    header->dictionary_id = dictionary_id;

    *output_size = BCDC_HEADER_SIZE + ((compression_type == BCDC_TYPE_NONE) ? input_size : compressed_size);

    g_bcdc_stats.blocks_compressed++;
    g_bcdc_stats.bytes_before += input_size;
    g_bcdc_stats.bytes_after += *output_size;

    spin_unlock(&g_bcdc_lock);

    return OK;
}

error_t BcdcDecompress(const void* input, uint16_t input_size,
                       void* output, uint16_t* output_size,
                       unsigned int dictionary_id) {
    if (!g_bcdc_initialized)
        return ERR_NOT_INITIALIZED;
    
    if (!input || !output || !output_size || input_size < BCDC_HEADER_SIZE)
        return ERR_INVALID_ARGUMENT;
    
    spin_lock(&g_bcdc_lock);
    
    const BcdcBlockHeader* header = (const BcdcBlockHeader*)input;
    const uint8_t* compressed_data = input + BCDC_HEADER_SIZE;
    uint16_t compressed_size = 0;
    error_t result;
    
    // Verify header
    if (header->magic != BCDC_MAGIC || header->version != BCDC_VERSION) {
        spin_unlock(&g_bcdc_lock);
        return ERR_CORRUPTED;
    }
    
    // Verify checksum
    compressed_size = (header->compressed_size == 0) ? header->original_size : header->compressed_size;
    if (!BcdcVerifyChecksum(compressed_data, compressed_size, header->checksum)) {
        g_bcdc_stats.compression_failures++;
        spin_unlock(&g_bcdc_lock);
        return ERR_CORRUPTED;
    }
    
    // Get dictionary if enabled
    BcdcDictionary* dict = NULL;
    if (dictionary_id < BCDC_MAX_DICTS && g_bcdc_dictionaries[dictionary_id].active) {
        dict = &g_bcdc_dictionaries[dictionary_id];
    }
    
    // Decompress
    if (header->compression_type == BCDC_TYPE_NONE) {
        memcpy(output, compressed_data, header->original_size);
        *output_size = header->original_size;
        result = OK;
    } else if (header->compression_type == BCDC_TYPE_LZ) {
        // Set output size hint before decompress
        *output_size = header->original_size;
        result = BcdcLZ_Decompress(compressed_data, compressed_size, output,
                                    output_size, dict ? dict->dictionary : NULL);
    } else if (header->compression_type == BCDC_TYPE_RLE) {
        // Set output size hint before decompress
        *output_size = header->original_size;
        result = BcdcRLE_Decompress(compressed_data, compressed_size, output, output_size);
    } else {
        result = ERR_CORRUPTED;
    }

    if (result == OK) {
        g_bcdc_stats.blocks_decompressed++;
        // output_size already set correctly by decompress function
    }
    
    spin_unlock(&g_bcdc_lock);
    return result;
}

// ============================================================================
// Statistics
// ============================================================================

void BcdcGetStats(BcdcStats* stats) {
    if (!stats)
        return;
    
    spin_lock(&g_bcdc_lock);
    memcpy(stats, &g_bcdc_stats, sizeof(BcdcStats));
    spin_unlock(&g_bcdc_lock);
}

void BcdcResetStats(void) {
    spin_lock(&g_bcdc_lock);
    memset(&g_bcdc_stats, 0, sizeof(BcdcStats));
    spin_unlock(&g_bcdc_lock);
}
