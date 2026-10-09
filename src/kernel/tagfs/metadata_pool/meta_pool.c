#include "meta_pool_internal.h"
#include "../plot/plot.h"
#include "../file_table/file_table.h"
#include "../mount/ground_census.h"
#include "../glean/glean.h"
#include "../../../lib/kernel/crypto.h"

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
 * its number whatever is later unlinked around it — Glean (glean/) takes
 * blocks off the front — and that number is what the map of live files keeps
 * as where a record sits, so a record's place stays true. A number finds its
 * link by halving: the numbers only rise along the chain.
 */
MetaPoolLink* g_pool_chain       = NULL;
uint32_t      g_pool_chain_len   = 0;
uint32_t      g_pool_chain_cap   = 0;
uint32_t      g_pool_closed_fold = 0;
uint64_t      g_pool_live_bytes  = 0;

/* PoolDiedBytes: only ever grows. */
static uint64_t s_died_bytes;

uint32_t PoolBlockMix(uint32_t block, const MetaPoolBlock* blk)
{
    uint32_t used = blk->used_bytes <= TAGFS_MPOOL_DATA_SIZE ? blk->used_bytes
                                                             : TAGFS_MPOOL_DATA_SIZE;
    uint32_t mix[3] = { block, used, KCrc32(blk->payload, used) };
    return KCrc32((const uint8_t*)mix, sizeof(mix));
}

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
    g_pool_chain       = NULL;
    g_pool_chain_len   = 0;
    g_pool_chain_cap   = 0;
    g_pool_closed_fold = 0;
    g_pool_live_bytes  = 0;
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
    g_pool_chain[g_pool_chain_len].block       = block;
    g_pool_chain[g_pool_chain_len].link        = link;
    g_pool_chain[g_pool_chain_len].mix         = 0;
    g_pool_chain[g_pool_chain_len].live        = 0;
    g_pool_chain[g_pool_chain_len].replaced_by = link;
    g_pool_chain_len++;
}

