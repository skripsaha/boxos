#include "meta_pool.h"
#include "../tagfs.h"
#include "../plot/plot.h"
#include "../file_map/file_map.h"

// Forward declarations - internal functions in tagfs.c. Both are meant for a
// caller holding g_state.lock. A write's, a span's and a defrag's commit
// (PlotCommit) still reaches meta_pool_write without it — W-LOCK, which Ink
// closes at step 1.4.
extern int     tagfs_alloc_blocks_internal(uint32_t count, uint32_t *out_start_block);
extern error_t tagfs_free_blocks_internal(uint32_t start_block, uint32_t count);

/* A record's own layout — its fields, its widths, its sum — is Plot's
 * (plot/plot_width.h, PackV1). The pool frames records: a 16-byte block
 * header, then records packed end to end, each opening with its u16 length
 * and u32 file id. */
#define MPOOL_BLOCK_HEADER  16

static MetaPoolBlock g_current_block;
static uint32_t     g_current_block_num = 0;
static bool         g_current_dirty    = false;

/* The empty header a block is stamped with when the chain lengthens. BSS and
 * not a local: a MetaPoolBlock is exactly 4 KiB, meta_pool_write is reached
 * from the K-Core continuation that finishes an async write, and a 4 KiB frame
 * there competes with nested IRQ frames on a per-cpu stack. Every path into
 * this file holds g_state.lock, so one shared buffer has one user at a time. */
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
 * of the medium per delete.
 *
 * ‼ AND EVERY LINK KEEPS ITS NUMBER. `link` counts from the head as the chain
 * was walked at mount, and a link appended later takes the next number; one
 * unlinked out of the middle takes its number with it and nobody else's
 * moves. That number is what the map of live files keeps as where a record
 * sits, so a record's place stays true whatever is unlinked — or gleaned off
 * the front — around it. A number finds its link by halving (the numbers only
 * rise along the chain), and a block finds its number through a hash that
 * grows with the chain: the pool is appended to without end, and a walk of
 * the whole chain on every metadata update is a cost that grows with it.
 */
typedef struct {
    uint32_t block;
    uint32_t link;
    uint16_t kept;
} MetaPoolLink;

static MetaPoolLink* g_chain     = NULL;
static uint32_t      g_chain_len = 0;
static uint32_t      g_chain_cap = 0;

/* Block -> link number, open addressed: a slot is 0 when empty and
 * block + 1 otherwise, and a table is never more than half full. */
typedef struct {
    uint32_t block1;
    uint32_t link;
} MetaPoolSlot;

static MetaPoolSlot* g_hash      = NULL;
static uint32_t      g_hash_cap  = 0;     /* a power of two */

static uint32_t hash_home(uint32_t block, uint32_t cap) {
    return (block * 0x9E3779B1u) & (cap - 1u);
}

static void hash_put_into(MetaPoolSlot* table, uint32_t cap, uint32_t block, uint32_t link) {
    uint32_t at = hash_home(block, cap);
    while (table[at].block1 != 0 && table[at].block1 != block + 1u)
        at = (at + 1u) & (cap - 1u);
    table[at].block1 = block + 1u;
    table[at].link   = link;
}

static bool hash_get(uint32_t block, uint32_t* out_link) {
    if (!g_hash_cap) return false;
    for (uint32_t at = hash_home(block, g_hash_cap);; at = (at + 1u) & (g_hash_cap - 1u)) {
        if (g_hash[at].block1 == 0) return false;
        if (g_hash[at].block1 == block + 1u) {
            *out_link = g_hash[at].link;
            return true;
        }
    }
}

/* Out of the table, and every slot after it in its run moved back to where a
 * search from its home still finds it. */
