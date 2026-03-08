#include "meta_pool.h"

#define RECORD_HEADER_SIZE  40
#define MPOOL_BLOCK_HEADER  16

static uint32_t     g_first_block      = 0;
static uint32_t     g_block_count      = 0;
static MetaPoolBlock g_current_block;
static uint32_t     g_current_block_num = 0;
static bool         g_current_dirty    = false;
static spinlock_t   g_lock;

// ---------------------------------------------------------------------------
// Public: init / shutdown / flush
// ---------------------------------------------------------------------------

int meta_pool_init(uint32_t first_block, uint32_t block_count) {
    g_first_block = first_block;
    g_block_count = block_count;

    memset(&g_current_block, 0, sizeof(MetaPoolBlock));
    g_current_block.magic        = TAGFS_MPOOL_MAGIC;
    g_current_block.used_bytes   = 0;
    g_current_block.record_count = 0;
    g_current_block.next_block   = 0;

    g_current_block_num = first_block;
    g_current_dirty     = false;

    spinlock_init(&g_lock);

    int read_result = tagfs_read_block(first_block, &g_current_block);
    if (read_result < 0) {
        debug_printf("[MetaPool] init: failed to read first block %u, starting fresh\n", first_block);
        memset(&g_current_block, 0, sizeof(MetaPoolBlock));
        g_current_block.magic = TAGFS_MPOOL_MAGIC;
        g_current_dirty = true;
        debug_printf("[MetaPool] initialized (fresh): first_block=%u block_count=%u\n",
                     first_block, block_count);
        return 0;
    }

    if (g_current_block.magic != TAGFS_MPOOL_MAGIC) {
        debug_printf("[MetaPool] init: bad magic 0x%x on block %u, starting fresh\n",
                     g_current_block.magic, first_block);
        memset(&g_current_block, 0, sizeof(MetaPoolBlock));
        g_current_block.magic = TAGFS_MPOOL_MAGIC;
        g_current_dirty = true;
        debug_printf("[MetaPool] initialized (fresh): first_block=%u block_count=%u\n",
                     first_block, block_count);
        return 0;
    }

    while (g_current_block.next_block != 0) {
        uint32_t next = g_current_block.next_block;
        MetaPoolBlock next_block_buf;
        int chain_result = tagfs_read_block(next, &next_block_buf);
        if (chain_result < 0) {
            debug_printf("[MetaPool] init: chain read failed at block %u, stopping\n", next);
            break;
        }
        if (next_block_buf.magic != TAGFS_MPOOL_MAGIC) {
            debug_printf("[MetaPool] init: bad magic in chain at block %u, stopping\n", next);
            break;
        }
        g_current_block_num = next;
        g_current_block     = next_block_buf;
    }

    debug_printf("[MetaPool] initialized: first_block=%u block_count=%u last_block=%u\n",
                 first_block, block_count, g_current_block_num);
    return 0;
}

int meta_pool_flush(void) {
    if (!g_current_dirty) {
        return 0;
    }
    int result = tagfs_write_block(g_current_block_num, &g_current_block);
    if (result < 0) {
        debug_printf("[MetaPool] flush: write failed for block %u\n", g_current_block_num);
        return result;
    }
    g_current_dirty = false;
    return 0;
}

void meta_pool_shutdown(void) {
    meta_pool_flush();

    memset(&g_current_block, 0, sizeof(MetaPoolBlock));
    g_first_block       = 0;
    g_block_count       = 0;
    g_current_block_num = 0;
    g_current_dirty     = false;

    debug_printf("[MetaPool] shutdown\n");
}

// ---------------------------------------------------------------------------
// Public: record size
// ---------------------------------------------------------------------------

uint32_t meta_pool_record_size(const TagFSMetadata* meta) {
    uint32_t name_len = meta->filename ? (uint32_t)strlen(meta->filename) : 0;
    return (uint32_t)RECORD_HEADER_SIZE
           + (uint32_t)meta->tag_count    * sizeof(uint16_t)
           + (uint32_t)meta->extent_count * sizeof(FileExtent)
           + name_len;
}

// ---------------------------------------------------------------------------
// Internal: pack / unpack
// ---------------------------------------------------------------------------

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

    if (meta->tag_count > 0 && meta->tag_ids) {
        uint32_t tag_bytes = (uint32_t)meta->tag_count * sizeof(uint16_t);
        memcpy(buf + pos, meta->tag_ids, tag_bytes);
        pos += tag_bytes;
    }

    if (meta->extent_count > 0 && meta->extents) {
        uint32_t extent_bytes = (uint32_t)meta->extent_count * sizeof(FileExtent);
        memcpy(buf + pos, meta->extents, extent_bytes);
        pos += extent_bytes;
    }

    if (name_len > 0 && meta->filename) {
        memcpy(buf + pos, meta->filename, name_len);
        pos += name_len;
    }

    return (uint32_t)record_len;
}

