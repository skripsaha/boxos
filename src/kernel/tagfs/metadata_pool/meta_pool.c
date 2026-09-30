#include "meta_pool_internal.h"
#include "../plot/plot.h"
#include "../file_table/file_table.h"

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
 * block numbers in chain order, each with the number of its link. A link keeps
 * its number whatever is later unlinked around it — Glean (step 1.8) takes
 * blocks off the front — and that number is what the map of live files keeps
 * as where a record sits, so a record's place stays true. A number finds its
 * link by halving: the numbers only rise along the chain.
 */
MetaPoolLink* g_pool_chain     = NULL;
uint32_t      g_pool_chain_len = 0;
uint32_t      g_pool_chain_cap = 0;

static TagFSLock* PoolLock(void)
{
    return &tagfs_get_state()->lock;
}

static FileMap* PoolFiles(void)
{
    TagFSState* fs = tagfs_get_state();
    return fs ? fs->files : NULL;
}

static void PoolChainRelease(void)
{
    kfree(g_pool_chain);
    g_pool_chain     = NULL;
    g_pool_chain_len = 0;
    g_pool_chain_cap = 0;
}

int PoolChainRoom(uint32_t want)
{
    for (;;) {
        TagFSLockTake(PoolLock());
        uint32_t cap = g_pool_chain_cap;
        TagFSLockDrop(PoolLock());
        if (want <= cap)
            return 0;

        uint32_t grown_cap = cap ? cap * 2u : 8u;
        while (grown_cap < want)
            grown_cap *= 2u;
        MetaPoolLink* grown = kmalloc(sizeof(MetaPoolLink) * grown_cap);
        if (!grown)
            return -1;

        MetaPoolLink* old = NULL;
        TagFSLockTake(PoolLock());
        if (g_pool_chain_cap == cap) {
            if (g_pool_chain_len)
                memcpy(grown, g_pool_chain, sizeof(MetaPoolLink) * g_pool_chain_len);
            old              = g_pool_chain;
            g_pool_chain     = grown;
            g_pool_chain_cap = grown_cap;
            grown            = NULL;
        }
        TagFSLockDrop(PoolLock());
        kfree(grown);
        kfree(old);
    }
}

void PoolChainAppend(uint32_t block)
{
    uint32_t link = g_pool_chain_len ? g_pool_chain[g_pool_chain_len - 1].link + 1u : 0;
    g_pool_chain[g_pool_chain_len].block = block;
    g_pool_chain[g_pool_chain_len].link  = link;
    g_pool_chain_len++;
}

uint32_t PoolChainAt(uint32_t link)
{
    uint32_t lo = 0, hi = g_pool_chain_len;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (g_pool_chain[mid].link < link)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < g_pool_chain_len && g_pool_chain[lo].link == link ? lo : g_pool_chain_len;
}

bool PoolLinkBlock(uint32_t link, uint32_t* out_block)
{
    uint32_t at = PoolChainAt(link);
    if (at == g_pool_chain_len)
        return false;
    *out_block = g_pool_chain[at].block;
    return true;
}

/*
 * How many records in this block a file still holds — those with a file id.
 * A record an earlier kernel retired kept its length and lost its file id, so
 * the payload is walked by length and counted by identity. This is the number
 * the pool BELIEVES; the one stored in the header is only ever checked
 * against it.
 */
uint16_t PoolCountKept(const MetaPoolBlock* blk)
{
    uint32_t used = blk->used_bytes;
    if (used > TAGFS_MPOOL_DATA_SIZE)
        used = TAGFS_MPOOL_DATA_SIZE;

    uint16_t kept = 0;
    for (uint32_t pos = 0; pos + RECORD_HEADER_SIZE <= used; ) {
        uint16_t record_len;
        uint32_t file_id;
        memcpy(&record_len, blk->payload + pos, sizeof(uint16_t));
        if (record_len < RECORD_HEADER_SIZE || record_len > used - pos)
            break;
        memcpy(&file_id, blk->payload + pos + 2, sizeof(uint32_t));
        if (file_id != 0)
            kept++;
        pos += record_len;
    }
    return kept;
}

// ---------------------------------------------------------------------------
// The mount
// ---------------------------------------------------------------------------

/* One step of the walk: `block` read into `into`, checked and appended to the
 * chain. False — said — for anything that means the volume is not mounted. */
