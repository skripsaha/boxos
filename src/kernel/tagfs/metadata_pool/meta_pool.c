#include "meta_pool.h"
#include "../tagfs.h"
#include "tagfs.h"
#include "../../lib/kernel/crypto.h"

// Forward declarations - internal functions in tagfs.c. Both assume the caller
// already holds g_state.lock, which every path into this file does.
extern int  tagfs_alloc_blocks_internal(uint32_t count, uint32_t *out_start_block);
extern void tagfs_free_blocks_internal(uint32_t start_block, uint32_t count);

#define RECORD_HEADER_SIZE  42   // 40 bytes of fields + 2 bytes CRC16
#define MPOOL_BLOCK_HEADER  16
#define RECORD_CRC_OFFSET   40   // CRC16 stored at bytes [40..41] of packed record

// CRC16 wrapper using shared crypto library
static uint16_t meta_crc16(const uint8_t* data, uint32_t len) {
    return KCrc16(data, len);
}

static MetaPoolBlock g_current_block;
static uint32_t     g_current_block_num = 0;
static bool         g_current_dirty    = false;

/* The empty header a block is stamped with when the chain lengthens. BSS and
 * not a local, for the reason tag_registry.c states over its own scratch: a
 * MetaPoolBlock is exactly 4 KiB, meta_pool_write is reached from the K-Core
 * continuation that finishes an async write, and a 4 KiB frame there competes
 * with nested IRQ frames on a per-cpu stack. Every path into this file holds
 * g_state.lock, so one shared buffer has one user at a time. */
static MetaPoolBlock g_fresh_block;
// Note: No local lock - use g_state.lock from tagfs.c for all operations
// static spinlock_t   g_lock;

/*
 * ‼ THE CHAIN IS WHAT THE MEDIUM SAYS IT IS, AND THIS IS WHERE IT IS KEPT.
 *
 * The pool used to be bounded by a number in the Deed — metadata_pool_blocks,
 * written once when the volume was made and never raised by anything that
 * lengthened the chain. A pool that had grown to four blocks on a volume whose
 * Deed still said one was walked two hops and stopped in the middle of itself,
 * and the first write after that overwrote the link to the rest. Every boot
 * severed the previous boot's tail.
 *
 * So the length is not remembered anywhere. It is walked, from the head to the
 * block whose next_block is zero, and what the walk finds is held here: the
 * block numbers in chain order, and how many living records each one carries.
 *
 * Order matters and is the whole reason this is an array and not a set. An
 * emptied block is unlinked by rewriting its PREDECESSOR's next_block, and the
 * predecessor is the entry before it — found in memory, without a second walk
 * of the medium per delete. A chain is a handful of blocks on any volume that
 * has ever been seen, so a linear search through it costs nothing worth
 * measuring and a hash of four entries would cost more to maintain than to
 * skip.
 */
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

// Make room for `want` links. Called before the disk block it will describe is
// allocated, so running out of memory costs the volume nothing.
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

// Where in the chain this block sits, or g_chain_len when it is not in it at
// all — which is a file table pointing somewhere the pool does not reach.
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

/*
 * How many records in this block are still held by a file.
 *
 * A retired record keeps its length and loses its file_id, so the payload is
 * walked by length and counted by identity. This is the number the pool
 * BELIEVES; the one stored in the header is only ever checked against it.
 */
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

/*
 * ‼ A RETIRE IS OWED UNTIL THE NEXT SYNC, AND THIS IS WHERE THE DEBT IS KEPT.
 *
 * Zeroing a retired record's file_id used to be written straight to the medium
 * — and on five of the paths that do it, that write was the ONLY thing that
 * reached the medium between two anchors. The replacing record sat in the
 * pool's block in memory and the entry naming it sat in the file table in
 * memory, so a crash there destroyed the old metadata and kept nothing new:
 * the one durable half of the operation was the half that throws something
 * away.
 *
 * Nothing reads a retire back before the next mount — the mirror answers every
 * metadata read, the file table no longer names the retired record, and the
 * only thing the zeroed file_id is FOR is letting a block's kept count reach
 * zero so the block can be reclaimed. So the retire is owed here, in memory,
 * and paid inside the sync that was going to happen anyway. Flushing before
 * each retire instead was rejected: it costs two extra 4 KiB writes on every
 * metadata update, and one of these paths runs on every file write that moved
 * bytes.
 *
 * Several retires into one block collapse into one write, so the steady-state
 * cost of paying the debt is zero extra I/O.
 */
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

