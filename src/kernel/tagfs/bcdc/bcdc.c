#include "bcdc.h"
#include "../tagfs.h"
#include "../../../kernel/drivers/timer/rtc.h"
#include "../../../kernel/config/kernel_config.h"

/* ============================================================================
 *  BCDC — dynamic redesign (2026-04-29)
 *
 *  RAM model:
 *    • g_dicts[] is a slot-array of POINTERS (256 × 8 B = 2 KB), not 2 MB
 *      of pre-baked structs. Each dictionary is kmalloc'd on first use and
 *      freed on eviction. Unused slots cost zero.
 *    • Policies live in a growable kmalloc'd table (starts at
 *      CONFIG_BCDC_POLICY_INITIAL, doubles on demand up to
 *      CONFIG_BCDC_POLICY_MAX).
 *
 *  Concurrency model:
 *    • g_table_lock: protects ADD/REMOVE on g_dicts[] and the policy table
 *      resize. Compress/decompress callers do NOT hold it for the duration
 *      of compression.
 *    • per-dict spinlock: covers data-buffer mutation (slide, pattern
 *      preservation). The hot path (BcdcLZ_Compress / Decompress) reads the
 *      buffer without taking the lock — buffers are append-only mostly and
 *      only the pattern preservation in BcdcUpdateDictionary needs it.
 *    • stats: every counter is _Atomic, no lock for increment/read.
 *
 *  Speed model:
 *    • LZ77 match finder uses a 4096-bucket hash chain (head[] + chain[])
 *      keyed on a 3-byte fingerprint. Average O(n) per block instead of
 *      the previous O(n²) brute-force scan.
 * ============================================================================ */

/* ------------------------------------------------------------------ */
/* Global state                                                        */
/* ------------------------------------------------------------------ */

static BcdcDictionary *g_dicts[CONFIG_BCDC_MAX_DICTS];   /* lazy slots */
static spinlock_t      g_table_lock;

static BcdcPolicy *g_policies          = NULL;           /* kmalloc'd */
static uint32_t    g_policy_capacity   = 0;
static uint32_t    g_policy_count      = 0;

static BcdcStats   g_stats;
static bool        g_initialized       = false;

/* ------------------------------------------------------------------ */
/* Checksum (CRC32 — IEEE 802.3 polynomial)                            */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/* Init / shutdown                                                     */
/* ------------------------------------------------------------------ */

static error_t bcdc_grow_policies_locked(uint32_t want_cap) {
    if (want_cap <= g_policy_capacity) return OK;
    if (want_cap > CONFIG_BCDC_POLICY_MAX) return ERR_NO_MEMORY;

    uint32_t new_cap = g_policy_capacity ? g_policy_capacity : CONFIG_BCDC_POLICY_INITIAL;
    while (new_cap < want_cap) new_cap *= 2;
    if (new_cap > CONFIG_BCDC_POLICY_MAX) new_cap = CONFIG_BCDC_POLICY_MAX;

    BcdcPolicy *fresh = kmalloc(sizeof(BcdcPolicy) * new_cap);
    if (!fresh) return ERR_NO_MEMORY;
    if (g_policies && g_policy_count > 0)
        memcpy(fresh, g_policies, sizeof(BcdcPolicy) * g_policy_count);
    if (g_policies) kfree(g_policies);
    g_policies        = fresh;
    g_policy_capacity = new_cap;
    return OK;
}

static error_t bcdc_alloc_dict_locked(uint16_t slot, uint16_t tag_id, uint16_t buf_size) {
    BcdcDictionary *d = kmalloc(sizeof(BcdcDictionary));
    if (!d) return ERR_NO_MEMORY;

    uint8_t *buf = kmalloc(buf_size);
    if (!buf) { kfree(d); return ERR_NO_MEMORY; }
    memset(buf, 0, buf_size);

    memset(d, 0, sizeof(*d));
    d->dictionary_id = slot;
    d->tag_id        = tag_id;
    d->active        = true;
    d->data          = buf;
    d->data_size     = buf_size;
    spinlock_init(&d->lock);
    __atomic_store_n(&d->last_used,   rtc_get_unix64(), __ATOMIC_RELAXED);
    __atomic_store_n(&d->usage_count, 0,                __ATOMIC_RELAXED);
    g_dicts[slot] = d;
    return OK;
}