static void hash_del(uint32_t block) {
    if (!g_hash_cap) return;
    uint32_t mask = g_hash_cap - 1u;
    uint32_t at   = hash_home(block, g_hash_cap);
    while (g_hash[at].block1 != block + 1u) {
        if (g_hash[at].block1 == 0) return;
        at = (at + 1u) & mask;
    }
    for (uint32_t next = (at + 1u) & mask; g_hash[next].block1 != 0; next = (next + 1u) & mask) {
        uint32_t home = hash_home(g_hash[next].block1 - 1u, g_hash_cap);
        if (((next - home) & mask) >= ((next - at) & mask)) {
            g_hash[at] = g_hash[next];
            at = next;
        }
    }
    g_hash[at].block1 = 0;
}

static void chain_release(void) {
    if (g_chain) kfree(g_chain);
    if (g_hash) kfree(g_hash);
    g_chain     = NULL;
    g_chain_len = 0;
    g_chain_cap = 0;
    g_hash      = NULL;
    g_hash_cap  = 0;
}

// Make room for `want` links, in the chain and in its hash — the hash rebuilt
// into a table twice the size, then put in place. Called before the disk block
// the new link will describe is allocated, so running out of memory costs the
// volume nothing.
static int chain_reserve(uint32_t want) {
    if (want > g_chain_cap) {
        uint32_t cap = g_chain_cap ? g_chain_cap * 2 : 8;
        if (cap < want) cap = want;

        MetaPoolLink* grown = kmalloc(sizeof(MetaPoolLink) * cap);
        if (!grown) return -1;
        if (g_chain && g_chain_len)
            memcpy(grown, g_chain, sizeof(MetaPoolLink) * g_chain_len);
        if (g_chain) kfree(g_chain);
        g_chain     = grown;
        g_chain_cap = cap;
    }

    if (want * 2u > g_hash_cap) {
        uint32_t cap = g_hash_cap ? g_hash_cap : 16u;
        while (want * 2u > cap) cap *= 2u;
        MetaPoolSlot* table = kmalloc(sizeof(MetaPoolSlot) * cap);
        if (!table) return -1;
        memset(table, 0, sizeof(MetaPoolSlot) * cap);
        for (uint32_t i = 0; i < g_chain_len; i++)
            hash_put_into(table, cap, g_chain[i].block, g_chain[i].link);
        if (g_hash) kfree(g_hash);
        g_hash     = table;
        g_hash_cap = cap;
    }
    return 0;
}

static int chain_append(uint32_t block, uint16_t kept) {
    if (chain_reserve(g_chain_len + 1) != 0) return -1;
    uint32_t link = g_chain_len ? g_chain[g_chain_len - 1].link + 1u : 0;
    g_chain[g_chain_len].block = block;
    g_chain[g_chain_len].link  = link;
    g_chain[g_chain_len].kept  = kept;
    g_chain_len++;
    hash_put_into(g_hash, g_hash_cap, block, link);
    return 0;
}

// Where in the chain the link numbered `link` sits, or g_chain_len when it has
// been unlinked.
static uint32_t chain_at_link(uint32_t link) {
    uint32_t lo = 0, hi = g_chain_len;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (g_chain[mid].link < link) lo = mid + 1;
        else hi = mid;
    }
    return lo < g_chain_len && g_chain[lo].link == link ? lo : g_chain_len;
}

// Where in the chain this block sits, or g_chain_len when it is not in it at
// all — which is a file table pointing somewhere the pool does not reach.
static uint32_t chain_find(uint32_t block) {
    uint32_t link;
    return hash_get(block, &link) ? chain_at_link(link) : g_chain_len;
}