static bool PoolWalkStep(uint32_t block, uint32_t run, MetaPoolBlock* into, uint32_t* disagreed)
{
    if (block >= run) {
        kprintf("[MetaPool] the chain points at block %u and this volume's data run is %u "
                "blocks — that is outside the volume, so it is not mounted\n", block, run);
        return false;
    }
    if (tagfs_read_block(block, into) != OK) {
        kprintf("[MetaPool] block %u would not read — this volume's metadata cannot be "
                "reached, so it is not mounted (%u block(s) of its pool had been read)\n",
                block, g_pool_chain_len);
        return false;
    }
    if (into->magic != TAGFS_MPOOL_MAGIC) {
        kprintf("[MetaPool] block %u holds 0x%08x where a metadata pool should be — this "
                "volume is not mounted, and nothing is written to it\n", block, into->magic);
        return false;
    }
    if (PoolCountKept(into) != into->kept)
        (*disagreed)++;
    if (g_pool_chain_len == g_pool_chain_cap) {
        uint32_t     cap   = g_pool_chain_cap ? g_pool_chain_cap * 2u : 8u;
        MetaPoolLink* grown = kmalloc(sizeof(MetaPoolLink) * cap);
        if (!grown) {
            kprintf("[MetaPool] there is no memory to hold this volume's metadata chain at "
                    "%u blocks — it is not mounted\n", g_pool_chain_len + 1);
            return false;
        }
        if (g_pool_chain_len)
            memcpy(grown, g_pool_chain, sizeof(MetaPoolLink) * g_pool_chain_len);
        kfree(g_pool_chain);
        g_pool_chain     = grown;
        g_pool_chain_cap = cap;
    }
    PoolChainAppend(block);
    return true;
}

int meta_pool_init(uint32_t first_block)
{
    meta_pool_shutdown();

    /*
     * ‼ THE ONLY BOUND ON THIS WALK IS A TERMINATION PROOF, NOT A LENGTH.
     *
     * The data run has `run` blocks in it, so a chain of distinct blocks
     * inside it is no longer than `run`, and a walk that takes `run` hops is
     * going round a cycle. A next_block outside the run is refused before it
     * is read: nothing under tagfs_read_block bounds a block number.
     *
     * ‼ NONE OF THESE IS AN EMPTY POOL. This is the record of every file's
     * name, size and extents; a mount that began it afresh would be a mount
     * that decided the volume is empty, and the first write after it would
     * allocate over what is still there. Every refusal is said with kprintf and
     * the volume is not mounted.
     */
    TagFSState* fs  = tagfs_get_state();
    uint32_t    run = fs ? fs->layout.data_blocks : 0;
    if (run == 0) {
        kprintf("[MetaPool] this volume states a data run of no blocks at all, so there is "
                "nowhere for a metadata pool to be — it is not mounted\n");
        return -1;
    }

    /* The walk reads into the buffer that becomes the tail: every block but
     * the last is read over by the next. */
    MetaPoolBlock* here = kmalloc(sizeof(MetaPoolBlock));
    if (!here) {
        kprintf("[MetaPool] there is no memory for the block this volume's metadata chain "
                "is walked through — it is not mounted\n");
        return -1;
    }

    uint32_t block     = first_block;
    uint32_t disagreed = 0;
    for (uint32_t hops = 0; ; hops++) {
        if (!PoolWalkStep(block, run, here, &disagreed)) {
            kfree(here);
            PoolChainRelease();
            return -1;
        }
        if (here->next_block == 0)
            break;
        if (hops + 1 >= run) {
            kprintf("[MetaPool] this volume's metadata chain has taken %u hops in a data run "
                    "of %u blocks, so it is going round in a circle — it is not mounted\n",
                    hops + 1, run);
            kfree(here);
            PoolChainRelease();
            return -1;
        }
        block = here->next_block;
    }

    /*
     * ‼ SAID, AND NOT WRITTEN BACK. The stored count is a convenience and the
     * scan is the truth; the next write of each block carries the right number
     * out by itself. A volume made before the count existed carries zero in
     * every block and says this once.
     */
    if (disagreed)
        kprintf("[MetaPool] %u of this volume's %u metadata block(s) state a kept-record "
                "count the records themselves do not bear out — the records are counted, "
                "and that is the number used\n", disagreed, g_pool_chain_len);

    here->kept            = PoolCountKept(here);
    g_pool_tail           = here;
    g_pool_tail_block     = block;
    g_pool_tail_link      = g_pool_chain[g_pool_chain_len - 1].link;
    g_pool_tail_frozen    = here->used_bytes;
    g_pool_tail_gen       = 0;
    g_pool_tail_changed   = false;
    return 0;
}

void meta_pool_shutdown(void)
{
    PoolTailShutdown();
    PoolChainRelease();
}

void tagfs_metadata_free(TagFSMetadata* meta)
{
    if (meta->filename) { kfree(meta->filename); meta->filename = NULL; }
    if (meta->tag_ids)  { kfree(meta->tag_ids);  meta->tag_ids  = NULL; }
    if (meta->extents)  { kfree(meta->extents);  meta->extents  = NULL; }
    meta->tag_count    = 0;
    meta->extent_count = 0;
}

// ---------------------------------------------------------------------------
// The mirror
// ---------------------------------------------------------------------------

error_t PoolMirrorPut(uint32_t fid, FileRecord* rec, const RecordWhere* where,
                      FileMapStake* stake)
{
    FileMap* files = PoolFiles();
    if (!rec || !(rec->flags & TAGFS_FILE_ACTIVE)) {
        FileRecordRelease(rec);
        FileMapDrop(files, fid);
        file_table_delete(fid);
        return OK;
    }
    return FileMapRecordPut(files, fid, rec, where, stake);
}

/* Every record of one block that a live file's row names, put into the map
 * through the mirror's door. The walk is in append order, so the last record
 * of a file it puts is the newest one on the medium. False — said — when a
 * record could not be held. */