static void bcdc_free_dict_locked(uint16_t slot) {
    BcdcDictionary *d = g_dicts[slot];
    if (!d) return;
    g_dicts[slot] = NULL;
    if (d->data) kfree(d->data);
    kfree(d);
}

error_t BcdcInit(void) {
    if (g_initialized) return ERR_ALREADY_INITIALIZED;

    spinlock_init(&g_table_lock);
    memset(g_dicts, 0, sizeof(g_dicts));
    memset(&g_stats, 0, sizeof(g_stats));

    /* Default dictionary (slot 0) — eagerly allocated so the legacy
     * dictionary_id=0 sentinel always resolves to a valid buffer. */
    spin_lock(&g_table_lock);
    error_t err = bcdc_alloc_dict_locked(0, 0, CONFIG_BCDC_DEFAULT_DICT_SIZE);
    if (err != OK) {
        spin_unlock(&g_table_lock);
        return err;
    }
    err = bcdc_grow_policies_locked(CONFIG_BCDC_POLICY_INITIAL);
    spin_unlock(&g_table_lock);
    if (err != OK) return err;

    g_initialized = true;
    debug_printf("[Bcdc] Initialized: 1 dict, policy cap=%u\n", g_policy_capacity);
    return OK;
}

void BcdcShutdown(void) {
    if (!g_initialized) return;

    spin_lock(&g_table_lock);
    g_initialized = false;
    for (uint16_t i = 0; i < CONFIG_BCDC_MAX_DICTS; i++) {
        bcdc_free_dict_locked(i);
    }
    if (g_policies) {
        kfree(g_policies);
        g_policies        = NULL;
        g_policy_capacity = 0;
        g_policy_count    = 0;
    }
    spin_unlock(&g_table_lock);

    debug_printf("[Bcdc] Shutdown complete\n");
}

/* ------------------------------------------------------------------ */
/* Dictionary management                                                */
/* ------------------------------------------------------------------ */

error_t BcdcCreateDictionary(uint8_t* dict_id, uint16_t tag_id) {
    if (!g_initialized || !dict_id) return ERR_NOT_INITIALIZED;

    spin_lock(&g_table_lock);

    /* First, look for an empty slot. Use a sentinel so we can tell apart
     * "found a slot" from "all slots taken". The previous version
     * initialised slot=0 and then tested `slot >= MAX`, which never
     * triggered — meaning a full table silently overwrote slot 0 (the
     * default dictionary). That bug is fixed here. */
    uint16_t slot = CONFIG_BCDC_MAX_DICTS;
    for (uint16_t i = 1; i < CONFIG_BCDC_MAX_DICTS; i++) {
        if (g_dicts[i] == NULL) { slot = i; break; }
    }

    if (slot == CONFIG_BCDC_MAX_DICTS) {
        /* Pick LRU among slots >0 (slot 0 is the default — never evict). */
        uint64_t oldest = UINT64_MAX;
        uint16_t victim = CONFIG_BCDC_MAX_DICTS;
        for (uint16_t i = 1; i < CONFIG_BCDC_MAX_DICTS; i++) {
            BcdcDictionary *d = g_dicts[i];
            if (!d) continue;
            uint64_t lu = __atomic_load_n(&d->last_used, __ATOMIC_RELAXED);
            if (lu < oldest) { oldest = lu; victim = i; }
        }
        if (victim == CONFIG_BCDC_MAX_DICTS) {
            spin_unlock(&g_table_lock);
            return ERR_NO_MEMORY;        /* table empty but somehow none active */
        }
        bcdc_free_dict_locked(victim);
        slot = victim;
    }

    error_t err = bcdc_alloc_dict_locked(slot, tag_id, CONFIG_BCDC_DEFAULT_DICT_SIZE);
    spin_unlock(&g_table_lock);

    if (err != OK) return err;

    *dict_id = (uint8_t)slot;
    debug_printf("[Bcdc] Created dictionary %u for tag %u\n", slot, tag_id);
    return OK;
}

