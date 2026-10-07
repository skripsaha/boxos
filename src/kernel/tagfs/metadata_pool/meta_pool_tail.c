#include "meta_pool_internal.h"
#include "../scribe/scribe.h"
#include "../glean/glean.h"
#include "../../../lib/kernel/crypto.h"

/*
 * The tail, the closed blocks and the Spares.
 *
 * ‼ NOTHING THE MEDIUM HAS SETTLED IS WRITTEN OVER. A record once in a cycle's
 * snapshot — `frozen` — is never replaced in memory, and a closed block is
 * never written again once a cycle has settled it. What is still rewritten in
 * place on format v1 is the open tail, a cycle at a time; a cycle with a
 * waiter closes it first (PoolSnapTake), so what an anchor settled lies in a
 * block no later write touches — and a cycle that finds no Spare it may close
 * onto says that its waiter's records stay in the open tail.
 *
 * ‼ A CLOSED BLOCK'S LINK IS WRITTEN AFTER THE FLUSH THAT KEPT THE STAMP IT
 * NAMES. The Spare was stamped when it was made, and its need rides with the
 * closed block; the cycle's first flush covers it before the block goes out.
 * Until the cycle that writes the link, the map leaves the Spare's block free
 * on the medium (PoolUnnamedLeftOut).
 */
MetaPoolBlock* g_pool_tail         = NULL;
uint32_t       g_pool_tail_block   = 0;
uint32_t       g_pool_tail_link    = 0;
uint32_t       g_pool_tail_frozen  = 0;
uint64_t       g_pool_tail_gen     = 0;
bool           g_pool_tail_changed = false;

static uint64_t      s_tail_edits;          /* +1 whenever what the tail holds changes */
static MetaPoolPage* s_closed_head;          /* closed, not yet settled, oldest first */
static MetaPoolPage* s_closed_tail;
static MetaPoolPage* s_spares;
static uint32_t      s_spare_count;
static uint64_t      s_closed_settled_gen;
static uint64_t      s_closed_gen_max;       /* the newest record ever closed this mount */

/* The oldest closed block turned since the last S0: from it on, every link
 * names a block no cycle has yet taken a snapshot to name. NULL: none. */
static MetaPoolPage* s_unnamed_from;

/* Said once for each run of cycles that owed a close and could not make one. */
static bool          s_unclosed_said;

/* The tail as the Settle's holder writes it: copied at S0 under the state
 * lock, written at S3 with no lock. The holder's alone — one cycle at a time. */
static MetaPoolBlock s_settle_tail;

static TagFSLock* TailLock(void)
{
    return &tagfs_get_state()->lock;
}

static FileMap* TailFiles(void)
{
    return tagfs_get_state()->files;
}

const MetaPoolBlock* PoolImageOf(uint32_t link)
{
    if (g_pool_tail && link == g_pool_tail_link)
        return g_pool_tail;
    for (MetaPoolPage* p = s_closed_head; p; p = p->next)
        if (p->link == link)
            return p->image;
    return NULL;
}

// ---------------------------------------------------------------------------
// The tail
// ---------------------------------------------------------------------------

uint32_t PoolTailRoom(void)
{
    return g_pool_tail ? TAGFS_MPOOL_DATA_SIZE - g_pool_tail->used_bytes : 0;
}

bool PoolTailHolds(const RecordWhere* where, uint32_t fid, uint32_t* out_len)
{
    if (!g_pool_tail || where->link != g_pool_tail_link ||
        where->offset < MPOOL_BLOCK_HEADER + g_pool_tail_frozen ||
        !RecordWhereValid(where, fid))
        return false;
    uint16_t record_len;
    memcpy(&record_len, g_pool_tail->payload + (where->offset - MPOOL_BLOCK_HEADER),
           sizeof(uint16_t));
    *out_len = record_len;
    return true;
}