// Take every retire this block is still owed out of the debt.
static void retire_paid(uint32_t block) {
    uint32_t w = 0;
    for (uint32_t r = 0; r < g_retire_len; r++) {
        if (g_retire[r].block != block) g_retire[w++] = g_retire[r];
    }
    g_retire_len = w;
}

/*
 * Zero the file_id of every retire `block` is still owed, in a buffer holding
 * that block as the medium has it, and answer how many records are then kept.
 *
 * The whole debt is applied and the answer COUNTED rather than decremented once
 * per delete: two records retired out of the same block between two syncs would
 * otherwise be counted against a medium copy that still shows the first one
 * alive, and the header would state one more than the payload bears out.
 */
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

// Memory Mirror: in-memory cache of all metadata, indexed by file_id
static TagFSMetadata* g_mirror          = NULL;
static uint32_t       g_mirror_capacity = 0;
static bool*          g_mirror_valid    = NULL;

/*
 * And WHERE in the pool each mirrored record was found.
 *
 * The mirror walk reads the chain in append order, so the last record it sees
 * for a file id is by construction the newest one on the medium. That makes
 * the pool the authority on where a file's metadata is — and a file table that
 * names somewhere else is a file table that missed a flush, which is exactly
 * what the mount fsck has to repair. Without this the mirror knew WHAT the
 * newest record said and not where it was, so it could not say the sentence
 * that corrects the table.
 */
typedef struct {
    uint32_t block;
    uint32_t offset;
} MetaPoolWhere;

static MetaPoolWhere* g_mirror_where = NULL;

// Seqlock for mirror: readers are lock-free, writers bump sequence.
// Odd sequence = write in progress, even = stable.
static volatile uint32_t g_mirror_seq   = 0;

// ---------------------------------------------------------------------------
// Public: init / shutdown / flush
// ---------------------------------------------------------------------------

int meta_pool_init(uint32_t first_block) {
    chain_release();
    retire_release();

    memset(&g_current_block, 0, sizeof(MetaPoolBlock));
    g_current_block_num = first_block;
    g_current_dirty     = false;

    // spinlock_init removed

    /*
     * ‼ THE ONLY BOUND ON THIS WALK IS A TERMINATION PROOF, NOT A LENGTH.
     *
     * The data run has `run` blocks in it. A chain of distinct blocks inside
     * that run therefore cannot be longer than `run`, so a walk that takes
     * `run` hops has visited a block twice and is going round a cycle. The hop
     * count alone proves termination AND catches the cycle, which is why there
     * is no visited-set here: a bitmap of the whole data run, allocated at
     * every mount, would buy the same refusal one hop sooner.
     *
     * A next_block outside the run is refused before it is read, because
     * nothing under tagfs_read_block bounds a block number and a chain that
     * points out of the volume would be followed off the end of it.
     *
     *
     * ‼ NONE OF THESE IS AN EMPTY POOL, AND THIS NO LONGER PRETENDS THEY ARE
     *
     * They used to "start fresh" and return success. This is the record that
     * holds every file's name, size and extents; a mount that begins it afresh
     * is a mount that has decided the volume is empty, and the first write
     * after that allocates over what is still there.
     *
     * On the board it looked like this: one boot worked, the next came up with
     * no files on it, and neither said anything — because it was said with
     * debug_printf, which compiles to nothing in a shipped build. Everything
     * refused below is said with kprintf, and every block of the chain is held
     * to the same standard as the head: it reads, and it says TMPL, or this
     * volume is not mounted and nothing is written to it.
     *
     * ‼ A FAILED READ IS TESTED AS `!= OK`, AND THAT IS NOT A STYLE CHOICE.
     * tagfs_read_block returns an error_t, whose failures are POSITIVE, so the
     * `< 0` this used to ask was never once true. The head block was read into
     * a buffer that had just had TAGFS_MPOOL_MAGIC written into it by hand, so
     * a medium that refused the read left the magic standing and the mount
     * carried on into an empty pool — the exact outcome the paragraph above
     * says cannot happen. The magic is no longer stamped before the read
     * either: what is checked is what came off the medium.
     */
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

    /*
     * ‼ SAID, AND NOT WRITTEN BACK.
     *
     * The stored count is a convenience; the scan is the truth, and the scan
     * has just run. Correcting the medium here would mean writing metadata
     * pool blocks during a mount — the one record on the volume that cannot
     * be half-written without losing files — to fix a number that is derived
     * again on the very next mount. The ordinary writes to these blocks carry
     * the corrected count out by themselves.
     *
     * A volume made before the count existed carries zero in every block and
     * says this once.
     */
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

/*
 * The block being appended to, onto the medium.
 *
 * ‼ THIS RUNS BEFORE THE FILE TABLE IS FLUSHED, AND THAT ORDER IS THE POINT.
 * A file table entry is a block number and a byte offset into this block, so
 * flushing the table first published a name for a record at an offset the
 * medium's copy of the block had not reached yet. A record exists before
 * anything names it. The retires go the other side of the table's flush — see
 * meta_pool_flush_retires.
 */
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
    /* A retire that is still owed dies with the mount and costs the volume
     * nothing: the record it would have zeroed is intact metadata that nothing
     * names any more, which is the same footprint an interrupted delete leaves
     * and exactly what the mount fsck is built to find. */
    retire_release();

    memset(&g_current_block, 0, sizeof(MetaPoolBlock));
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

    // CRC16 — zeroed during computation, stamped below
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

    // Stamp CRC16 over the entire record (with CRC field zeroed)
    uint16_t crc = meta_crc16(buf, (uint32_t)record_len);
    memcpy(buf + RECORD_CRC_OFFSET, &crc, sizeof(uint16_t));

    return (uint32_t)record_len;
}