error_t BcdcGetDictionary(unsigned int dict_id, BcdcDictionary** out) {
    if (!g_initialized || !out || dict_id >= CONFIG_BCDC_MAX_DICTS)
        return ERR_INVALID_ARGUMENT;

    spin_lock(&g_table_lock);
    BcdcDictionary *d = g_dicts[dict_id];
    if (!d || !d->active) {
        spin_unlock(&g_table_lock);
        return ERR_OBJECT_NOT_FOUND;
    }
    *out = d;
    spin_unlock(&g_table_lock);
    return OK;
}

error_t BcdcEvictDictionary(unsigned int dict_id) {
    if (!g_initialized || dict_id == 0 || dict_id >= CONFIG_BCDC_MAX_DICTS)
        return ERR_INVALID_ARGUMENT;

    spin_lock(&g_table_lock);
    if (!g_dicts[dict_id]) {
        spin_unlock(&g_table_lock);
        return ERR_OBJECT_NOT_FOUND;
    }
    bcdc_free_dict_locked(dict_id);
    spin_unlock(&g_table_lock);

    debug_printf("[Bcdc] Evicted dictionary %u\n", dict_id);
    return OK;
}

void BcdcUpdateDictionaryUsage(unsigned int dict_id) {
    if (dict_id >= CONFIG_BCDC_MAX_DICTS) return;
    BcdcDictionary *d = g_dicts[dict_id];
    if (!d) return;
    __atomic_fetch_add(&d->usage_count, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&d->last_used, rtc_get_unix64(), __ATOMIC_RELAXED);
}

/* Slide-window dictionary update. Holds only the per-dict lock. */
static void BcdcUpdateDictionary(BcdcDictionary *dict, const uint8_t* data, uint16_t size) {
    if (!dict || !dict->data || !data || size < 4) return;
    /* The earlier "if (size >= BCDC_DICT_SIZE)" branch was dead code:
     * input_size for compression is bounded by BCDC_BLOCK_SIZE (4096) and
     * dict->data_size defaults to 8192 — size can never exceed it. */
    if (size > dict->data_size) size = dict->data_size;

    spin_lock(&dict->lock);
    uint16_t keep = (uint16_t)(dict->data_size - size);
    if (keep > 0) memmove(dict->data, dict->data + size, keep);
    memcpy(dict->data + keep, data, size);
    spin_unlock(&dict->lock);
}

/* ------------------------------------------------------------------ */
/* Policy management                                                    */
/* ------------------------------------------------------------------ */

error_t BcdcSetPolicy(const BcdcPolicy* policy) {
    if (!g_initialized || !policy) return ERR_NOT_INITIALIZED;

    spin_lock(&g_table_lock);

    for (uint32_t i = 0; i < g_policy_count; i++) {
        if (g_policies[i].tag_id == policy->tag_id) {
            g_policies[i] = *policy;
            spin_unlock(&g_table_lock);
            return OK;
        }
    }

    if (g_policy_count >= g_policy_capacity) {
        error_t err = bcdc_grow_policies_locked(g_policy_capacity * 2);
        if (err != OK) {
            spin_unlock(&g_table_lock);
            return err;
        }
    }

    g_policies[g_policy_count++] = *policy;
    spin_unlock(&g_table_lock);
    return OK;
}

error_t BcdcGetPolicy(uint16_t tag_id, BcdcPolicy* out) {
    if (!g_initialized || !out) return ERR_NOT_INITIALIZED;

    spin_lock(&g_table_lock);
    for (uint32_t i = 0; i < g_policy_count; i++) {
        if (g_policies[i].tag_id == tag_id) {
            *out = g_policies[i];
            spin_unlock(&g_table_lock);
            return OK;
        }
    }
    spin_unlock(&g_table_lock);

    /* Fallback default. */
    out->tag_id             = tag_id;
    out->compression_type   = BCDC_TYPE_LZ;
    out->compression_level  = BCDC_LEVEL_DEFAULT;
    out->dictionary_sharing = 1;
    out->reserved           = 0;
    return OK;
}

/* ------------------------------------------------------------------ */
/* Bcdc-LZ — hash-chain backed LZ77                                    */
/* ------------------------------------------------------------------ */

#define BCDC_LZ_HASH_SIZE  (1u << CONFIG_BCDC_LZ_HASH_BITS)
#define BCDC_LZ_HASH_MASK  (BCDC_LZ_HASH_SIZE - 1u)