void PoolTakeBack(const RecordWhere* where, uint32_t len)
{
    uint8_t* payload = g_pool_tail->payload;
    uint32_t at      = where->offset - MPOOL_BLOCK_HEADER;
    uint32_t used    = g_pool_tail->used_bytes;

    memmove(payload + at, payload + at + len, used - at - len);
    memset(payload + used - len, 0, len);
    g_pool_tail->used_bytes   = (uint16_t)(used - len);
    g_pool_tail->record_count--;
    g_pool_tail_changed = true;
    s_tail_edits++;

    /* Every record after it moved down by `len`: each file whose newest
     * record it is is told where it sits now. */
    for (uint32_t pos = at; pos + RECORD_HEADER_SIZE <= g_pool_tail->used_bytes; ) {
        uint16_t record_len;
        uint32_t file_id;
        memcpy(&record_len, payload + pos, sizeof(uint16_t));
        if (record_len < RECORD_HEADER_SIZE)
            break;
        memcpy(&file_id, payload + pos + 2, sizeof(uint32_t));
        if (file_id)
            FileMapRecordMoved(TailFiles(), file_id, g_pool_tail_link, g_pool_tail_block,
                               MPOOL_BLOCK_HEADER + pos + len, MPOOL_BLOCK_HEADER + pos);
        pos += record_len;
    }
}

bool PoolSpareAvailable(bool dig_reserve)
{
    return s_spare_count > (dig_reserve ? 0u : (uint32_t)SPARE_RESERVE);
}

/* The turn, leaving the pool at least `keep` Spares. */
static bool TailTurnKeeping(uint32_t keep)
{
    if (!g_pool_tail || s_spare_count <= keep || g_pool_chain_len >= g_pool_chain_cap)
        return false;

    MetaPoolPage* page = s_spares;
    s_spares = page->next;
    s_spare_count--;

    /* The Spare's block and image become the tail; its page carries the old
     * tail away as a closed block, whose link names the Spare. */
    uint32_t       spare_block = page->block;
    MetaPoolBlock* spare_image = page->image;
    g_pool_tail->next_block = spare_block;
    g_pool_tail->kept       = PoolCountKept(g_pool_tail);

    /* Closed now, and never written over again: its mix joins the fold. */
    MetaPoolLink* closing = &g_pool_chain[g_pool_chain_len - 1];
    closing->mix        = PoolBlockMix(g_pool_tail_block, g_pool_tail);
    g_pool_closed_fold ^= closing->mix;

    page->next    = NULL;
    page->block   = g_pool_tail_block;
    page->link    = g_pool_tail_link;
    page->image   = g_pool_tail;
    page->gen_max = g_pool_tail_gen;
    if (g_pool_tail_gen > s_closed_gen_max)
        s_closed_gen_max = g_pool_tail_gen;
    /* page->need stays: the stamp the closed block's link names */
    if (s_closed_tail)
        __atomic_store_n(&s_closed_tail->next, page, __ATOMIC_RELEASE);
    else
        s_closed_head = page;
    s_closed_tail = page;
    if (!s_unnamed_from)
        s_unnamed_from = page;

    PoolChainAppend(spare_block);
    g_pool_tail         = spare_image;
    g_pool_tail_block   = spare_block;
    g_pool_tail_link    = g_pool_chain[g_pool_chain_len - 1].link;
    g_pool_tail_frozen  = 0;
    g_pool_tail_gen     = 0;
    g_pool_tail_changed = false;     /* the Spare's stamp is what the medium holds */
    s_tail_edits++;
    return true;
}

bool PoolTailTurn(bool dig_reserve)
{
    return TailTurnKeeping(dig_reserve ? 0u : (uint32_t)SPARE_RESERVE);
}

bool PoolTailTurnGlean(void)
{
    return TailTurnKeeping((uint32_t)SPARE_FOR_QUITCLAIMS);
}

