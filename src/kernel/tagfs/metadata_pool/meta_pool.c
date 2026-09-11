#include "meta_pool.h"
#include "../tagfs.h"
#include "tagfs.h"
#include "../../lib/kernel/crypto.h"

extern int  tagfs_alloc_blocks_internal(uint32_t count, uint32_t *out_start_block);
extern void tagfs_free_blocks_internal(uint32_t start_block, uint32_t count);

#define RECORD_HEADER_SIZE  42
#define MPOOL_BLOCK_HEADER  16
#define RECORD_CRC_OFFSET   40

static uint16_t meta_crc16(const uint8_t* data, uint32_t len) {
    return KCrc16(data, len);
}

static MetaPoolBlock g_current_block;
static uint32_t     g_current_block_num = 0;
static bool         g_current_dirty    = false;

static MetaPoolBlock g_fresh_block;

typedef struct {
    uint32_t block;
    uint16_t kept;
} MetaPoolLink;

static MetaPoolLink* g_chain     = NULL;
static uint32_t      g_chain_len = 0;
static uint32_t      g_chain_cap = 0;

static void chain_release(void) {
    if (g_chain) kfree(g_chain);
    g_chain     = NULL;
    g_chain_len = 0;
    g_chain_cap = 0;
}

static int chain_reserve(uint32_t want) {
    if (want <= g_chain_cap) return 0;

    uint32_t cap = g_chain_cap ? g_chain_cap * 2 : 8;
    if (cap < want) cap = want;

    MetaPoolLink* grown = kmalloc(sizeof(MetaPoolLink) * cap);
    if (!grown) return -1;

    if (g_chain && g_chain_len)
        memcpy(grown, g_chain, sizeof(MetaPoolLink) * g_chain_len);
    if (g_chain) kfree(g_chain);

    g_chain     = grown;
    g_chain_cap = cap;
    return 0;
}

static int chain_append(uint32_t block, uint16_t kept) {
    if (chain_reserve(g_chain_len + 1) != 0) return -1;
    g_chain[g_chain_len].block = block;
    g_chain[g_chain_len].kept  = kept;
    g_chain_len++;
    return 0;
}

static uint32_t chain_find(uint32_t block) {
    for (uint32_t i = 0; i < g_chain_len; i++) {
        if (g_chain[i].block == block) return i;
    }
    return g_chain_len;
}

static void chain_drop(uint32_t index) {
    if (index >= g_chain_len) return;
    for (uint32_t i = index; i + 1 < g_chain_len; i++) {
        g_chain[i] = g_chain[i + 1];
    }
    g_chain_len--;
}

static uint16_t count_kept_records(const MetaPoolBlock* blk) {
    uint32_t used = blk->used_bytes;
    if (used > TAGFS_MPOOL_DATA_SIZE) used = TAGFS_MPOOL_DATA_SIZE;

    uint16_t kept = 0;
    uint32_t pos  = 0;
    while (pos + RECORD_HEADER_SIZE <= used) {
        uint16_t record_len;
        memcpy(&record_len, blk->payload + pos, sizeof(uint16_t));
        if (record_len < RECORD_HEADER_SIZE || record_len > used - pos) break;

        uint32_t file_id;
        memcpy(&file_id, blk->payload + pos + 2, sizeof(uint32_t));
        if (file_id != 0) kept++;

        pos += record_len;
    }
    return kept;
}

typedef struct {
    uint32_t block;
    uint32_t offset;
} MetaPoolRetire;

static MetaPoolRetire* g_retire     = NULL;
static uint32_t        g_retire_len = 0;
static uint32_t        g_retire_cap = 0;

static void retire_release(void) {
    if (g_retire) kfree(g_retire);
    g_retire     = NULL;
    g_retire_len = 0;
    g_retire_cap = 0;
}

static int retire_owe(uint32_t block, uint32_t offset) {
    if (g_retire_len == g_retire_cap) {
        uint32_t cap = g_retire_cap ? g_retire_cap * 2 : 8;
        MetaPoolRetire* grown = kmalloc(sizeof(MetaPoolRetire) * cap);
        if (!grown) return -1;
        if (g_retire && g_retire_len)
            memcpy(grown, g_retire, sizeof(MetaPoolRetire) * g_retire_len);
        if (g_retire) kfree(g_retire);
        g_retire     = grown;
        g_retire_cap = cap;
    }
    g_retire[g_retire_len].block  = block;
    g_retire[g_retire_len].offset = offset;
    g_retire_len++;
    return 0;
}