static inline uint32_t bcdc_lz_hash(const uint8_t *p) {
    /* 3-byte FNV-ish fingerprint, reduced to HASH_BITS. */
    uint32_t h = (uint32_t)p[0];
    h = (h * 2654435761u) ^ (uint32_t)p[1];
    h = (h * 2654435761u) ^ (uint32_t)p[2];
    return h & BCDC_LZ_HASH_MASK;
}

/*
 * Compress with hash-chain accelerated match finding. The chain head[h]
 * gives the most recent input position whose 3-byte prefix hashes to h;
 * chain[pos] gives the next-older position with the same hash. We follow
 * the chain and keep the longest match.
 *
 * Stack budget: BCDC_LZ_HASH_SIZE × 2 + BCDC_BLOCK_SIZE ≈ 16 KB at 4096
 * buckets. The kernel stack is 16 KB by default (4 pages), so we
 * heap-allocate the working buffers instead of trying to fit them on the
 * stack.
 */
typedef struct {
    int32_t *head;       /* [BCDC_LZ_HASH_SIZE] — newest pos per hash, -1 if empty */
    int32_t *chain;      /* [BCDC_BLOCK_SIZE]   — predecessor pos per pos */
} BcdcLZWork;

/* Caller guarantees `pos + 3 <= input_size`. The hash reads three bytes
 * starting at src[pos]; calling without that guarantee corrupts the hash
 * by mixing in past-end memory. */
static void bcdc_lz_chain_insert(BcdcLZWork *w, const uint8_t *src, uint16_t pos) {
    uint32_t h = bcdc_lz_hash(src + pos);
    w->chain[pos] = w->head[h];
    w->head[h]    = pos;
}