void PoolLiveMove(const RecordWhere* from, const RecordWhere* to)
{
    if (from && from->len) {
        uint32_t at = PoolChainAt(from->link);
        if (at < g_pool_chain_len) {
            uint32_t took = from->len <= g_pool_chain[at].live ? from->len : g_pool_chain[at].live;
            g_pool_chain[at].live -= took;
            g_pool_live_bytes     -= took;
        }
    }
    if (to && to->len) {
        uint32_t at = PoolChainAt(to->link);
        if (at < g_pool_chain_len) {
            g_pool_chain[at].live += to->len;
            g_pool_live_bytes     += to->len;
        }
    }
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
// The volume let go, and the ground it holds
// ---------------------------------------------------------------------------

void meta_pool_shutdown(void)
{
    PoolTailShutdown();
    PoolChainRelease();
    GleanUnmount();
}

void PoolClaimGround(uint32_t deed_head)
{
    bool head_linked = false;
    for (uint32_t i = 0; i < g_pool_chain_len; i++) {
        GroundClaim(GROUND_POOL, g_pool_chain[i].block, 1, 0);
        head_linked |= g_pool_chain[i].block == deed_head;
    }
    if (!head_linked)
        GroundClaim(GROUND_POOL, deed_head, 1, 0);
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

/* The record at `was` — none when its length is 0 — replaced by one laid in
 * link `by`: its link leaves the chain only under a seal that counts `by`
 * closed (glean.h). */
static void PoolReplaced(const RecordWhere* was, uint32_t by)
{
    if (!was->len)
        return;
    uint32_t at = PoolChainAt(was->link);
    if (at < g_pool_chain_len && g_pool_chain[at].replaced_by < by)
        g_pool_chain[at].replaced_by = by;
}

/* The file out of the map, and its record's bytes out of its link's count;
 * `by`, when a Quitclaim took its place, is where that lies, NULL when nothing
 * did. */
static void PoolMirrorLetGo(uint64_t fid, const RecordWhere* by)
{
    RecordWhere was;
    FileMapDrop(PoolFiles(), fid, &was);
    PoolLiveMove(&was, NULL);
    if (by)
        PoolReplaced(&was, by->link);
    s_died_bytes += was.len;
}

error_t PoolMirrorPut(uint64_t fid, FileRecord* rec, const RecordWhere* where,
                      FileMapStake* stake)
{
    if (!rec || !(rec->flags & TAGFS_FILE_ACTIVE)) {
        FileRecordRelease(rec);
        PoolMirrorLetGo(fid, where);
        file_table_delete(fid);
        return OK;
    }
    RecordWhere was;
    error_t     rc = FileMapRecordPut(PoolFiles(), fid, rec, where, stake, &was);
    if (rc == OK) {
        PoolLiveMove(&was, where);
        PoolReplaced(&was, where->link);
        s_died_bytes += was.len;
    }
    return rc;
}

void PoolMirrorDrop(uint64_t fid)
{
    PoolMirrorLetGo(fid, NULL);
}

bool PoolMirrorMove(uint64_t fid, const RecordWhere* from, const RecordWhere* to)
{
    uint32_t block = 0;
    if (!PoolLinkBlock(to->link, &block) ||
        !FileMapRecordRelink(PoolFiles(), fid, from, to, block))
        return false;
    PoolLiveMove(from, to);
    PoolReplaced(from, to->link);
    return true;
}

// ---------------------------------------------------------------------------
// Glean's view of the chain
// ---------------------------------------------------------------------------

uint32_t PoolChainBlocks(void)
{
    return g_pool_chain_len;
}

uint32_t PoolLiveBlocks(void)
{
    return (uint32_t)((g_pool_live_bytes + TAGFS_MPOOL_DATA_SIZE - 1u) / TAGFS_MPOOL_DATA_SIZE);
}

uint64_t PoolDiedBytes(void)
{
    return s_died_bytes;
}

uint32_t PoolChainFirst(void)
{
    return g_pool_chain_len ? g_pool_chain[0].block : 0;
}

bool PoolChainHolds(uint32_t block)
{
    for (uint32_t i = 0; i < g_pool_chain_len; i++)
        if (g_pool_chain[i].block == block)
            return true;
    return false;
}

bool PoolFrontLink(uint32_t i, PoolFront* out)
{
    if (!PoolChainLink(i, out))
        return false;
    return !PoolImageOf(out->link);     /* closed, but no cycle has written it yet */
}

bool PoolChainLink(uint32_t i, PoolFront* out)
{
    if (i + 1u >= g_pool_chain_len)
        return false;       /* past the closed links: the tail is never gleaned */
    const MetaPoolLink* l = &g_pool_chain[i];
    out->block       = l->block;
    out->link        = l->link;
    out->live        = l->live;
    out->next        = g_pool_chain[i + 1u].block;
    out->mix         = l->mix;
    out->replaced_by = l->replaced_by;
    return true;
}

uint32_t PoolTailLink(void)
{
    return g_pool_tail_link;
}

bool PoolFrontReadsTrue(const PoolFront* front, const MetaPoolBlock* img)
{
    return img->magic == TAGFS_MPOOL_MAGIC && PoolBlockMix(front->block, img) == front->mix;
}

void PoolUnlinkFront(uint32_t count)
{
    if (count >= g_pool_chain_len)
        return;
    for (uint32_t i = 0; i < count; i++) {
        g_pool_closed_fold ^= g_pool_chain[i].mix;
        g_pool_live_bytes  -= g_pool_chain[i].live;
    }
    memmove(g_pool_chain, g_pool_chain + count,
            sizeof(MetaPoolLink) * (g_pool_chain_len - count));
    g_pool_chain_len -= count;
}

// ---------------------------------------------------------------------------
// Where a record sits
// ---------------------------------------------------------------------------

bool RecordWhereValid(const RecordWhere* where, uint64_t fid)
{
    if (PoolChainAt(where->link) == g_pool_chain_len)
        return false;
    if (where->offset < MPOOL_BLOCK_HEADER ||
        (uint32_t)where->offset + RECORD_HEADER_SIZE > TAGFS_BLOCK_SIZE)
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