static bool retire_owed(uint32_t block, uint32_t offset) {
    for (uint32_t i = 0; i < g_retire_len; i++) {
        if (g_retire[i].block == block && g_retire[i].offset == offset) return true;
    }
    return false;
}

static void retire_paid(uint32_t block) {
    uint32_t w = 0;
    for (uint32_t r = 0; r < g_retire_len; r++) {
        if (g_retire[r].block != block) g_retire[w++] = g_retire[r];
    }
    g_retire_len = w;
}

static uint16_t retire_apply(uint32_t block, MetaPoolBlock* blk) {
    for (uint32_t i = 0; i < g_retire_len; i++) {
        if (g_retire[i].block != block) continue;
        uint32_t offset = g_retire[i].offset;
        if (offset < MPOOL_BLOCK_HEADER ||
            offset + RECORD_HEADER_SIZE > TAGFS_BLOCK_SIZE) continue;
        memset((uint8_t*)blk + offset + 2, 0, 4);
    }
    return count_kept_records(blk);
}

static TagFSMetadata* g_mirror          = NULL;
static uint32_t       g_mirror_capacity = 0;
static bool*          g_mirror_valid    = NULL;

typedef struct {
    uint32_t block;
    uint32_t offset;
} MetaPoolWhere;

static MetaPoolWhere* g_mirror_where = NULL;

static volatile uint32_t g_mirror_seq   = 0;


int meta_pool_init(uint32_t first_block) {
    chain_release();
    retire_release();

    memset(&g_current_block, 0, sizeof(MetaPoolBlock));
    g_current_block_num = first_block;
    g_current_dirty     = false;


    TagFSState* fs  = tagfs_get_state();
    uint32_t    run = fs ? fs->layout.data_blocks : 0;
    if (run == 0) {
        kprintf("[MetaPool] this volume states a data run of no blocks at all, "
                "so there is nowhere for a metadata pool to be — it is not "
                "mounted\n");
        return -1;
    }

    uint32_t block     = first_block;
    uint32_t hops      = 0;
    uint32_t disagreed = 0;

    for (;;) {
        if (block >= run) {
            kprintf("[MetaPool] the chain points at block %u and this volume's "
                    "data run is %u blocks — that is outside the volume, so it "
                    "is not mounted\n", block, run);
            chain_release();
            return -1;
        }

        MetaPoolBlock here;
        if (tagfs_read_block(block, &here) != OK) {
            kprintf("[MetaPool] block %u would not read — this volume's "
                    "metadata cannot be reached, so it is not mounted "
                    "(%u block(s) of its pool had been read)\n",
                    block, g_chain_len);
            chain_release();
            return -1;
        }

        if (here.magic != TAGFS_MPOOL_MAGIC) {
            kprintf("[MetaPool] block %u holds 0x%08x where a metadata pool "
                    "should be — this volume is not mounted, and nothing is "
                    "written to it\n", block, here.magic);
            chain_release();
            return -1;
        }

        uint16_t kept = count_kept_records(&here);
        if (kept != here.kept) disagreed++;

        if (chain_append(block, kept) != 0) {
            kprintf("[MetaPool] there is no memory to hold this volume's "
                    "metadata chain at %u blocks — it is not mounted\n",
                    g_chain_len + 1);
            chain_release();
            return -1;
        }

        g_current_block_num = block;
        g_current_block     = here;
        g_current_block.kept = kept;

        if (here.next_block == 0) break;

        if (++hops >= run) {
            kprintf("[MetaPool] this volume's metadata chain has taken %u hops "
                    "in a data run of %u blocks, so it is going round in a "
                    "circle — it is not mounted\n", hops, run);
            chain_release();
            return -1;
        }
        block = here.next_block;
    }

    if (disagreed) {
        kprintf("[MetaPool] %u of this volume's %u metadata block(s) state a "
                "kept-record count the records themselves do not bear out — "
                "the records are counted, and that is the number used\n",
                disagreed, g_chain_len);
    }

    debug_printf("[MetaPool] initialized: first_block=%u blocks=%u last_block=%u\n",
                 first_block, g_chain_len, g_current_block_num);
    return 0;
}