error_t BcdcLZ_Compress(const void* input, uint16_t input_size,
                        void* output, uint16_t* output_size,
                        uint8_t level, const uint8_t* dictionary,
                        uint16_t dictionary_size) {
    if (!input || !output || !output_size || input_size == 0)
        return ERR_INVALID_ARGUMENT;
    (void)level;

    const uint8_t* in  = (const uint8_t*)input;
    uint8_t*       out = (uint8_t*)output;
    uint16_t       in_pos  = 0;
    uint16_t       out_pos = 0;

    /* Working buffers — kmalloc'd to avoid 16 KB on the kernel stack. */
    BcdcLZWork w;
    w.head  = kmalloc(sizeof(int32_t) * BCDC_LZ_HASH_SIZE);
    w.chain = kmalloc(sizeof(int32_t) * BCDC_BLOCK_SIZE);
    if (!w.head || !w.chain) {
        if (w.head)  kfree(w.head);
        if (w.chain) kfree(w.chain);
        return ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < BCDC_LZ_HASH_SIZE; i++) w.head[i] = -1;

    while (in_pos < input_size && out_pos < BCDC_MAX_COMPRESSED) {
        if ((int)in_pos + 3 > (int)input_size) {
            /* Tail bytes — emit as literals. */
            while (in_pos < input_size && out_pos < BCDC_MAX_COMPRESSED)
                out[out_pos++] = in[in_pos++];
            break;
        }

        uint16_t best_offset = 0;
        uint16_t best_length = 0;
        uint32_t h = bcdc_lz_hash(in + in_pos);
        int32_t  cur = w.head[h];

        /* Bound the chain walk so worst-case stays O(n). */
        int chain_budget = 64;
        while (cur >= 0 && cur < (int)in_pos && chain_budget-- > 0) {
            uint16_t offset = (uint16_t)((int)in_pos - cur);
            if (offset > BCDC_LZ_WINDOW_SIZE) break;

            const uint8_t *cand = in + cur;
            uint16_t max_len = (uint16_t)(input_size - in_pos);
            if (max_len > BCDC_LZ_MAX_MATCH) max_len = BCDC_LZ_MAX_MATCH;
            if (max_len > offset)            max_len = offset;

            uint16_t len = 0;
            while (len < max_len && cand[len] == in[in_pos + len]) len++;

            if (len >= BCDC_LZ_MIN_MATCH && len > best_length) {
                best_length = len;
                best_offset = offset;
                if (len == BCDC_LZ_MAX_MATCH) break;
            }
            cur = w.chain[cur];
        }

        /* Dictionary matches: encoded as `in_pos + (dict_size - p)`. Since
         * the wire format only carries 13 bits of offset (max 8191), and
         * dict_size can be up to 8192, we must cap each candidate. */
        if (dictionary && dictionary_size > 0 && best_length < BCDC_LZ_MAX_MATCH) {
            const uint8_t *cur_data = in + in_pos;
            uint16_t max_len = (uint16_t)(input_size - in_pos);
            if (max_len > BCDC_LZ_MAX_MATCH) max_len = BCDC_LZ_MAX_MATCH;

            for (uint16_t p = 0; p + 1 < dictionary_size; p++) {
                if (dictionary[p] != cur_data[0] || dictionary[p+1] != cur_data[1])
                    continue;
                uint32_t encoded_offset = (uint32_t)in_pos + (uint32_t)(dictionary_size - p);
                if (encoded_offset == 0 || encoded_offset > 0x1FFF) continue;

                uint16_t cap = (uint16_t)(dictionary_size - p);
                if (cap > max_len) cap = max_len;
                uint16_t len = 0;
                while (len < cap && dictionary[p + len] == cur_data[len]) len++;
                if (len >= BCDC_LZ_MIN_MATCH && len > best_length) {
                    best_length = len;
                    best_offset = (uint16_t)encoded_offset;
                    if (len == BCDC_LZ_MAX_MATCH) break;
                }
            }
        }

        /* Encoded offset is 13 bits → 0..8191. Anything past that wraps in
         * the wire format, which is exactly the corruption that broke the
         * BCDC tests on real data. Reject the match instead. */
        if (best_length >= BCDC_LZ_MIN_MATCH && best_offset > 0 &&
            best_offset <= 0x1FFF) {
            if (out_pos + 3 > BCDC_MAX_COMPRESSED) break;

            /* 3-byte match token (Bcdc-LZ v2):
             *   byte 0: 1 LLLLLLL   — flag bit + lower 7 of (len-3)
             *   byte 1: OOOOOOOO   — lower 8 of offset
             *   byte 2: L OOOOOOO  — high bit of length + upper 5 of offset
             *
             * Length: 8 bits → 3..258. Offset: 13 bits → 0..8191. */
            uint16_t enc_len = (uint16_t)(best_length - BCDC_LZ_MIN_MATCH);
            if (enc_len > 255) enc_len = 255;

            out[out_pos++] = (uint8_t)(0x80 | (enc_len & 0x7F));
            out[out_pos++] = (uint8_t)(best_offset & 0xFF);
            out[out_pos++] = (uint8_t)(((enc_len >> 7) & 0x01) << 7) |
                             (uint8_t)((best_offset >> 8) & 0x1F);

            uint16_t consumed = (uint16_t)(BCDC_LZ_MIN_MATCH + enc_len);
            for (uint16_t k = 0; k < consumed && in_pos < input_size; k++) {
                if ((uint32_t)in_pos + 3 <= (uint32_t)input_size)
                    bcdc_lz_chain_insert(&w, in, in_pos);
                in_pos++;
            }
        } else {
            if (out_pos + 1 >= BCDC_MAX_COMPRESSED) break;
            out[out_pos++] = in[in_pos];
            /* in the main loop branch we already verified pos+3 <= size */
            bcdc_lz_chain_insert(&w, in, in_pos);
            in_pos++;
        }
    }

    *output_size = out_pos;

    kfree(w.head);
    kfree(w.chain);

    if (in_pos < input_size) return ERR_BUFFER_TOO_SMALL;
    return OK;
}