static int unpack_record(const uint8_t* buf, TagFSMetadata* out) {
    uint32_t pos = 0;

    uint16_t record_len;
    memcpy(&record_len,         buf + pos, sizeof(uint16_t));  pos += 2;
    memcpy(&out->file_id,       buf + pos, sizeof(uint32_t));  pos += 4;
    memcpy(&out->flags,         buf + pos, sizeof(uint32_t));  pos += 4;
    memcpy(&out->size,          buf + pos, sizeof(uint64_t));  pos += 8;
    memcpy(&out->created_time,  buf + pos, sizeof(uint64_t));  pos += 8;
    memcpy(&out->modified_time, buf + pos, sizeof(uint64_t));  pos += 8;
    memcpy(&out->tag_count,     buf + pos, sizeof(uint16_t));  pos += 2;
    memcpy(&out->extent_count,  buf + pos, sizeof(uint16_t));  pos += 2;

    uint16_t name_len;
    memcpy(&name_len, buf + pos, sizeof(uint16_t));            pos += 2;

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

// ---------------------------------------------------------------------------
// Public: free
// ---------------------------------------------------------------------------

void tagfs_metadata_free(TagFSMetadata* meta) {
    if (meta->filename) { kfree(meta->filename); meta->filename = NULL; }
    if (meta->tag_ids)  { kfree(meta->tag_ids);  meta->tag_ids  = NULL; }
    if (meta->extents)  { kfree(meta->extents);  meta->extents  = NULL; }
    meta->tag_count    = 0;
    meta->extent_count = 0;
}

// ---------------------------------------------------------------------------
// Public: write
// ---------------------------------------------------------------------------

int meta_pool_write(const TagFSMetadata* meta, uint32_t* out_block, uint32_t* out_offset) {
    spin_lock(&g_lock);

    uint32_t record_size = meta_pool_record_size(meta);

    if (record_size > TAGFS_MPOOL_DATA_SIZE) {
        debug_printf("[MetaPool] write: record too large (%u > %u) for file_id=%u\n",
                     record_size, TAGFS_MPOOL_DATA_SIZE, meta->file_id);
        spin_unlock(&g_lock);
        return -1;
    }

    if (record_size > (uint32_t)(TAGFS_MPOOL_DATA_SIZE - g_current_block.used_bytes)) {
        debug_printf("[MetaPool] write: block %u full (used=%u need=%u), chaining to new block\n",
                     g_current_block_num, g_current_block.used_bytes, record_size);

        // Allocate a new block for the chain (safe: g_lock != g_state.lock)
        uint32_t new_block;
        int alloc_ret = tagfs_alloc_blocks(1, &new_block);
        if (alloc_ret != 0) {
            debug_printf("[MetaPool] write: failed to allocate new block\n");
            spin_unlock(&g_lock);
            return -1;
        }

        // Link old block → new block, then flush old block to disk
        g_current_block.next_block = new_block;
        int flush_ret = tagfs_write_block(g_current_block_num, &g_current_block);
        if (flush_ret != 0) {
            debug_printf("[MetaPool] write: failed to flush old block %u\n", g_current_block_num);
            spin_unlock(&g_lock);
            return -1;
        }

        // Move to the new block
        g_current_block_num = new_block;
        memset(&g_current_block, 0, sizeof(MetaPoolBlock));
        g_current_block.magic        = TAGFS_MPOOL_MAGIC;
        g_current_block.used_bytes   = 0;
        g_current_block.record_count = 0;
        g_current_block.next_block   = 0;
        g_current_dirty = false;  // fresh block, not yet dirty
    }

    uint8_t* dest = g_current_block.payload + g_current_block.used_bytes;
    pack_record(meta, dest);

    *out_block  = g_current_block_num;
    *out_offset = MPOOL_BLOCK_HEADER + g_current_block.used_bytes;

    g_current_block.used_bytes   += (uint16_t)record_size;
    g_current_block.record_count++;
    g_current_dirty = true;

    debug_printf("[MetaPool] write: file_id=%u block=%u offset=%u size=%u\n",
                 meta->file_id, *out_block, *out_offset, record_size);

    spin_unlock(&g_lock);
    return 0;
}

// ---------------------------------------------------------------------------
// Public: read
// ---------------------------------------------------------------------------

int meta_pool_read(uint32_t block, uint32_t offset, TagFSMetadata* out) {
    uint8_t buf[TAGFS_BLOCK_SIZE];
    int result = tagfs_read_block(block, buf);
    if (result < 0) {
        debug_printf("[MetaPool] read: tagfs_read_block failed for block %u\n", block);
        return result;
    }
    return unpack_record(buf + offset, out);
}

// ---------------------------------------------------------------------------
// Public: delete
// ---------------------------------------------------------------------------

int meta_pool_delete(uint32_t block, uint32_t offset) {
    uint8_t buf[TAGFS_BLOCK_SIZE];
    int result = tagfs_read_block(block, buf);
    if (result < 0) {
        debug_printf("[MetaPool] delete: tagfs_read_block failed for block %u\n", block);
        return result;
    }
    // record layout: uint16_t record_len, then uint32_t file_id
    memset(buf + offset + 2, 0, 4);
    result = tagfs_write_block(block, buf);
    if (result < 0) {
        debug_printf("[MetaPool] delete: tagfs_write_block failed for block %u\n", block);
        return result;
    }
    return 0;
}