int meta_pool_flush(void) {
    if (!g_current_dirty) {
        return 0;
    }
    if (tagfs_write_block(g_current_block_num, &g_current_block) != OK) {
        kprintf("[MetaPool] block %u would not take this volume's newest "
                "metadata — it stays in memory and is written again at the "
                "next chance\n", g_current_block_num);
        return -1;
    }
    g_current_dirty = false;
    return 0;
}

void meta_pool_shutdown(bool write_back) {
    if (write_back) {
        meta_pool_flush();
    }
    g_current_dirty = false;

    if (g_mirror) {
        for (uint32_t i = 0; i < g_mirror_capacity; i++) {
            if (g_mirror_valid && g_mirror_valid[i]) {
                tagfs_metadata_free(&g_mirror[i]);
            }
        }
        kfree(g_mirror);
        g_mirror = NULL;
    }
    if (g_mirror_valid) {
        kfree(g_mirror_valid);
        g_mirror_valid = NULL;
    }
    if (g_mirror_where) {
        kfree(g_mirror_where);
        g_mirror_where = NULL;
    }
    g_mirror_capacity = 0;

    chain_release();
    retire_release();

    memset(&g_current_block, 0, sizeof(MetaPoolBlock));
    g_current_block_num = 0;
    g_current_dirty     = false;

    debug_printf("[MetaPool] shutdown\n");
}


uint32_t meta_pool_record_size(const TagFSMetadata* meta) {
    uint32_t name_len = meta->filename ? (uint32_t)strlen(meta->filename) : 0;
    return (uint32_t)RECORD_HEADER_SIZE
           + (uint32_t)meta->tag_count    * sizeof(uint16_t)
           + (uint32_t)meta->extent_count * sizeof(FileExtent)
           + name_len;
}


static uint32_t pack_record(const TagFSMetadata* meta, uint8_t* buf) {
    uint32_t name_len   = meta->filename ? (uint32_t)strlen(meta->filename) : 0;
    uint16_t record_len = (uint16_t)meta_pool_record_size(meta);

    uint32_t pos = 0;

    memcpy(buf + pos, &record_len,          sizeof(uint16_t));  pos += 2;
    memcpy(buf + pos, &meta->file_id,       sizeof(uint32_t));  pos += 4;
    memcpy(buf + pos, &meta->flags,         sizeof(uint32_t));  pos += 4;
    memcpy(buf + pos, &meta->size,          sizeof(uint64_t));  pos += 8;
    memcpy(buf + pos, &meta->created_time,  sizeof(uint64_t));  pos += 8;
    memcpy(buf + pos, &meta->modified_time, sizeof(uint64_t));  pos += 8;
    memcpy(buf + pos, &meta->tag_count,     sizeof(uint16_t));  pos += 2;
    memcpy(buf + pos, &meta->extent_count,  sizeof(uint16_t));  pos += 2;
    memcpy(buf + pos, &name_len,            sizeof(uint16_t));  pos += 2;

    uint16_t zero_crc = 0;
    memcpy(buf + pos, &zero_crc, sizeof(uint16_t));             pos += 2;

    if (meta->tag_count > 0 && meta->tag_ids) {
        uint32_t tag_bytes = (uint32_t)meta->tag_count * sizeof(uint16_t);
        memcpy(buf + pos, meta->tag_ids, tag_bytes);
        pos += tag_bytes;
    }

    if (meta->extent_count > 0 && meta->extents) {
        TagFSState* fs = tagfs_get_state();
        uint32_t total_blocks = (fs && fs->initialized) ? fs->layout.data_blocks : 0;

        for (uint16_t e = 0; e < meta->extent_count; e++) {
            uint32_t start = meta->extents[e].start_block;
            uint32_t count = meta->extents[e].block_count;
            bool overflow  = (count > 0) && ((uint64_t)start + count > (uint64_t)0xFFFFFFFFU);
            bool oob       = (total_blocks > 0) &&
                             (start >= total_blocks || (uint64_t)start + count > total_blocks);
            if (count == 0 || overflow || oob) {
                debug_printf("[MetaPool] pack: extent %u invalid (start=%u count=%u total=%u) — skipping write\n",
                             e, start, count, total_blocks);
                return 0;
            }
        }

        uint32_t extent_bytes = (uint32_t)meta->extent_count * sizeof(FileExtent);
        memcpy(buf + pos, meta->extents, extent_bytes);
        pos += extent_bytes;
    }

    if (name_len > 0 && meta->filename) {
        memcpy(buf + pos, meta->filename, name_len);
        pos += name_len;
    }

    uint16_t crc = meta_crc16(buf, (uint32_t)record_len);
    memcpy(buf + RECORD_CRC_OFFSET, &crc, sizeof(uint16_t));

    return (uint32_t)record_len;
}