error_t BcdcLZ_Decompress(const void* input, uint16_t input_size,
                          void* output, uint16_t* output_size,
                          const uint8_t* dictionary,
                          uint16_t dictionary_size) {
    if (!input || !output || !output_size || input_size == 0)
        return ERR_INVALID_ARGUMENT;

    const uint8_t* in  = (const uint8_t*)input;
    uint8_t*       out = (uint8_t*)output;
    uint16_t       in_pos = 0;
    uint16_t       out_pos = 0;

    while (in_pos < input_size && out_pos < *output_size) {
        uint8_t token = in[in_pos++];

        if (token & 0x80) {
            if (in_pos + 2 > input_size) return ERR_CORRUPTED;

            uint8_t offset_lo = in[in_pos++];
            uint8_t byte2     = in[in_pos++];

            uint16_t match_len = (uint16_t)(token & 0x7F) |
                                 ((uint16_t)(byte2 >> 7) << 7);
            match_len += BCDC_LZ_MIN_MATCH;

            uint16_t match_offset = (uint16_t)offset_lo |
                                    ((uint16_t)(byte2 & 0x1F) << 8);

            if (match_offset == 0) return ERR_CORRUPTED;

            if (match_offset > out_pos) {
                /* Match crosses into the dictionary. */
                if (!dictionary || dictionary_size == 0) return ERR_CORRUPTED;
                uint16_t back = (uint16_t)(match_offset - out_pos);
                if (back > dictionary_size) return ERR_CORRUPTED;

                uint16_t dict_offset = (uint16_t)(dictionary_size - back);
                for (uint16_t i = 0; i < match_len && out_pos < *output_size; i++) {
                    uint16_t src = (uint16_t)(dict_offset + i);
                    if (src < dictionary_size) {
                        out[out_pos++] = dictionary[src];
                    } else {
                        uint16_t out_src = (uint16_t)(src - dictionary_size);
                        if (out_src >= out_pos) return ERR_CORRUPTED;
                        out[out_pos++] = out[out_src];
                    }
                }
            } else {
                for (uint16_t i = 0; i < match_len && out_pos < *output_size; i++) {
                    uint16_t src = (uint16_t)(out_pos - match_offset);
                    out[out_pos++] = out[src];
                }
            }
        } else {
            if (out_pos >= *output_size) return ERR_BUFFER_TOO_SMALL;
            out[out_pos++] = token;
        }
    }

    *output_size = out_pos;
    return OK;
}

/* ------------------------------------------------------------------ */
/* Bcdc-RLE                                                              */
/* ------------------------------------------------------------------ */

error_t BcdcRLE_Compress(const void* input, uint16_t input_size,
                         void* output, uint16_t* output_size) {
    if (!input || !output || !output_size || input_size == 0)
        return ERR_INVALID_ARGUMENT;

    const uint8_t* in  = (const uint8_t*)input;
    uint8_t*       out = (uint8_t*)output;
    uint16_t       in_pos = 0;
    uint16_t       out_pos = 0;

    while (in_pos < input_size && out_pos < BCDC_MAX_COMPRESSED) {
        uint8_t  byte       = in[in_pos];
        uint16_t run_length = 1;

        while (in_pos + run_length < input_size &&
               in[in_pos + run_length] == byte &&
               run_length < 255) {
            run_length++;
        }

        if (run_length >= BCDC_RLE_MIN_RUN) {
            if (out_pos + 3 > BCDC_MAX_COMPRESSED) break;
            out[out_pos++] = BCDC_RLE_ESCAPE;
            out[out_pos++] = byte;
            out[out_pos++] = (uint8_t)run_length;
            in_pos += run_length;
        } else {
            if (byte == BCDC_RLE_ESCAPE) {
                if (out_pos + 2 > BCDC_MAX_COMPRESSED) break;
                out[out_pos++] = BCDC_RLE_ESCAPE;
                out[out_pos++] = BCDC_RLE_ESCAPE;
            } else {
                if (out_pos + 1 > BCDC_MAX_COMPRESSED) break;
                out[out_pos++] = byte;
            }
            in_pos++;
        }
    }

    *output_size = out_pos;
    if (in_pos < input_size) return ERR_BUFFER_TOO_SMALL;
    return OK;
}