RecordWhere PoolAppend(const uint8_t* packed, uint32_t bytes, uint64_t gen)
{
    uint32_t pos = g_pool_tail->used_bytes;
    uint32_t file_id;

    memcpy(g_pool_tail->payload + pos, packed, bytes);
    memcpy(&file_id, packed + 2, sizeof(uint32_t));
    g_pool_tail->used_bytes = (uint16_t)(pos + bytes);
    g_pool_tail->record_count++;
    if (file_id)
        g_pool_tail->kept++;
    g_pool_tail_gen     = gen;
    g_pool_tail_changed = true;
    s_tail_edits++;

    RecordWhere where = { .link = g_pool_tail_link, .offset = (uint16_t)(MPOOL_BLOCK_HEADER + pos),
                          .len = (uint16_t)bytes, .gen = gen, .shape = 0 };
    return where;
}

// ---------------------------------------------------------------------------
// Spares
// ---------------------------------------------------------------------------

/* The page into the stock, once the chain has room for the link it will
 * take: every Spare in the stock has its link's room made, so a tail turns
 * under the state lock without asking the heap. A write of the map since the
 * Spare's take may have carried it taken: its sector is owed again, to go out
 * leaving it free. */
static error_t SpareInto(MetaPoolPage* page)
{
    for (;;) {
        TagFSLockTake(TailLock());
        uint32_t want = g_pool_chain_len + s_spare_count + 1u;
        if (want <= g_pool_chain_cap) {
            page->next = s_spares;
            s_spares   = page;
            s_spare_count++;
            BlockMapOweLocked(page->block);
            TagFSLockDrop(TailLock());
            return OK;
        }
        TagFSLockDrop(TailLock());
        if (PoolChainRoom(want) != 0)
            return ERR_NO_MEMORY;
    }
}

void PoolSpareDrop(uint32_t block, MetaPoolBlock* image)
{
    /* Named by nothing: the block goes back, its stamp — when it landed — an
     * empty header in ground the allocator may hand out. */
    TagFSLockTake(TailLock());
    (void)BlockMapGiveLocked(block, 1);
    TagFSLockDrop(TailLock());
    kfree(image);
}

error_t PoolSpareAdopt(uint32_t block, MetaPoolBlock* image, uint64_t need)
{
    MetaPoolPage* page = kmalloc(sizeof(MetaPoolPage));
    error_t       rc   = page ? OK : ERR_NO_MEMORY;
    if (page) {
        memset(page, 0, sizeof(*page));
        page->block = block;
        page->image = image;
        page->need  = need;
        rc = SpareInto(page);
    }
    if (rc != OK) {
        PoolSpareDrop(block, image);
        kfree(page);
    }
    return rc;
}

error_t PoolSpareBegin(uint32_t* out_block, MetaPoolBlock** out_image)
{
    MetaPoolBlock* image = kmalloc(sizeof(MetaPoolBlock));
    if (!image)
        return ERR_NO_MEMORY;

    TagFSLockTake(TailLock());
    error_t took = BlockMapTakeLocked(1, 0, out_block);
    TagFSLockDrop(TailLock());
    if (took != OK) {
        kfree(image);
        return took;
    }
    memset(image, 0, sizeof(*image));
    image->magic = TAGFS_MPOOL_MAGIC;
    *out_image   = image;
    return OK;
}

error_t PoolSpareMake(void)
{
    MetaPoolBlock* image = kmalloc(sizeof(MetaPoolBlock));
    if (!image)
        return ERR_NO_MEMORY;

    uint32_t block = 0;
    uint64_t need  = 0;
    error_t  rc    = SpareStamp(TAGFS_MPOOL_MAGIC, image, &block, &need);
    if (rc != OK) {
        kfree(image);
        return rc;
    }
    return PoolSpareAdopt(block, image, need);
}

error_t PoolSpareRefill(uint32_t want)
{
    for (;;) {
        TagFSLockTake(TailLock());
        bool enough = s_spare_count >= want;
        TagFSLockDrop(TailLock());
        if (enough)
            return OK;
        error_t rc = PoolSpareMake();
        if (rc != OK)
            return rc;
    }
}