static int unpack_record(const uint8_t* buf, TagFSMetadata* out) {
    uint32_t pos = 0;

    uint16_t record_len;
    memcpy(&record_len,         buf + pos, sizeof(uint16_t));  pos += 2;

    if (record_len < RECORD_HEADER_SIZE || record_len > TAGFS_MPOOL_DATA_SIZE) {
        debug_printf("[MetaPool] unpack: invalid record_len=%u\n", record_len);
        return -1;
    }

    uint16_t stored_crc;
    memcpy(&stored_crc, buf + RECORD_CRC_OFFSET, sizeof(uint16_t));
    {
        uint8_t check_buf[TAGFS_MPOOL_DATA_SIZE];
        memcpy(check_buf, buf, record_len);
        memset(check_buf + RECORD_CRC_OFFSET, 0, 2);
        uint16_t computed_crc = meta_crc16(check_buf, record_len);
        if (computed_crc != stored_crc) {
            debug_printf("[MetaPool] unpack: CRC16 mismatch (stored=0x%04x computed=0x%04x)\n",
                         stored_crc, computed_crc);
            return -1;
        }
    }

    memcpy(&out->file_id,       buf + pos, sizeof(uint32_t));  pos += 4;
    memcpy(&out->flags,         buf + pos, sizeof(uint32_t));  pos += 4;
    memcpy(&out->size,          buf + pos, sizeof(uint64_t));  pos += 8;
    memcpy(&out->created_time,  buf + pos, sizeof(uint64_t));  pos += 8;
    memcpy(&out->modified_time, buf + pos, sizeof(uint64_t));  pos += 8;
    memcpy(&out->tag_count,     buf + pos, sizeof(uint16_t));  pos += 2;
    memcpy(&out->extent_count,  buf + pos, sizeof(uint16_t));  pos += 2;

    uint16_t name_len;
    memcpy(&name_len, buf + pos, sizeof(uint16_t));            pos += 2;

    pos += 2;

    out->tag_ids  = NULL;
    out->extents  = NULL;
    out->filename = NULL;

    if (out->tag_count > 0) {
        out->tag_ids = kmalloc(sizeof(uint16_t) * out->tag_count);
        if (!out->tag_ids) {
            debug_printf("[MetaPool] unpack: kmalloc failed for tag_ids (count=%u)\n",
                         out->tag_count);
            return -1;
        }
        uint32_t tag_bytes = (uint32_t)out->tag_count * sizeof(uint16_t);
        memcpy(out->tag_ids, buf + pos, tag_bytes);
        pos += tag_bytes;
    }

    if (out->extent_count > 0) {
        out->extents = kmalloc(sizeof(FileExtent) * out->extent_count);
        if (!out->extents) {
            debug_printf("[MetaPool] unpack: kmalloc failed for extents (count=%u)\n",
                         out->extent_count);
            kfree(out->tag_ids);
            out->tag_ids = NULL;
            return -1;
        }
        uint32_t extent_bytes = (uint32_t)out->extent_count * sizeof(FileExtent);
        memcpy(out->extents, buf + pos, extent_bytes);
        pos += extent_bytes;

        TagFSState* fs = tagfs_get_state();
        uint32_t total_blocks = (fs && fs->initialized) ? fs->layout.data_blocks : 0;

        for (uint16_t e = 0; e < out->extent_count; e++) {
            uint32_t start = out->extents[e].start_block;
            uint32_t count = out->extents[e].block_count;
            bool overflow  = (count > 0) && ((uint64_t)start + count > (uint64_t)0xFFFFFFFFU);
            bool oob       = (total_blocks > 0) &&
                             (start >= total_blocks || (uint64_t)start + count > total_blocks);
            if (count == 0 || overflow || oob) {
                debug_printf("[MetaPool] WARNING: extent %u invalid (start=%u count=%u total=%u) — discarding extents\n",
                             e, start, count, total_blocks);
                kfree(out->extents);
                out->extents      = NULL;
                out->extent_count = 0;
                break;
            }
        }
    }

    out->filename = kmalloc(name_len + 1);
    if (!out->filename) {
        debug_printf("[MetaPool] unpack: kmalloc failed for filename (len=%u)\n", name_len);
        kfree(out->tag_ids);
        kfree(out->extents);
        out->tag_ids = NULL;
        out->extents = NULL;
        return -1;
    }
    if (name_len > 0) {
        memcpy(out->filename, buf + pos, name_len);
    }
    out->filename[name_len] = '\0';

    return 0;
}