static int unpack_record(const uint8_t* buf, TagFSMetadata* out) {
    uint32_t pos = 0;

    uint16_t record_len;
    memcpy(&record_len,         buf + pos, sizeof(uint16_t));  pos += 2;

    // Validate record_len sanity before CRC check
    if (record_len < RECORD_HEADER_SIZE || record_len > TAGFS_MPOOL_DATA_SIZE) {
        debug_printf("[MetaPool] unpack: invalid record_len=%u\n", record_len);
        return -1;
    }

    /* Verify CRC16 integrity (zero the CRC field for computation).
     *
     * ‼ A STORED ZERO IS CHECKED LIKE ANY OTHER SUM.
     *
     * It used to be waved through, for compatibility with records written
     * before the CRC existed. That compatibility is empty: the CRC landed
     * 2026-03-17 and the Deed format landed 2026-08-26, and a mount reads a
     * Deed or it does not happen at all — so no volume this kernel can mount
     * predates the sum. What the exception actually did was make about one
     * record in 65536 written by the CURRENT code uncheckable for ever, because
     * CRC-16/CCITT-FALSE legitimately produces 0x0000. */
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

    pos += 2;  // Skip CRC16 field (already validated above)

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
// Internal: deep-copy src into dst (allocates new memory for pointers)
// ---------------------------------------------------------------------------

// Returns true on success, false if any allocation failed (dst is left clean on failure).
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

// ---------------------------------------------------------------------------
// Public: mirror init / cached read
// ---------------------------------------------------------------------------

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

    /*
     * The chain the init walk proved, rather than a second walk of next_block
     * pointers. That walk refused the mount for anything it could not read or
     * did not recognise, so there is nothing left here for a `break` to hide —
     * and one walker of the chain is one fewer place to keep in agreement.
     */
    uint32_t loaded = 0;
    for (uint32_t link = 0; link < g_chain_len; link++) {
        uint8_t buf[TAGFS_BLOCK_SIZE];
        uint32_t block_num = g_chain[link].block;

        // The last block of the chain may hold records that have not reached
        // the medium yet; it is in memory and that copy is the newer one.
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
                    /* Last one wins, and the walk is in append order, so this
                     * ends up holding the NEWEST record for the file and the
                     * place it sits — which is what the mount fsck corrects a
                     * file table against. */
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

/* Where the pool holds this file's newest record, for a caller that has to
 * decide whether a file table entry still names it. Answers -1 when the pool
 * has nothing for this file, which is a different sentence from "the entry is
 * wrong" and is left to the caller to judge. */
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
    // Seqlock read: no spinlock, retry if writer was active.
    // kfree() in kernel heap doesn't unmap pages, so reading stale
    // pointers during a concurrent write won't fault — the seqlock
    // retry detects the race and discards the garbage copy.
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

        // Writer was active during our copy — discard and retry
        tagfs_metadata_free(out);
    }
}