static bool PoolMirrorBlock(const MetaPoolBlock* hdr, uint32_t link, uint32_t* loaded,
                            uint32_t* unreadable)
{
    FileMap* files = PoolFiles();
    uint32_t used  = hdr->used_bytes <= TAGFS_MPOOL_DATA_SIZE ? hdr->used_bytes
                                                              : TAGFS_MPOOL_DATA_SIZE;
    for (uint32_t pos = 0; pos + RECORD_HEADER_SIZE <= used; ) {
        uint16_t record_len;
        uint32_t file_id, row_block, row_offset;
        memcpy(&record_len, hdr->payload + pos, sizeof(uint16_t));
        if (record_len < RECORD_HEADER_SIZE || record_len > used - pos)
            break;
        memcpy(&file_id, hdr->payload + pos + 2, sizeof(uint32_t));

        if (file_id > 0 && FileMapRowGet(files, file_id, &row_block, &row_offset)) {
            TagFSMetadata meta;
            memset(&meta, 0, sizeof(meta));
            error_t unpacked = UnpackV1(hdr->payload + pos, record_len, &meta);
            if (unpacked == ERR_NO_MEMORY) {
                kprintf("[MetaPool] there is no memory to unpack file %u's record at mount — "
                        "this volume is not mounted\n", file_id);
                return false;
            }
            if (unpacked != OK || meta.file_id != file_id) {
                (*unreadable)++;
            } else {
                FileRecord*  rec   = (meta.flags & TAGFS_FILE_ACTIVE) ? FileRecordOf(&meta) : NULL;
                FileMapStake stake = { .count = 0 };
                RecordWhere  where = { .link = link, .offset = MPOOL_BLOCK_HEADER + pos,
                                       .gen = 0, .shape = 0 };
                bool held = !(meta.flags & TAGFS_FILE_ACTIVE) || rec;
                if (!held || PoolMirrorPut(file_id, rec, &where, &stake) != OK) {
                    kprintf("[MetaPool] there is no memory to hold file %u's record — the map "
                            "of live files is what every read and write of this volume stands "
                            "on, so it is not mounted (%u record(s) held)\n", file_id, *loaded);
                    tagfs_metadata_free(&meta);
                    return false;
                }
                (*loaded)++;
            }
            tagfs_metadata_free(&meta);
        }
        pos += record_len;
    }
    return true;
}

int meta_pool_mirror_init(void)
{
    uint8_t* buf = kmalloc(TAGFS_BLOCK_SIZE);
    if (!PoolFiles() || !buf) {
        kfree(buf);
        kprintf("[MetaPool] there is no memory for the one block the mount reads this "
                "volume's metadata through — it is not mounted\n");
        return -1;
    }

    uint32_t loaded = 0, unreadable = 0;
    for (uint32_t at = 0; at < g_pool_chain_len; at++) {
        const MetaPoolBlock* hdr = (const MetaPoolBlock*)buf;
        if (g_pool_chain[at].link == g_pool_tail_link) {
            hdr = g_pool_tail;
        } else if (tagfs_read_block(g_pool_chain[at].block, buf) != OK) {
            kprintf("[MetaPool] block %u of this volume's metadata read at mount and will not "
                    "read now — it is not mounted\n", g_pool_chain[at].block);
            kfree(buf);
            return -1;
        }
        if (hdr->magic != TAGFS_MPOOL_MAGIC ||
            !PoolMirrorBlock(hdr, g_pool_chain[at].link, &loaded, &unreadable)) {
            kfree(buf);
            return -1;
        }
    }
    kfree(buf);

    if (unreadable)
        kprintf("[MetaPool] %u record(s) of this volume's pool do not check out — each file "
                "they belong to keeps the newest of its records that does\n", unreadable);
    return 0;
}

// ---------------------------------------------------------------------------
// Where a record sits
// ---------------------------------------------------------------------------

bool RecordWhereValid(const RecordWhere* where, uint32_t fid)
{
    if (PoolChainAt(where->link) == g_pool_chain_len)
        return false;
    if (where->offset < MPOOL_BLOCK_HEADER ||
        where->offset + RECORD_HEADER_SIZE > TAGFS_BLOCK_SIZE)
        return false;

    const MetaPoolBlock* img = PoolImageOf(where->link);
    if (!img)
        return true;        /* the medium's alone: its bounds are all memory can check */

    uint32_t want = where->offset - MPOOL_BLOCK_HEADER;
    uint32_t used = img->used_bytes;
    for (uint32_t pos = 0; pos + RECORD_HEADER_SIZE <= used; ) {
        uint16_t record_len;
        memcpy(&record_len, img->payload + pos, sizeof(uint16_t));
        if (record_len < RECORD_HEADER_SIZE || record_len > used - pos)
            return false;
        if (pos == want) {
            uint32_t file_id;
            memcpy(&file_id, img->payload + pos + 2, sizeof(uint32_t));
            return file_id == fid;
        }
        if (pos > want)
            return false;
        pos += record_len;
    }
    return false;
}