void tagfs_metadata_free(TagFSMetadata* meta) {
    if (meta->filename) { kfree(meta->filename); meta->filename = NULL; }
    if (meta->tag_ids)  { kfree(meta->tag_ids);  meta->tag_ids  = NULL; }
    if (meta->extents)  { kfree(meta->extents);  meta->extents  = NULL; }
    meta->tag_count    = 0;
    meta->extent_count = 0;
}


static bool mirror_deep_copy(const TagFSMetadata* src, TagFSMetadata* dst) {
    dst->file_id       = src->file_id;
    dst->flags         = src->flags;
    dst->size          = src->size;
    dst->created_time  = src->created_time;
    dst->modified_time = src->modified_time;
    dst->tag_count     = src->tag_count;
    dst->extent_count  = src->extent_count;
    dst->tag_ids  = NULL;
    dst->extents  = NULL;
    dst->filename = NULL;

    if (src->tag_count > 0 && src->tag_ids) {
        dst->tag_ids = kmalloc(sizeof(uint16_t) * src->tag_count);
        if (!dst->tag_ids) return false;
        memcpy(dst->tag_ids, src->tag_ids, sizeof(uint16_t) * src->tag_count);
    }
    if (src->extent_count > 0 && src->extents) {
        dst->extents = kmalloc(sizeof(FileExtent) * src->extent_count);
        if (!dst->extents) {
            kfree(dst->tag_ids);
            dst->tag_ids = NULL;
            return false;
        }
        memcpy(dst->extents, src->extents, sizeof(FileExtent) * src->extent_count);
    }
    if (src->filename) {
        size_t len = strlen(src->filename);
        dst->filename = kmalloc(len + 1);
        if (!dst->filename) {
            kfree(dst->tag_ids);
            kfree(dst->extents);
            dst->tag_ids = NULL;
            dst->extents = NULL;
            return false;
        }
        memcpy(dst->filename, src->filename, len);
        dst->filename[len] = '\0';
    }
    return true;
}


int meta_pool_mirror_init(uint32_t max_file_id) {
    g_mirror_capacity = max_file_id + 1;
    g_mirror       = kmalloc(sizeof(TagFSMetadata) * g_mirror_capacity);
    g_mirror_valid = kmalloc(sizeof(bool) * g_mirror_capacity);
    g_mirror_where = kmalloc(sizeof(MetaPoolWhere) * g_mirror_capacity);
    if (!g_mirror || !g_mirror_valid || !g_mirror_where) {
        debug_printf("[MetaPool] Mirror: alloc failed\n");
        return -1;
    }
    memset(g_mirror,       0, sizeof(TagFSMetadata) * g_mirror_capacity);
    memset(g_mirror_valid, 0, sizeof(bool)          * g_mirror_capacity);
    memset(g_mirror_where, 0, sizeof(MetaPoolWhere) * g_mirror_capacity);

    uint32_t loaded = 0;
    for (uint32_t link = 0; link < g_chain_len; link++) {
        uint8_t buf[TAGFS_BLOCK_SIZE];
        uint32_t block_num = g_chain[link].block;

        if (block_num == g_current_block_num) {
            memcpy(buf, &g_current_block, sizeof(MetaPoolBlock));
        } else if (tagfs_read_block(block_num, buf) != OK) {
            kprintf("[MetaPool] block %u of this volume's metadata read at "
                    "mount and will not read now — the files it names are not "
                    "in the cache\n", block_num);
            continue;
        }

        MetaPoolBlock* hdr = (MetaPoolBlock*)buf;
        if (hdr->magic != TAGFS_MPOOL_MAGIC) continue;

        uint32_t pos = 0;
        while (pos < hdr->used_bytes) {
            uint16_t record_len;
            memcpy(&record_len, hdr->payload + pos, sizeof(uint16_t));
            if (record_len == 0 || record_len > TAGFS_MPOOL_DATA_SIZE - pos) break;

            uint32_t file_id;
            memcpy(&file_id, hdr->payload + pos + 2, sizeof(uint32_t));

            if (file_id > 0 && file_id < g_mirror_capacity) {
                TagFSMetadata meta;
                memset(&meta, 0, sizeof(meta));
                if (unpack_record(hdr->payload + pos, &meta) == 0) {
                    if (g_mirror_valid[file_id]) {
                        tagfs_metadata_free(&g_mirror[file_id]);
                    }
                    g_mirror[file_id] = meta;
                    g_mirror_valid[file_id] = true;
                    g_mirror_where[file_id].block  = block_num;
                    g_mirror_where[file_id].offset = MPOOL_BLOCK_HEADER + pos;
                    loaded++;
                }
            }
            pos += record_len;
        }
    }

    debug_printf("[MetaPool] Mirror loaded: %u entries\n", loaded);
    return 0;
}