// ---------------------------------------------------------------------------
// Public: write
// ---------------------------------------------------------------------------

int meta_pool_write(const TagFSMetadata* meta, uint32_t* out_block, uint32_t* out_offset) {
    uint32_t record_size = meta_pool_record_size(meta);

    if (record_size > TAGFS_MPOOL_DATA_SIZE) {
        debug_printf("[MetaPool] write: record too large (%u > %u) for file_id=%u\n",
                     record_size, TAGFS_MPOOL_DATA_SIZE, meta->file_id);
        return -1;
    }

    // spin_lock removed - using g_state.lock

    // Check if we need to chain to a new block
    if (record_size > (uint32_t)(TAGFS_MPOOL_DATA_SIZE - g_current_block.used_bytes)) {
        debug_printf("[MetaPool] CHAINING: block %u full (used=%u need=%u)\n",
                     g_current_block_num, g_current_block.used_bytes, record_size);

        /*
         * ‼ THE BLOCK BEING APPENDED TO IS THE END OF THE CHAIN, AND THIS IS
         * WHERE THAT IS PROVED RATHER THAN ASSUMED.
         *
         * The line below used to be written unconditionally, and it is the
         * line that ate volumes: a walk that stopped early left the current
         * block in the MIDDLE of the chain, and assigning next_block here
         * dropped everything past it — on the medium, permanently, with the
         * blocks still marked used and nothing pointing at them.
         *
         * The walk at mount can no longer stop early, so this cannot be
         * reached any more. It is written down anyway because "cannot happen"
         * is what the old comment believed too, and the cost of finding out
         * otherwise is every file on the volume. A pool that refuses to write
         * loses the newest record; a pool that papers over this loses all the
         * older ones.
         */
        if (g_current_block.next_block != 0) {
            kprintf("[MetaPool] block %u is being appended to and already "
                    "points at block %u — the pool is not writing over that "
                    "link, because everything past it would go off the chain "
                    "for good\n", g_current_block_num, g_current_block.next_block);
            return -1;
        }

        // Room for the link BEFORE the block it describes exists: a chain the
        // pool cannot account for is a chain it cannot later shorten, and
        // failing here costs the volume nothing at all.
        if (chain_reserve(g_chain_len + 1) != 0) {
            kprintf("[MetaPool] there is no memory to lengthen this volume's "
                    "metadata chain past %u blocks\n", g_chain_len);
            return -1;
        }

        // Allocate new block - caller holds g_state.lock
        // Use internal version to avoid double-lock
        uint32_t new_block;
        int alloc_ret = tagfs_alloc_blocks_internal(1, &new_block);
        if (alloc_ret != 0) {
            debug_printf("[MetaPool] CHAINING: failed to allocate new block\n");
            return -1;
        }

        /*
         * ‼ THE NEW BLOCK IS STAMPED BEFORE ANYTHING POINTS AT IT.
         *
         * These two writes used to be the other way round: the predecessor was
         * written carrying next_block = new_block, and only afterwards was the
         * new block given its pool magic — with that second write's error_t
         * thrown away. So the chain pointed at ground holding whatever was
         * there before, and a crash in that window, or a single refused sector
         * on the second write with no crash at all, left a volume that REFUSES
         * TO MOUNT FOR EVER: the mount walk reads that block, does not find
         * TMPL, and says so instead of mounting. There is no repair tool.
         * Chaining happens every forty-odd metadata records — continuously on
         * a working machine.
         *
         * Stamped first, the window holds a volume with one block marked used
         * that nothing points at: a leak the mount's additive repair keeps, and
         * never a chain running into ground the allocator may hand to a file.
         * The comment this replaces already WANTED the magic on the medium
         * before another subsystem could overwrite the block; putting it first
         * serves that better than putting it last.
         *
         * Same two writes, in the other order — no extra I/O.
         */
        memset(&g_fresh_block, 0, sizeof(g_fresh_block));
        g_fresh_block.magic = TAGFS_MPOOL_MAGIC;
        if (tagfs_write_block(new_block, &g_fresh_block) != OK) {
            /* Nothing on the medium changed: the predecessor still ends the
             * chain, and the block goes straight back to the bitmap. */
            tagfs_free_blocks_internal(new_block, 1);
            kprintf("[MetaPool] block %u would not take a metadata pool "
                    "header, so this volume's chain is left as it was and the "
                    "record is not written\n", new_block);
            return -1;
        }

        // Link old block to new block and flush
        debug_printf("[MetaPool] CHAINING: linking block %u -> %u\n", g_current_block_num, new_block);
        g_current_block.next_block = new_block;
        int flush_ret = tagfs_write_block(g_current_block_num, &g_current_block);
        if (flush_ret != OK) {
            /*
             * The medium says it did not take the link, so the block on it
             * still ends the chain and memory is put back to match. Leaving
             * the link standing in memory instead — which is what this used to
             * do — now means the invariant above refuses every later write,
             * and the pool would be shut for the rest of the boot over one
             * refused sector.
             *
             * The new block goes back because nothing reached the medium
             * pointing at it. It carries pool magic now and nothing else does:
             * unlink_empty_block already hands back blocks stamped TMPL, so a
             * stamped block back in the bitmap is ground the allocator may
             * reuse exactly as before. Keeping it would be a block leaked on
             * every attempt, and the mount's additive repair never releases a
             * block it finds marked used, so the leak would be permanent.
             */
            g_current_block.next_block = 0;
            tagfs_free_blocks_internal(new_block, 1);
            kprintf("[MetaPool] block %u would not take the link to block %u, "
                    "so the chain is left as it was and the record is not "
                    "written\n", g_current_block_num, new_block);
            // spin_unlock removed - using g_state.lock
            return -1;
        }

        // Move to the new block
        g_current_block_num = new_block;
        g_current_block     = g_fresh_block;
        g_current_dirty = true;
        chain_append(new_block, 0);   // reserved above; cannot fail here
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

    // A record with no file_id is not a record anybody holds, so it is not
    // counted as kept — that keeps the header's count and what a scan of the
    // payload finds the same number, which is the only reason the header's
    // count is worth carrying at all.
    if (meta->file_id != 0) {
        g_current_block.kept++;
        if (g_chain_len) g_chain[g_chain_len - 1].kept = g_current_block.kept;
    }

    debug_printf("[MetaPool] write: file_id=%u block=%u offset=%u size=%u\n",
                 meta->file_id, *out_block, *out_offset, record_size);

    // Write-through: update mirror (seqlock write)
    if (g_mirror && meta->file_id < g_mirror_capacity) {
        __atomic_fetch_add(&g_mirror_seq, 1, __ATOMIC_RELEASE);  // odd = writing
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
            // Allocation failed: mark entry invalid so stale/partial data isn't returned.
            g_mirror_valid[meta->file_id] = false;
            debug_printf("[MetaPool] mirror: kmalloc failed for file_id=%u — entry invalidated\n",
                         meta->file_id);
        }
        __atomic_fetch_add(&g_mirror_seq, 1, __ATOMIC_RELEASE);  // even = done
    }

    /* Self-Healing keeps a whole 4096-byte BLOCK per mirror slot and sums it
     * over TAGFS_BLOCK_SIZE, so what it must be handed is the block — which is
     * this one, in memory, newer than the medium's copy and therefore the right
     * thing to restore from. It used to be handed the TagFSMetadata, a ~48-byte
     * object with pointers in it, and read 4 KiB past the end of it on every
     * metadata write. */
    TagFS_SelfHealOnMetadataWrite(*out_block, (const uint8_t*)&g_current_block);

    // spin_unlock removed - using g_state.lock
    return 0;
}