static void chain_drop(uint32_t index) {
    if (index >= g_chain_len) return;
    hash_del(g_chain[index].block);
    memmove(&g_chain[index], &g_chain[index + 1],
            sizeof(MetaPoolLink) * (g_chain_len - index - 1));
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

/*
 * ‼ THE MIRROR IS THE MAP OF LIVE FILES (file_map/), AND SO IS WHERE.
 *
 * Every record this pool writes goes into the map as the file's record, with
 * the place it sits — its link's number and its byte offset — and the mount's
 * walk puts there the newest record it finds for every file the table names.
 * The walk reads the chain in append order, so the last record it sees for a
 * file is by construction the newest one on the medium: the pool is the
 * authority on where a file's metadata is, and a file table that names
 * somewhere else is one that missed a flush, which is what the mount fsck
 * corrects it to.
 */
static FileMap* pool_files(void) {
    TagFSState* fs = tagfs_get_state();
    return fs ? fs->files : NULL;
}

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
// Public: the mirror
// ---------------------------------------------------------------------------

/*
 * The newest record of every file the table names, into the map of live
 * files, from the chain the init walk proved rather than a second walk of
 * next_block pointers — that walk refused the mount for anything it could not
 * read or did not recognise. A record of a file the table does not name is
 * passed over: no live file is made of it, and the map holds nothing for it.
 * One block buffer for the walk, the heap's.
 */
int meta_pool_mirror_init(void) {
    FileMap* files = pool_files();
    uint8_t* buf   = kmalloc(TAGFS_BLOCK_SIZE);
    if (!files || !buf) {
        kfree(buf);
        kprintf("[MetaPool] there is no memory for the one block the mount reads this "
                "volume's metadata through\n");
        return -1;
    }

    uint32_t loaded = 0, refused = 0;
    for (uint32_t at = 0; at < g_chain_len; at++) {
        uint32_t block_num = g_chain[at].block;

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

            uint32_t file_id, row_block, row_offset;
            memcpy(&file_id, hdr->payload + pos + 2, sizeof(uint32_t));

            TagFSMetadata meta;
            memset(&meta, 0, sizeof(meta));
            /* record_len is bounded by what the block holds past pos, above. */
            if (file_id > 0 && FileMapRowGet(files, file_id, &row_block, &row_offset) &&
                UnpackV1(hdr->payload + pos, record_len, &meta) == OK) {
                /* Last one wins, and the walk is in append order, so this
                 * ends up holding the NEWEST record for the file and the
                 * place it sits — which is what the mount fsck corrects a
                 * file table against. The file is in the map by its row, so
                 * the put takes no node. */
                FileRecord*  rec   = FileRecordOf(&meta);
                FileMapStake stake = { .count = 0 };
                if (rec && FileMapRecordPut(files, file_id, rec, g_chain[at].link,
                                            MPOOL_BLOCK_HEADER + pos, &stake) == OK)
                    loaded++;
                else
                    refused++;
                tagfs_metadata_free(&meta);
            }
            pos += record_len;
        }
    }
    kfree(buf);

    if (refused)
        kprintf("[MetaPool] %u record(s) of this volume's files could not be held in memory "
                "at mount — those files are read from the medium instead\n", refused);
    debug_printf("[MetaPool] Mirror loaded: %u entries\n", loaded);
    return 0;
}

/* Where the pool holds this file's newest record, for a caller that has to
 * decide whether a file table entry still names it. Answers -1 when the pool
 * has nothing for this file, which is a different sentence from "the entry is
 * wrong" and is left to the caller to judge. */
int meta_pool_mirror_where(uint32_t file_id, uint32_t* out_block, uint32_t* out_offset) {
    FileMap*    files = pool_files();
    uint32_t    link = 0, offset = 0;
    FileRecord* rec  = files ? FileMapRecordHold(files, file_id, &link, &offset) : NULL;
    if (!rec)
        return -1;
    FileRecordRelease(rec);
    uint32_t at = chain_at_link(link);
    if (at == g_chain_len)
        return -1;
    *out_block  = g_chain[at].block;
    *out_offset = offset;
    return 0;
}

/* The file's record out of the map, a copy the caller frees. A reader holds a
 * reference while it copies, so a record replaced meanwhile is freed after the
 * copy and never under it. */
int meta_pool_read_cached(uint32_t file_id, TagFSMetadata* out) {
    FileMap*    files = pool_files();
    FileRecord* rec   = files ? FileMapRecordHold(files, file_id, NULL, NULL) : NULL;
    if (!rec)
        return -1;
    int rc = FileRecordCopyOut(rec, out);
    FileRecordRelease(rec);
    return rc;
}