int meta_pool_mirror_where(uint32_t file_id, uint32_t* out_block, uint32_t* out_offset) {
    if (!g_mirror_where || !g_mirror_valid || file_id >= g_mirror_capacity)
        return -1;
    if (!g_mirror_valid[file_id] || g_mirror_where[file_id].offset == 0)
        return -1;
    *out_block  = g_mirror_where[file_id].block;
    *out_offset = g_mirror_where[file_id].offset;
    return 0;
}

int meta_pool_read_cached(uint32_t file_id, TagFSMetadata* out) {
    for (;;) {
        uint32_t seq = __atomic_load_n(&g_mirror_seq, __ATOMIC_ACQUIRE);
        if (seq & 1) { __asm__ volatile("pause"); continue; }

        if (!g_mirror || file_id >= g_mirror_capacity || !g_mirror_valid[file_id]) {
            return -1;
        }
        if (!mirror_deep_copy(&g_mirror[file_id], out))
            return -1;

        __asm__ volatile("" ::: "memory");
        if (__atomic_load_n(&g_mirror_seq, __ATOMIC_ACQUIRE) == seq)
            return 0;

        tagfs_metadata_free(out);
    }
}


int meta_pool_write(const TagFSMetadata* meta, uint32_t* out_block, uint32_t* out_offset) {
    uint32_t record_size = meta_pool_record_size(meta);

    if (record_size > TAGFS_MPOOL_DATA_SIZE) {
        debug_printf("[MetaPool] write: record too large (%u > %u) for file_id=%u\n",
                     record_size, TAGFS_MPOOL_DATA_SIZE, meta->file_id);
        return -1;
    }


    if (record_size > (uint32_t)(TAGFS_MPOOL_DATA_SIZE - g_current_block.used_bytes)) {
        debug_printf("[MetaPool] CHAINING: block %u full (used=%u need=%u)\n",
                     g_current_block_num, g_current_block.used_bytes, record_size);

        if (g_current_block.next_block != 0) {
            kprintf("[MetaPool] block %u is being appended to and already "
                    "points at block %u — the pool is not writing over that "
                    "link, because everything past it would go off the chain "
                    "for good\n", g_current_block_num, g_current_block.next_block);
            return -1;
        }

        if (chain_reserve(g_chain_len + 1) != 0) {
            kprintf("[MetaPool] there is no memory to lengthen this volume's "
                    "metadata chain past %u blocks\n", g_chain_len);
            return -1;
        }

        uint32_t new_block;
        int alloc_ret = tagfs_alloc_blocks_internal(1, &new_block);
        if (alloc_ret != 0) {
            debug_printf("[MetaPool] CHAINING: failed to allocate new block\n");
            return -1;
        }

        memset(&g_fresh_block, 0, sizeof(g_fresh_block));
        g_fresh_block.magic = TAGFS_MPOOL_MAGIC;
        if (tagfs_write_block(new_block, &g_fresh_block) != OK) {
            tagfs_free_blocks_internal(new_block, 1);
            kprintf("[MetaPool] block %u would not take a metadata pool "
                    "header, so this volume's chain is left as it was and the "
                    "record is not written\n", new_block);
            return -1;
        }

        debug_printf("[MetaPool] CHAINING: linking block %u -> %u\n", g_current_block_num, new_block);
        g_current_block.next_block = new_block;
        int flush_ret = tagfs_write_block(g_current_block_num, &g_current_block);
        if (flush_ret != OK) {
            g_current_block.next_block = 0;
            tagfs_free_blocks_internal(new_block, 1);
            kprintf("[MetaPool] block %u would not take the link to block %u, "
                    "so the chain is left as it was and the record is not "
                    "written\n", g_current_block_num, new_block);
            return -1;
        }

        g_current_block_num = new_block;
        g_current_block     = g_fresh_block;
        g_current_dirty = true;
        chain_append(new_block, 0);
        debug_printf("[MetaPool] CHAINING: switched to block %u\n", g_current_block_num);
    }

    uint8_t* dest = g_current_block.payload + g_current_block.used_bytes;
    uint32_t packed = pack_record(meta, dest);
    if (packed == 0) {
        debug_printf("[MetaPool] write: pack_record failed for file_id=%u — invalid extent\n",
                     meta->file_id);
        return -1;
    }

    *out_block  = g_current_block_num;
    *out_offset = MPOOL_BLOCK_HEADER + g_current_block.used_bytes;

    g_current_block.used_bytes   += (uint16_t)record_size;
    g_current_block.record_count++;
    g_current_dirty = true;

    if (meta->file_id != 0) {
        g_current_block.kept++;
        if (g_chain_len) g_chain[g_chain_len - 1].kept = g_current_block.kept;
    }

    debug_printf("[MetaPool] write: file_id=%u block=%u offset=%u size=%u\n",
                 meta->file_id, *out_block, *out_offset, record_size);

    if (g_mirror && meta->file_id < g_mirror_capacity) {
        __atomic_fetch_add(&g_mirror_seq, 1, __ATOMIC_RELEASE);
        if (g_mirror_valid[meta->file_id]) {
            tagfs_metadata_free(&g_mirror[meta->file_id]);
        }
        if (mirror_deep_copy(meta, &g_mirror[meta->file_id])) {
            g_mirror_valid[meta->file_id] = true;
            if (g_mirror_where) {
                g_mirror_where[meta->file_id].block  = *out_block;
                g_mirror_where[meta->file_id].offset = *out_offset;
            }
        } else {
            g_mirror_valid[meta->file_id] = false;
            debug_printf("[MetaPool] mirror: kmalloc failed for file_id=%u — entry invalidated\n",
                         meta->file_id);
        }
        __atomic_fetch_add(&g_mirror_seq, 1, __ATOMIC_RELEASE);
    }

    TagFS_SelfHealOnMetadataWrite(*out_block, (const uint8_t*)&g_current_block);

    return 0;
}