error_t BcdcRLE_Decompress(const void* input, uint16_t input_size,
                           void* output, uint16_t* output_size) {
    if (!input || !output || !output_size || input_size == 0)
        return ERR_INVALID_ARGUMENT;

    const uint8_t* in  = (const uint8_t*)input;
    uint8_t*       out = (uint8_t*)output;
    uint16_t       in_pos = 0;
    uint16_t       out_pos = 0;

    while (in_pos < input_size && out_pos < *output_size) {
        uint8_t byte = in[in_pos++];

        if (byte == BCDC_RLE_ESCAPE) {
            if (in_pos >= input_size) return ERR_CORRUPTED;
            uint8_t run_byte = in[in_pos++];

            if (run_byte == BCDC_RLE_ESCAPE) {
                if (out_pos >= *output_size) return ERR_BUFFER_TOO_SMALL;
                out[out_pos++] = BCDC_RLE_ESCAPE;
            } else {
                if (in_pos >= input_size) return ERR_CORRUPTED;
                uint8_t run_length = in[in_pos++];
                for (uint8_t i = 0; i < run_length && out_pos < *output_size; i++)
                    out[out_pos++] = run_byte;
            }
        } else {
            if (out_pos >= *output_size) return ERR_BUFFER_TOO_SMALL;
            out[out_pos++] = byte;
        }
    }

    *output_size = out_pos;
    return OK;
}

/* ------------------------------------------------------------------ */
/* Public compress / decompress                                         */
/* ------------------------------------------------------------------ */

error_t BcdcCompress(const void* input, uint16_t input_size,
                     void* output, uint16_t* output_size,
                     uint8_t compression_type, uint8_t level,
                     unsigned int dictionary_id) {
    if (!g_initialized) return ERR_NOT_INITIALIZED;
    if (!input || !output || !output_size ||
        input_size == 0 || input_size > BCDC_BLOCK_SIZE)
        return ERR_INVALID_ARGUMENT;

    BcdcBlockHeader *header = (BcdcBlockHeader*)output;
    uint8_t         *cdata  = (uint8_t*)output + BCDC_HEADER_SIZE;
    uint16_t         csize  = 0;

    /* Snapshot the dictionary pointer under the table lock; the dict
     * itself is reference-stable until eviction (which also takes the
     * table lock). */
    BcdcDictionary *dict = NULL;
    if (dictionary_id < CONFIG_BCDC_MAX_DICTS) {
        spin_lock(&g_table_lock);
        BcdcDictionary *cand = g_dicts[dictionary_id];
        if (cand && cand->active) {
            dict = cand;
            __atomic_fetch_add(&dict->usage_count, 1, __ATOMIC_RELAXED);
            __atomic_store_n(&dict->last_used, rtc_get_unix64(), __ATOMIC_RELAXED);
            __atomic_fetch_add(&g_stats.dict_hits, 1, __ATOMIC_RELAXED);
        } else {
            __atomic_fetch_add(&g_stats.dict_misses, 1, __ATOMIC_RELAXED);
        }
        spin_unlock(&g_table_lock);
    }

    header->original_checksum = BcdcComputeChecksum(input, input_size);

    error_t result;
    if (compression_type == BCDC_TYPE_LZ) {
        result = BcdcLZ_Compress(input, input_size, cdata, &csize, level,
                                 dict ? dict->data : NULL,
                                 dict ? dict->data_size : 0);
        if (result == OK)
            __atomic_fetch_add(&g_stats.lz_compressions, 1, __ATOMIC_RELAXED);
    } else if (compression_type == BCDC_TYPE_RLE) {
        result = BcdcRLE_Compress(input, input_size, cdata, &csize);
        if (result == OK)
            __atomic_fetch_add(&g_stats.rle_compressions, 1, __ATOMIC_RELAXED);
    } else {
        result = ERR_INVALID_ARGUMENT;
    }

    if (result != OK || csize >= input_size) {
        memcpy(cdata, input, input_size);
        csize = input_size;
        compression_type = BCDC_TYPE_NONE;
        __atomic_fetch_add(&g_stats.uncompressed_blocks, 1, __ATOMIC_RELAXED);
    } else if (dict) {
        BcdcUpdateDictionary(dict, (const uint8_t*)input, input_size);
    }

    header->magic             = BCDC_MAGIC;
    header->version           = BCDC_VERSION;
    header->compression_type  = compression_type;
    header->flags             = 0;
    header->original_size     = input_size;
    header->compressed_size   = (compression_type == BCDC_TYPE_NONE) ? 0 : csize;
    header->checksum          = BcdcComputeChecksum(cdata,
                                  (compression_type == BCDC_TYPE_NONE) ? input_size : csize);
    header->dictionary_id     = (uint8_t)dictionary_id;

    *output_size = (uint16_t)(BCDC_HEADER_SIZE +
                              (compression_type == BCDC_TYPE_NONE ? input_size : csize));

    __atomic_fetch_add(&g_stats.blocks_compressed, 1,           __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_stats.bytes_before,      input_size,  __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_stats.bytes_after,       *output_size,__ATOMIC_RELAXED);

    return OK;
}