MetaPoolPage* PoolSparesGiveBack(void)
{
    MetaPoolPage* given = NULL;
    MetaPoolPage* kept  = NULL;
    uint32_t      left  = 0;

    while (s_spares) {
        MetaPoolPage* p = s_spares;
        s_spares = p->next;
        if (BlockMapGiveHeldLocked(p->block, 1) == ERR_NO_MEMORY) {
            p->next = kept;
            kept    = p;
            left++;
        } else {
            p->next = given;
            given   = p;
        }
    }
    s_spares      = kept;
    s_spare_count = left;
    if (left)
        kprintf("[MetaPool] %u Spare(s) of the last cycle stay in the stock — the block map had "
                "no room to write their gives down; the map leaves them free on the medium, "
                "and a later cycle gives them back\n", left);
    return given;
}

uint32_t PoolSparesHeld(uint32_t* out, uint32_t cap)
{
    uint32_t i = 0;
    for (MetaPoolPage* p = s_spares; p && out && i < cap; p = p->next)
        out[i++] = p->block;
    return s_spare_count;
}

static uint32_t LeaveOut(uint32_t block, uint32_t first, uint32_t count, uint8_t* bits)
{
    if (block < first || block - first >= count)
        return 0;
    uint32_t off = block - first;
    bits[off / 8] &= (uint8_t)~(1u << (off % 8));
    return 1;
}

uint32_t PoolUnnamedLeftOut(uint32_t first, uint32_t count, uint8_t* bits)
{
    uint32_t left = 0;
    for (MetaPoolPage* p = s_spares; p; p = p->next)
        left += LeaveOut(p->block, first, count, bits);
    for (MetaPoolPage* p = s_unnamed_from; p; p = p->next)
        left += LeaveOut(p->image->next_block, first, count, bits);
    return left;
}

void PoolPagesFree(MetaPoolPage* list)
{
    while (list) {
        MetaPoolPage* next = list->next;
        kfree(list->image);
        kfree(list);
        list = next;
    }
}

// ---------------------------------------------------------------------------
// The Settle cycle
// ---------------------------------------------------------------------------

/* The tail closed early for a waiter, onto a Spare the deletes may spare:
 * otherwise it stays open, is written as it stands, and that is said once. */
static void TailCloseEarly(PoolSnap* snap, bool dig_reserve)
{
    if (TailTurnKeeping(dig_reserve ? 0u : (uint32_t)SPARE_FOR_QUITCLAIMS)) {
        s_unclosed_said = false;
        return;
    }
    snap->unclosed = true;
    if (!s_unclosed_said)
        kprintf("[MetaPool] a cycle owes a waiter a closed block and the pool holds %u "
                "Spare(s), the last %u of them the deletes' — the tail stays open, and "
                "what the waiter is answered for lies in a block the next cycle writes "
                "again in place, where a flash page torn by the power can take it, until "
                "a Spare can be made\n",
                s_spare_count, (unsigned)SPARE_FOR_QUITCLAIMS);
    s_unclosed_said = true;
}

void PoolSnapTake(PoolSnap* snap, uint64_t close_upto, bool dig_reserve)
{
    memset(snap, 0, sizeof(*snap));
    if (!g_pool_tail)
        return;

    /* Every record the tail holds is newer than every closed block's: a record
     * owed a closed block is in the tail exactly when it is newer than the
     * newest record ever closed. */
    if (close_upto > s_closed_gen_max && g_pool_tail->used_bytes > 0)
        TailCloseEarly(snap, dig_reserve);

    /* Every closed block is in this snapshot, so every link turned so far is
     * written at its S3: the blocks they name are the map's again at its S1. */
    for (MetaPoolPage* p = s_unnamed_from; p; p = p->next)
        BlockMapOweLocked(p->image->next_block);
    s_unnamed_from = NULL;

    snap->first = s_closed_head;
    snap->upto  = s_closed_tail;
    snap->next  = s_closed_head;
    for (MetaPoolPage* p = s_closed_head; p; p = p->next) {
        if (p->need > snap->link_need)
            snap->link_need = p->need;
        if (p->gen_max > snap->gen_max)
            snap->gen_max = p->gen_max;
        if (p == s_closed_tail)
            break;
    }

    snap->tail_block = g_pool_tail_block;
    snap->tail_edits = s_tail_edits;
    snap->tail_write = g_pool_tail_changed;
    if (snap->tail_write) {
        memcpy(&s_settle_tail, g_pool_tail, sizeof(s_settle_tail));
        s_settle_tail.kept = PoolCountKept(&s_settle_tail);
    }
    g_pool_tail_frozen = g_pool_tail->used_bytes;
}