int meta_pool_read(uint32_t block, uint32_t offset, TagFSMetadata* out) {
    if (block == g_current_block_num) {
        int r = unpack_record((uint8_t *)&g_current_block + offset, out);
        return r;
    }

    uint8_t buf[TAGFS_BLOCK_SIZE];
    if (tagfs_read_block(block, buf) != OK) {
        debug_printf("[MetaPool] read: tagfs_read_block failed for block %u\n", block);
        return -1;
    }
    return unpack_record(buf + offset, out);
}


static void mirror_forget(uint32_t file_id) {
    if (!g_mirror || file_id >= g_mirror_capacity || !g_mirror_valid[file_id])
        return;
    __atomic_fetch_add(&g_mirror_seq, 1, __ATOMIC_RELEASE);
    tagfs_metadata_free(&g_mirror[file_id]);
    memset(&g_mirror[file_id], 0, sizeof(TagFSMetadata));
    g_mirror_valid[file_id] = false;
    __atomic_fetch_add(&g_mirror_seq, 1, __ATOMIC_RELEASE);
}

static bool unlink_empty_block(uint32_t index, uint32_t says_next) {
    uint32_t block = g_chain[index].block;
    uint32_t prev  = g_chain[index - 1].block;
    uint32_t next  = g_chain[index + 1].block;

    if (says_next != next) {
        kprintf("[MetaPool] block %u points at %u on the medium and at %u on "
                "the chain this volume was mounted with — nothing is "
                "unlinked\n", block, says_next, next);
        return false;
    }

    MetaPoolBlock* before = kmalloc(sizeof(MetaPoolBlock));
    if (!before) return false;

    bool done = false;
    if (tagfs_read_block(prev, before) != OK) {
        kprintf("[MetaPool] block %u of this volume's metadata is empty and "
                "block %u in front of it would not read, so it stays on the "
                "chain\n", block, prev);
    } else if (before->magic != TAGFS_MPOOL_MAGIC) {
        kprintf("[MetaPool] block %u in front of the empty block %u holds "
                "0x%08x instead of a metadata pool — the chain is left "
                "alone\n", prev, block, before->magic);
    } else {
        before->next_block = next;
        if (tagfs_write_block(prev, before) != OK) {
            kprintf("[MetaPool] block %u would not take the link past the "
                    "empty block %u, so that block stays on the chain\n",
                    prev, block);
        } else {
            tagfs_free_blocks_internal(block, 1);
            chain_drop(index);
            done = true;
            debug_printf("[MetaPool] block %u held nothing and went back: %u -> %u\n",
                         block, prev, next);
        }
    }

    kfree(before);
    return done;
}