// ---------------------------------------------------------------------------
// Public: read
// ---------------------------------------------------------------------------

int meta_pool_read(uint32_t block, uint32_t offset, TagFSMetadata* out) {
    // Current block may have unflushed data — read from in-memory copy
    // spin_lock removed - using g_state.lock
    if (block == g_current_block_num) {
        int r = unpack_record((uint8_t *)&g_current_block + offset, out);
        // spin_unlock removed - using g_state.lock
        return r;
    }
    // spin_unlock removed - using g_state.lock

    // Non-current blocks are always flushed — read from disk
    uint8_t buf[TAGFS_BLOCK_SIZE];
    if (tagfs_read_block(block, buf) != OK) {
        debug_printf("[MetaPool] read: tagfs_read_block failed for block %u\n", block);
        return -1;
    }
    return unpack_record(buf + offset, out);
}

// ---------------------------------------------------------------------------
// Public: delete
// ---------------------------------------------------------------------------

// Drop the mirror's copy of a file whose record is being retired.
static void mirror_forget(uint32_t file_id) {
    if (!g_mirror || file_id >= g_mirror_capacity || !g_mirror_valid[file_id])
        return;
    __atomic_fetch_add(&g_mirror_seq, 1, __ATOMIC_RELEASE);
    tagfs_metadata_free(&g_mirror[file_id]);
    memset(&g_mirror[file_id], 0, sizeof(TagFSMetadata));
    g_mirror_valid[file_id] = false;
    __atomic_fetch_add(&g_mirror_seq, 1, __ATOMIC_RELEASE);
}