// ---------------------------------------------------------------------------
// Public: write
// ---------------------------------------------------------------------------

static int pool_append(const TagFSMetadata* meta, uint32_t record_size,
                       uint32_t* out_block, uint32_t* out_offset);

/*
 * The record appended, and made the file's record in the map of live files.
 *
 * ‼ THE MAP'S ROOM IS TAKEN BEFORE THE POOL IS TOUCHED. The copy the map keeps
 * and the nodes an insert of a new file may split are allocated first, so a
 * heap that refuses them refuses the write whole — nothing appended, nothing
 * named — and the append that follows cannot leave the map behind the pool.
 */
int meta_pool_write(const TagFSMetadata* meta, uint32_t* out_block, uint32_t* out_offset) {
    uint32_t record_size = 0;
    if (!PlotFits(meta, &record_size)) {
        kprintf("[MetaPool] file %u: its record would be wider than a record of this volume "
                "holds (%u bytes) — it is not written\n", meta->file_id, TAGFS_MPOOL_DATA_SIZE);
        return -1;
    }

    FileMap*     files = meta->file_id ? pool_files() : NULL;
    FileRecord*  rec   = NULL;
    FileMapStake stake = { .count = 0 };
    if (files) {
        rec = FileRecordOf(meta);
        if (!rec) {
            kprintf("[MetaPool] there is no memory for memory's copy of file %u's record "
                    "(%u bytes) — the record is not written\n", meta->file_id, record_size);
            return -1;
        }
        if (FileMapStakeFor(files, meta->file_id, &stake) != OK) {
            FileRecordRelease(rec);
            return -1;
        }
    }

    int rc = pool_append(meta, record_size, out_block, out_offset);
    if (rc == 0 && rec)
        (void)FileMapRecordPut(files, meta->file_id, rec, g_chain[g_chain_len - 1].link,
                               *out_offset, &stake);
    else
        FileRecordRelease(rec);
    FileMapStakeLetGo(&stake);
    return rc;
}

/* The record packed onto the end of the chain, chaining a new block when the
 * last one has no room. */
static int pool_append(const TagFSMetadata* meta, uint32_t record_size,
                       uint32_t* out_block, uint32_t* out_offset) {
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
    if (PackV1(meta, dest) == 0)
        return -1;

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
    /* A record starts past the block's header and leaves room for its own. */
    if (offset < MPOOL_BLOCK_HEADER || offset + RECORD_HEADER_SIZE > TAGFS_BLOCK_SIZE)
        return -(int)ERR_METADATA_CORRUPTED;

    // Current block may have unflushed data — read from in-memory copy
    if (block == g_current_block_num)
        return -(int)UnpackV1((uint8_t *)&g_current_block + offset, TAGFS_BLOCK_SIZE - offset, out);

    // Non-current blocks are always flushed — read from disk
    uint8_t buf[TAGFS_BLOCK_SIZE];
    if (tagfs_read_block(block, buf) != OK) {
        debug_printf("[MetaPool] read: tagfs_read_block failed for block %u\n", block);
        return -(int)ERR_READ_FAILED;
    }
    return -(int)UnpackV1(buf + offset, TAGFS_BLOCK_SIZE - offset, out);
}

// ---------------------------------------------------------------------------
// Public: delete
// ---------------------------------------------------------------------------

/* The record at byte `offset` of link `index` is retired: the map lets the
 * file's record go only when that is where it sits. A retire that follows the
 * write of a newer record — every update does it in that order — used to drop
 * the newer one from memory too, and every read of the file went back to the
 * medium until the next mount. */
static void mirror_forget(uint32_t file_id, uint32_t index, uint32_t offset) {
    FileMap* files = pool_files();
    if (files)
        FileMapRecordForgetAt(files, file_id, g_chain[index].link, offset);
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
        mirror_forget(del_file_id, index, offset);
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
    mirror_forget(del_file_id, index, offset);

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