int meta_pool_delete(uint32_t block, uint32_t offset) {
    uint32_t index = chain_find(block);
    if (index == g_chain_len) {
        kprintf("[MetaPool] a file's metadata is said to be in block %u and "
                "that block is not on this volume's metadata chain — nothing "
                "is written to it\n", block);
        return -1;
    }

    if (block == g_current_block_num) {
        uint8_t *rec = (uint8_t *)&g_current_block + offset;
        uint32_t del_file_id;
        memcpy(&del_file_id, rec + 2, sizeof(uint32_t));
        if (del_file_id == 0) {
            return 0;
        }
        mirror_forget(del_file_id);
        memset(rec + 2, 0, 4);

        g_current_block.kept = count_kept_records(&g_current_block);
        g_chain[index].kept = g_current_block.kept;
        g_current_dirty = true;

        return 0;
    }

    uint8_t buf[TAGFS_BLOCK_SIZE];
    if (tagfs_read_block(block, buf) != OK) {
        debug_printf("[MetaPool] delete: tagfs_read_block failed for block %u\n", block);
        return -1;
    }

    MetaPoolBlock* here = (MetaPoolBlock*)buf;

    uint32_t del_file_id;
    memcpy(&del_file_id, buf + offset + 2, sizeof(uint32_t));
    if (del_file_id == 0) {
        return 0;
    }
    if (retire_owed(block, offset)) {
        return 0;
    }

    if (retire_owe(block, offset) != 0) {
        kprintf("[MetaPool] there is no memory to record that block %u owes a "
                "retired record — the record stays as it is\n", block);
        return -1;
    }
    mirror_forget(del_file_id);

    g_chain[index].kept = retire_apply(block, here);
    return 0;
}

int meta_pool_flush_retires(void) {
    if (g_retire_len == 0) return 0;

    TagFSState* fs = tagfs_get_state();
    if (!fs) return -1;

    int rc = 0;
    spin_lock(&fs->lock);

    while (g_retire_len > 0) {
        uint32_t block = g_retire[0].block;

        uint32_t index = chain_find(block);
        if (index == g_chain_len || block == g_current_block_num) {
            retire_paid(block);
            continue;
        }

        uint8_t buf[TAGFS_BLOCK_SIZE];
        if (tagfs_read_block(block, buf) != OK) {
            kprintf("[MetaPool] block %u would not read, so the records it "
                    "still owes stay as they are and are retired again at the "
                    "next chance\n", block);
            rc = -1;
            break;
        }

        MetaPoolBlock* here = (MetaPoolBlock*)buf;
        here->kept = retire_apply(block, here);
        g_chain[index].kept = here->kept;

        bool interior = (index > 0) && (index + 1 < g_chain_len);
        if (here->kept == 0 && interior &&
            unlink_empty_block(index, here->next_block)) {
            retire_paid(block);
            continue;
        }

        if (tagfs_write_block(block, buf) != OK) {
            kprintf("[MetaPool] block %u would not take the records it has "
                    "retired — they stay as they are and are written again at "
                    "the next chance\n", block);
            rc = -1;
            break;
        }
        retire_paid(block);
    }

    spin_unlock(&fs->lock);
    return rc;
}