/*
 * Take a block that holds nothing any more out of the chain and give it back.
 *
 * ‼ THE ORDER IS THE CRASH SAFETY, and there is nothing else protecting this.
 * The predecessor's next_block is rewritten FIRST, and that write is the
 * commit: before it the block is still on the chain and still marked used, and
 * after it the block is off the chain and the bitmap catches up. Lose power in
 * between and the volume has one block marked used that nothing points at —
 * which the mount's additive repair keeps marked used, so it is a block
 * leaked, never a block handed out over live metadata. The opposite order
 * would free it first, and a crash there leaves the chain running through
 * ground the allocator is free to give away.
 *
 * The predecessor is read into the heap rather than onto the stack because the
 * caller already holds a 4096-byte block buffer of its own, and two of them in
 * one frame is more stack than a kernel path is allowed.
 *
 * The caller holds g_state.lock — tagfs_free_blocks_internal below says so.
 *
 * `says_next` is what the block being removed has on the MEDIUM. The chain in
 * memory says the same thing or the two have come apart, and a chain that has
 * come apart is not one to be rewriting links in.
 *
 * Returns true when the block is off the chain and back in the bitmap.
 */
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
    /*
     * ‼ A RETIRED RECORD LOSES ITS file_id AND KEEPS ITS PLACE.
     *
     * Nothing is ever moved inside a pool block, because a file table entry is
     * a byte offset into one and moving a record would strand every entry
     * behind it. So a delete costs four bytes and the block's kept count falls
     * by one — and when that count reaches zero the block itself is what goes
     * back, which is the only way a pool that is appended to on every write of
     * every file can ever get shorter again.
     */
    /*
     * ‼ THE POOL DOES NOT WRITE INTO A BLOCK IT DOES NOT OWN.
     *
     * Every block a file table entry can name is on this chain, so a block
     * that is not on it means the entry is stale — pointing at a record that
     * was retired, in a block that then emptied, was unlinked, and has since
     * gone back to the allocator. Reading four bytes out of it and writing
     * four zeros back would be zeroing four bytes of whatever file now lives
     * there. It costs nothing to refuse, and the entry it refuses named
     * nothing that was still a file.
     */
    uint32_t index = chain_find(block);
    if (index == g_chain_len) {
        kprintf("[MetaPool] a file's metadata is said to be in block %u and "
                "that block is not on this volume's metadata chain — nothing "
                "is written to it\n", block);
        return -1;
    }

    // spin_lock removed - using g_state.lock
    if (block == g_current_block_num) {
        // Current block lives in memory — modify directly (no disk round-trip)
        uint8_t *rec = (uint8_t *)&g_current_block + offset;
        uint32_t del_file_id;
        memcpy(&del_file_id, rec + 2, sizeof(uint32_t));
        if (del_file_id == 0) {
            // Already retired. Counting it again would make the header's
            // number smaller than what a scan of the payload finds.
            return 0;
        }
        mirror_forget(del_file_id);
        memset(rec + 2, 0, 4);

        /* Counted from the records rather than decremented on faith, the same
         * way the mount walk does it — one rule for the number, in one
         * routine, over a block that is already in memory. */
        g_current_block.kept = count_kept_records(&g_current_block);
        g_chain[index].kept = g_current_block.kept;
        g_current_dirty = true;

        /*
         * The block being appended to is never unlinked, even when it is
         * empty: it is where the next record goes, and a block that is about
         * to be written into is not spare ground. The head is never unlinked
         * either — the Deed names it, so it is the one block of the pool whose
         * place is not the chain's to choose.
         */
        // spin_unlock removed - using g_state.lock
        return 0;
    }
    // spin_unlock removed - using g_state.lock

    /*
     * ‼ A RETIRE IN ANY OTHER BLOCK IS OWED, NOT WRITTEN.
     *
     * This used to read the block, zero the four bytes and write it straight
     * back — and on five of the seven paths that reach here, that write was
     * the only thing between two anchors that touched the medium at all. The
     * replacing record and the entry naming it were both still in memory, so a
     * crash in that window threw the old metadata away and kept nothing new.
     * The debt is paid in meta_pool_flush_retires, after the file table has
     * published the replacement, which is the only order in which losing the
     * old one is safe. See the note over MetaPoolRetire.
     */
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
    /* The medium still shows this record alive while the debt is outstanding,
     * so the test above cannot catch a second retire of the same place — the
     * debt itself is what answers that. */
    if (retire_owed(block, offset)) {
        return 0;
    }

    /* The debt is taken on first, so a refusal here leaves nothing changed
     * anywhere — the record is still whole and the mirror still has it. */
    if (retire_owe(block, offset) != 0) {
        kprintf("[MetaPool] there is no memory to record that block %u owes a "
                "retired record — the record stays as it is\n", block);
        return -1;
    }
    mirror_forget(del_file_id);

    /* The header's count is only ever as good as the records under it, so it
     * is taken from them rather than decremented on faith — the same walk the
     * mount makes, over one block that is already in hand, with the WHOLE debt
     * this block owes applied to it. Two retires out of one block between two
     * syncs would otherwise each be counted against a medium copy that still
     * shows the other one alive. */
    g_chain[index].kept = retire_apply(block, here);
    return 0;
}