error_t BcdcDecompress(const void* input, uint16_t input_size,
                       void* output, uint16_t* output_size,
                       unsigned int dictionary_id) {
    if (!g_initialized) return ERR_NOT_INITIALIZED;
    if (!input || !output || !output_size || input_size < BCDC_HEADER_SIZE)
        return ERR_INVALID_ARGUMENT;

    const BcdcBlockHeader *header = (const BcdcBlockHeader*)input;
    const uint8_t         *cdata  = (const uint8_t*)input + BCDC_HEADER_SIZE;

    if (header->magic != BCDC_MAGIC || header->version != BCDC_VERSION)
        return ERR_CORRUPTED;

    uint16_t csize = (header->compressed_size == 0)
                        ? header->original_size
                        : header->compressed_size;
    if (!BcdcVerifyChecksum(cdata, csize, header->checksum)) {
        __atomic_fetch_add(&g_stats.compression_failures, 1, __ATOMIC_RELAXED);
        return ERR_CORRUPTED;
    }

    BcdcDictionary *dict = NULL;
    if (dictionary_id < CONFIG_BCDC_MAX_DICTS) {
        spin_lock(&g_table_lock);
        BcdcDictionary *cand = g_dicts[dictionary_id];
        if (cand && cand->active) dict = cand;
        spin_unlock(&g_table_lock);
    }

    error_t result;
    if (header->compression_type == BCDC_TYPE_NONE) {
        /* original_size is bounded to BCDC_BLOCK_SIZE by the encoder; the
         * caller's buffer is its responsibility. Matching legacy behaviour. */
        memcpy(output, cdata, header->original_size);
        *output_size = header->original_size;
        result = OK;
    } else if (header->compression_type == BCDC_TYPE_LZ) {
        *output_size = header->original_size;
        result = BcdcLZ_Decompress(cdata, csize, output, output_size,
                                   dict ? dict->data : NULL,
                                   dict ? dict->data_size : 0);
    } else if (header->compression_type == BCDC_TYPE_RLE) {
        *output_size = header->original_size;
        result = BcdcRLE_Decompress(cdata, csize, output, output_size);
    } else {
        result = ERR_CORRUPTED;
    }

    if (result == OK)
        __atomic_fetch_add(&g_stats.blocks_decompressed, 1, __ATOMIC_RELAXED);
    return result;
}

/* ------------------------------------------------------------------ */
/* Stats                                                                */
/* ------------------------------------------------------------------ */

void BcdcGetStats(BcdcStats* out) {
    if (!out) return;
    /* Atomic snapshot — relaxed load is fine; counters are independent. */
    out->blocks_compressed    = __atomic_load_n(&g_stats.blocks_compressed,   __ATOMIC_RELAXED);
    out->blocks_decompressed  = __atomic_load_n(&g_stats.blocks_decompressed, __ATOMIC_RELAXED);
    out->bytes_before         = __atomic_load_n(&g_stats.bytes_before,        __ATOMIC_RELAXED);
    out->bytes_after          = __atomic_load_n(&g_stats.bytes_after,         __ATOMIC_RELAXED);
    out->lz_compressions      = __atomic_load_n(&g_stats.lz_compressions,     __ATOMIC_RELAXED);
    out->rle_compressions     = __atomic_load_n(&g_stats.rle_compressions,    __ATOMIC_RELAXED);
    out->uncompressed_blocks  = __atomic_load_n(&g_stats.uncompressed_blocks, __ATOMIC_RELAXED);
    out->compression_failures = __atomic_load_n(&g_stats.compression_failures,__ATOMIC_RELAXED);
    out->dict_hits            = __atomic_load_n(&g_stats.dict_hits,           __ATOMIC_RELAXED);
    out->dict_misses          = __atomic_load_n(&g_stats.dict_misses,         __ATOMIC_RELAXED);
}

void BcdcResetStats(void) {
    memset(&g_stats, 0, sizeof(g_stats));
}