error_t PoolSnapWriteNext(PoolSnap* snap, bool* done)
{
    *done = false;
    MetaPoolPage* p = snap->next;
    if (p) {
        error_t wrote = TagFSMetaBlockWrite(p->block, p->image);
        if (wrote != OK) {
            kprintf("[MetaPool] closed block %u of this volume's metadata would not be "
                    "written (error %u) — it stays in memory, and the next cycle writes it "
                    "again\n", p->block, (unsigned)wrote);
            return wrote;
        }
        snap->next = p == snap->upto ? NULL : __atomic_load_n(&p->next, __ATOMIC_ACQUIRE);
        return OK;
    }
    if (snap->tail_write && !snap->tail_written) {
        error_t wrote = TagFSMetaBlockWrite(snap->tail_block, &s_settle_tail);
        if (wrote != OK) {
            kprintf("[MetaPool] block %u, the tail of this volume's metadata, would not be "
                    "written (error %u) — the next cycle writes it again\n",
                    snap->tail_block, (unsigned)wrote);
            return wrote;
        }
        snap->tail_written = true;
        return OK;
    }
    *done = true;
    return OK;
}

uint64_t PoolClosedSettledGen(void)
{
    return __atomic_load_n(&s_closed_settled_gen, __ATOMIC_ACQUIRE);
}

bool PoolClosedWaiting(void)
{
    return __atomic_load_n(&s_closed_head, __ATOMIC_ACQUIRE) != NULL;
}

uint64_t PoolSnapSettled(const PoolSnap* snap, MetaPoolPage** spent)
{
    *spent = NULL;
    if (snap->upto) {
        *spent        = s_closed_head;
        s_closed_head = snap->upto->next;
        if (!s_closed_head)
            s_closed_tail = NULL;
        snap->upto->next = NULL;
    }
    if (snap->gen_max > s_closed_settled_gen)
        __atomic_store_n(&s_closed_settled_gen, snap->gen_max, __ATOMIC_RELEASE);
    if (snap->tail_write && s_tail_edits == snap->tail_edits &&
        g_pool_tail_block == snap->tail_block)
        g_pool_tail_changed = false;
    return s_closed_settled_gen;
}

void PoolSealTake(SettleMark* mark)
{
    if (!g_pool_tail || !g_pool_chain_len)
        return;
    uint32_t used = g_pool_tail->used_bytes;
    mark->chain_first   = g_pool_chain[0].block;
    mark->pool_to_block = g_pool_tail_block;
    mark->pool_to_bytes = used;
    mark->closed_crc32  = g_pool_closed_fold;
    mark->tail_crc32    = KCrc32(g_pool_tail->payload, used);
}

uint64_t PoolBlocksHeld(void)
{
    return (uint64_t)g_pool_chain_len + s_spare_count + (GleanHeadOffChain() ? 1u : 0u);
}

uint64_t PoolTailGen(void)
{
    return g_pool_tail && g_pool_tail->used_bytes ? g_pool_tail_gen : 0;
}

void PoolTailShutdown(void)
{
    PoolPagesFree(s_closed_head);
    PoolPagesFree(s_spares);
    kfree(g_pool_tail);
    s_closed_head        = NULL;
    s_closed_tail        = NULL;
    s_spares             = NULL;
    s_spare_count        = 0;
    s_closed_settled_gen = 0;
    s_closed_gen_max     = 0;
    s_unnamed_from       = NULL;
    s_unclosed_said      = false;
    s_tail_edits         = 0;
    g_pool_tail          = NULL;
    g_pool_tail_block    = 0;
    g_pool_tail_link     = 0;
    g_pool_tail_frozen   = 0;
    g_pool_tail_gen      = 0;
    g_pool_tail_changed  = false;
}