/*
 * Pay every retire this pool owes.
 *
 * ‼ CALLED AFTER THE FILE TABLE HAS BEEN FLUSHED, AND THAT IS THE WHOLE ORDER.
 * A retire destroys the only copy of a file's old metadata. Doing it before the
 * table on the medium names the replacement is the one ordering in which a
 * crash loses the file; doing it after is a write nobody would miss if it never
 * happened. So the sequence is: the pool's block (the record), the file table
 * (the name), and only then this (the old record's place).
 *
 * The retires batch, so several out of one block cost one write, and a sync
 * with none owed costs nothing at all. The whole run is under g_state.lock,
 * which is what every other path into this file is under and what
 * tagfs_free_blocks_internal requires — the sync itself holds no lock, and
 * without this a block could be unlinked from under a concurrent write.
 *
 * A block that will not take its write keeps its debt and stops the run; the
 * next sync tries again, exactly as the pool's own block already does.
 */
int meta_pool_flush_retires(void) {
    if (g_retire_len == 0) return 0;

    TagFSState* fs = tagfs_get_state();
    if (!fs) return -1;

    int rc = 0;
    TagFSLockTake(&fs->lock);

    while (g_retire_len > 0) {
        uint32_t block = g_retire[0].block;

        /* Owed against a block that has since left the chain — unlinked by an
         * earlier turn of this loop and handed back. Whatever lives there now
         * is not the pool's to write into. */
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

        /* Index 0 is the head, which the Deed names and the chain may not
         * move; the last entry is the block being appended to. What is left is
         * an interior block, and an interior block with nothing in it is 4080
         * bytes the volume can have back. */
        bool interior = (index > 0) && (index + 1 < g_chain_len);
        if (here->kept == 0 && interior &&
            unlink_empty_block(index, here->next_block)) {
            /* The block is off the chain and back in the bitmap. Writing the
             * retired records into it now would put a metadata pool's magic
             * onto ground the allocator may hand to a file. */
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

    TagFSLockDrop(&fs->lock);
    return rc;
}
