#include "meta_pool_internal.h"
#include "../plot/plot.h"
#include "../../../lib/kernel/crypto.h"

/*
 * The mount's walk of the pool (meta_pool.h, THE POOL IS WALKED ONCE, AND THE
 * SEAL SAYS HOW FAR). The first walk holds every block it reads against the
 * seals as it passes and puts the records of the newest seal's span; only
 * when that seal is not borne out is the walk made again, up to where the
 * mount then stands. The mount's alone: the door is shut, so the chain, the
 * tail and the map are written with no lock.
 */

typedef enum {
    SEAL_UNMET = 0,          /* its `to` block not reached (yet) */
    SEAL_BORNE,              /* closed blocks and open block both as the seal says */
    SEAL_TAIL_TORN,          /* the closed blocks are, the open block is not */
    SEAL_CLOSED_BROKEN,      /* the closed blocks are not, or the walk never reached `to` */
} SealVerdict;

typedef struct {
    const SettleMark* mark;
    uint32_t          fold;        /* the mixes of the blocks met before its `to` */
    SealVerdict       verdict;
    bool              closed;      /* its `to` met short of the walk's own: closed by the walk's seal */
} SealTry;

/* One walk. `sealed`: the chain is read up to `to` and past it only for the
 * file ids its records name; `torn`: the `to` block's records roll back. */
typedef struct {
    uint32_t start;
    bool     sealed;
    bool     torn;
    uint32_t to_block;
    uint32_t to_bytes;
    uint32_t from_block;           /* the stood seal's own records begin here (RT-22) */
    uint32_t from_bytes;
    bool     has_from;
    SealTry* judge;                /* the first walk's seals; NULL for a walk made again */
} PoolPlan;

typedef struct {
    MetaPoolBlock*    here;        /* the block being read; the tail's buffer at the end */
    uint32_t          run;
    uint32_t          disagreed;
    bool              in_span;
    uint32_t          span_cap;
    bool              span_refused;
    uint32_t          gone_cap;
    uint32_t          skipped;       /* blocks whose records end short of the bytes read to */
    uint32_t          skipped_bytes;
    uint32_t          skipped_block; /* the first of them, and the byte its records end at */
    uint32_t          skipped_at;
    uint32_t          dropped;       /* records whose extents are not followed */
    uint32_t          dropped_fid;   /* the first of them */
    PoolMountOutcome* out;
} PoolWalk;

/* A walk the first pass could not take further: the seals' verdicts say which
 * walk comes next. */
#define POOL_WALK_STOPPED ((error_t)0x7FFFFFFF)

static bool WalkChainAppend(uint32_t block)
{
    if (g_pool_chain_len == g_pool_chain_cap) {
        uint32_t      cap   = g_pool_chain_cap ? g_pool_chain_cap * 2u : 8u;
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

/* `fid` added to the list `*ids` of `*count`, which has room for `*cap` and
 * doubles when it has none left. False when there is no memory for it. */
static bool WalkIdsAdd(uint32_t** ids, uint32_t* count, uint32_t* cap, uint32_t fid)
{
    if (*count == *cap) {
        uint32_t  room = *cap ? *cap * 2u : 32u;
        uint32_t* more = kmalloc(sizeof(uint32_t) * room);
        if (!more)
            return false;
        if (*count)
            memcpy(more, *ids, sizeof(uint32_t) * *count);
        kfree(*ids);
        *ids = more;
        *cap = room;
    }
    (*ids)[(*count)++] = fid;
    return true;
}

static void WalkNoteSpan(PoolWalk* w, uint32_t fid)
{
    PoolMountOutcome* out = w->out;
    if (w->span_refused)
        return;
    if (!WalkIdsAdd(&out->last_span, &out->last_span_count, &w->span_cap, fid)) {
        w->span_refused = true;
        kprintf("[MetaPool] no memory to note the files the last cycle wrote — their "
                "digests are not set back to unknown on this unclean mount\n");
    }
}

/* The file ids the records of `blk` name from byte `from` on — records no seal
 * holds, whose ids the medium has seen all the same. Only a record that checks
 * out names an id: a bit rotten in the id of one that does not would carry the
 * next file id anywhere, and every create after it with it. */
static void WalkFidsPast(PoolWalk* w, const MetaPoolBlock* blk, uint32_t from)
{
    uint32_t used = blk->used_bytes <= TAGFS_MPOOL_DATA_SIZE ? blk->used_bytes
                                                             : TAGFS_MPOOL_DATA_SIZE;
    for (uint32_t pos = 0; pos + RECORD_HEADER_SIZE <= used; ) {
        uint16_t len;
        memcpy(&len, blk->payload + pos, sizeof(len));
        if (len < RECORD_HEADER_SIZE || len > used - pos)
            return;
        uint32_t fid = pos >= from ? UnpackV1Fid(blk->payload + pos, len) : 0;
        if (fid > w->out->top_fid)
            w->out->top_fid = fid;
        pos += len;
    }
}

/* One record into the map through the mirror's door: an ACTIVE record becomes
 * its file's, a Quitclaim takes the file out — and its row, once the table is
 * read: the table is read after the pool, so the row is not there yet to take
 * (mount.c). */
static error_t WalkPut(PoolWalk* w, const uint8_t* at, uint32_t len, uint32_t fid,
                       uint32_t link, uint32_t pos)
{
    TagFSMetadata meta;
    bool          dropped = false;
    memset(&meta, 0, sizeof(meta));
    error_t unpacked = UnpackV1(at, len, &meta, &dropped);
    if (unpacked == ERR_NO_MEMORY) {
        kprintf("[MetaPool] there is no memory to unpack file %u's record at mount — this "
                "volume is not mounted\n", fid);
        return ERR_NO_MEMORY;
    }
    if (unpacked != OK || meta.file_id != fid) {
        w->out->unreadable++;
        tagfs_metadata_free(&meta);
        return OK;
    }
    /* Only a record that checks out says which ids the medium has seen. */
    if (fid > w->out->top_fid)
        w->out->top_fid = fid;
    for (uint32_t t = 0; t < meta.tag_count; t++)
        if (meta.tag_ids[t] > w->out->top_tag)
            w->out->top_tag = meta.tag_ids[t];
    if (dropped && w->dropped++ == 0)
        w->dropped_fid = fid;

    bool         active = (meta.flags & TAGFS_FILE_ACTIVE) != 0;
    FileRecord*  rec    = active ? FileRecordOf(&meta) : NULL;
    FileMapStake stake  = { .count = 0 };
    RecordWhere  where  = { .link = link, .offset = (uint16_t)(MPOOL_BLOCK_HEADER + pos),
                            .len = (uint16_t)len, .gen = 0, .shape = 0 };
    tagfs_metadata_free(&meta);
    if ((active && !rec) || PoolMirrorPut(fid, rec, &where, &stake) != OK) {
        kprintf("[MetaPool] there is no memory to hold file %u's record — the map of live "
                "files is what every read and write of this volume stands on, so it is not "
                "mounted\n", fid);
        return ERR_NO_MEMORY;
    }
    if (!active && !WalkIdsAdd(&w->out->gone, &w->out->gone_count, &w->gone_cap, fid)) {
        kprintf("[MetaPool] there is no memory to note the files a Quitclaim took out — "
                "their rows would come back from the file table as files lost, so this "
                "volume is not mounted\n");
        return ERR_NO_MEMORY;
    }
    return OK;
}

/*
 * The records of `blk` up to byte `upto` into the map, in order — the walk is
 * in append order, so a file's last record put is its newest. A record past
 * `upto`, or one whose length does not fit, ends the block's records: a block
 * is packed from its start and nothing is read past a record that is not one.
 * What is left unread of the block is counted, for the line PoolMount says.
 */
static error_t WalkRecords(PoolWalk* w, const PoolPlan* plan, const MetaPoolBlock* blk,
                           uint32_t block, uint32_t link, uint32_t upto)
{
    bool span_here = w->in_span;
    bool from_here = plan->has_from && block == plan->from_block;
    if (from_here)
        w->in_span = true;

    uint32_t pos = 0;
    while (pos + RECORD_HEADER_SIZE <= upto) {
        uint16_t len;
        uint32_t fid;
        memcpy(&len, blk->payload + pos, sizeof(len));
        if (len < RECORD_HEADER_SIZE || len > upto - pos)
            break;
        memcpy(&fid, blk->payload + pos + 2, sizeof(fid));
        if (fid) {
            error_t put = WalkPut(w, blk->payload + pos, len, fid, link, pos);
            if (put != OK)
                return put;
            if (span_here || (from_here && pos >= plan->from_bytes))
                WalkNoteSpan(w, fid);
        }
        pos += len;
    }
    if (pos < upto) {
        if (w->skipped++ == 0) {
            w->skipped_block = block;
            w->skipped_at    = MPOOL_BLOCK_HEADER + pos;
        }
        w->skipped_bytes += upto - pos;
    }
    return OK;
}

/* The seals not yet met, held against the block the walk is at — NULL when it
 * would not read as a block of the pool — on a walk whose own seal's open
 * block is `own_to`. */
static void WalkJudge(SealTry* judge, uint32_t block, const MetaPoolBlock* img, uint32_t own_to)
{
    for (uint32_t i = 0; judge && i < VOLUME_LEDGER_COPIES; i++) {
        SealTry* t = &judge[i];
        if (!t->mark || t->verdict != SEAL_UNMET)
            continue;
        if (block != t->mark->pool_to_block) {
            if (img)
                t->fold ^= PoolBlockMix(block, img);
            else
                t->verdict = SEAL_CLOSED_BROKEN;
            continue;
        }
        uint32_t to   = t->mark->pool_to_bytes;
        bool     tail = img && to <= TAGFS_MPOOL_DATA_SIZE && img->used_bytes >= to &&
                        KCrc32(img->payload, to) == t->mark->tail_crc32;
        t->closed  = block != own_to;
        t->verdict = t->fold != t->mark->closed_crc32 ? SEAL_CLOSED_BROKEN
                   : tail                             ? SEAL_BORNE
                                                      : SEAL_TAIL_TORN;
    }
}

/* Past the span: the blocks a cycle that did not settle linked on, read only
 * for the file ids their records name, and quietly left at the first that is
 * not one. */
static void WalkPastSpan(PoolWalk* w, uint32_t next)
{
    MetaPoolBlock* past = kmalloc(sizeof(MetaPoolBlock));
    for (uint32_t hops = 0; past && next != 0 && next < w->run && hops < w->run; hops++) {
        if (tagfs_read_block(next, past) != OK || past->magic != TAGFS_MPOOL_MAGIC)
            break;
        WalkFidsPast(w, past, 0);
        next = past->next_block;
    }
    kfree(past);
}

/* The tail the mount leaves: the `to` block's settled records alone — the
 * first `to` bytes, which a seal that is not borne out may not even reach —
 * linking nothing, and owed whole to the medium when that is not what the
 * medium holds. */
static void WalkTailAt(PoolWalk* w, const PoolPlan* plan, uint32_t block, uint32_t to)
{
    MetaPoolBlock* t    = w->here;
    bool           owed = plan->sealed &&
                          (plan->torn || t->next_block != 0 || t->used_bytes != to);
    if (plan->torn) {
        memset(t, 0, sizeof(*t));
        t->magic = TAGFS_MPOOL_MAGIC;
    } else if (plan->sealed) {
        memset(t->payload + to, 0, TAGFS_MPOOL_DATA_SIZE - to);
        t->used_bytes = (uint16_t)to;
        t->next_block = 0;
    }
    uint16_t records = 0;
    for (uint32_t pos = 0; pos + RECORD_HEADER_SIZE <= t->used_bytes; records++) {
        uint16_t len;
        memcpy(&len, t->payload + pos, sizeof(len));
        if (len < RECORD_HEADER_SIZE || len > t->used_bytes - pos)
            break;
        pos += len;
    }
    t->record_count     = records;
    t->kept             = PoolCountKept(t);
    g_pool_tail         = t;
    g_pool_tail_block   = block;
    g_pool_tail_link    = g_pool_chain[g_pool_chain_len - 1].link;
    g_pool_tail_frozen  = t->used_bytes;
    g_pool_tail_gen     = 0;
    g_pool_tail_changed = owed;
}

/*
 * ‼ A READ THE MEDIUM REFUSED IS NO VERDICT ON ANY SEAL.
 *
 * A flash drive that drops one command is not a pool torn or rotted: held
 * against the seals as a block that is not the pool's, the refusal of the
 * open block rolled settled records back — and the first cycle wrote the
 * emptied block over the whole one — and the refusal of a closed block sent
 * the mount down the road of a kernel before the seal. The refusal says
 * nothing of what the block holds, so the mount is refused as the read was
 * (counted where the read met it, tagfs_read_block), and asked again when the
 * seat answers. Said; the read's own error.
 */
static error_t WalkRefused(uint32_t block, error_t read)
{
    kprintf("[MetaPool] the medium refused the read of block %u of this volume's metadata "
            "chain (error %d) — a refusal says nothing of what the block holds, so no seal is "
            "judged on it and nothing is rolled back: the volume is not mounted\n",
            block, (int)read);
    return read;
}

/* Why the walk cannot take `block` as a block of the pool, or NULL — what the
 * block holds, and never the medium's refusal to read it, which is *refused:
 * the read's error, OK when it read. */
static const char* WalkRead(PoolWalk* w, uint32_t block, error_t* refused)
{
    *refused = OK;
    if (block >= w->run)
        return "lies outside the volume's data run";
    *refused = tagfs_read_block(block, w->here);
    if (*refused != OK)
        return "would not read";
    if (w->here->magic != TAGFS_MPOOL_MAGIC)
        return "holds no metadata pool";
    return NULL;
}

/*
 * The open block a torn walk rolls back, read for one thing: which files the
 * stood seal's own cycle wrote records of there, among the records that still
 * check out. Their blocks' digests are set back like those of every other file
 * that cycle wrote (RT-22, MountAfterDoor): a block it wrote in place whose
 * digest did not land would otherwise read as rot. Nothing else of the block
 * is taken, and a refused read is the mount's refusal.
 */
static error_t WalkTornSpan(PoolWalk* w, const PoolPlan* plan, uint32_t block)
{
    if (block >= w->run)
        return OK;
    error_t read = tagfs_read_block(block, w->here);
    if (read != OK)
        return WalkRefused(block, read);
    if (w->here->magic != TAGFS_MPOOL_MAGIC)
        return OK;

    const MetaPoolBlock* blk  = w->here;
    bool                 from = plan->has_from && block == plan->from_block;
    uint32_t             used = blk->used_bytes <= TAGFS_MPOOL_DATA_SIZE ? blk->used_bytes
                                                                         : TAGFS_MPOOL_DATA_SIZE;
    uint32_t             upto = plan->to_bytes < used ? plan->to_bytes : used;
    for (uint32_t pos = 0; pos + RECORD_HEADER_SIZE <= upto; ) {
        uint16_t len;
        memcpy(&len, blk->payload + pos, sizeof(len));
        if (len < RECORD_HEADER_SIZE || len > upto - pos)
            break;
        uint32_t fid = UnpackV1Fid(blk->payload + pos, len);
        if (fid && (w->in_span || (from && pos >= plan->from_bytes)))
            WalkNoteSpan(w, fid);
        pos += len;
    }
    return OK;
}

/* One walk by `plan`. OK with the tail laid; POOL_WALK_STOPPED when a first
 * walk met a block that is not the pool's or ran out of chain before its `to`
 * — the seals' verdicts say what comes next; the read's error when the medium
 * refused one; ERR_TAGFS_CORRUPTED (said) or ERR_NO_MEMORY otherwise. */
static error_t WalkOnce(PoolWalk* w, const PoolPlan* plan)
{
    uint32_t block = plan->start;
    for (uint32_t hops = 0; ; hops++) {
        bool at_to = plan->sealed && block == plan->to_block;
        if (at_to && plan->torn) {
            error_t read = WalkTornSpan(w, plan, block);
            if (read != OK)
                return read;
            if (!WalkChainAppend(block))
                return ERR_NO_MEMORY;
            WalkTailAt(w, plan, block, 0);
            return OK;
        }

        error_t     refused;
        const char* why = WalkRead(w, block, &refused);
        if (refused != OK)
            return WalkRefused(block, refused);
        WalkJudge(plan->judge, block, why ? NULL : w->here, plan->to_block);
        if (why) {
            if (plan->judge)
                return POOL_WALK_STOPPED;
            kprintf("[MetaPool] block %u of this volume's metadata chain %s — the records of "
                    "its files cannot all be reached, so it is not mounted (%u block(s) of its "
                    "pool had been read)\n", block, why, g_pool_chain_len);
            return ERR_TAGFS_CORRUPTED;
        }
        if (PoolCountKept(w->here) != w->here->kept)
            w->disagreed++;
        if (!WalkChainAppend(block))
            return ERR_NO_MEMORY;
        uint32_t link = g_pool_chain[g_pool_chain_len - 1].link;
        uint32_t used = w->here->used_bytes <= TAGFS_MPOOL_DATA_SIZE ? w->here->used_bytes
                                                                     : TAGFS_MPOOL_DATA_SIZE;
        uint32_t next = w->here->next_block;

        if (at_to || (!plan->sealed && next == 0)) {
            uint32_t upto = at_to && plan->to_bytes < used ? plan->to_bytes : used;
            error_t  put  = WalkRecords(w, plan, w->here, block, link, upto);
            if (put != OK)
                return put;
            if (at_to) {
                WalkFidsPast(w, w->here, upto);
                WalkPastSpan(w, next);
            }
            WalkTailAt(w, plan, block, upto);
            return OK;
        }

        error_t put = WalkRecords(w, plan, w->here, block, link, used);
        if (put != OK)
            return put;
        MetaPoolLink* closed = &g_pool_chain[g_pool_chain_len - 1];
        closed->mix         = PoolBlockMix(block, w->here);
        g_pool_closed_fold ^= closed->mix;

        const char* stop = next == 0      ? "ends"
                         : hops + 1 >= w->run ? "goes round in a circle"
                                          : NULL;
        if (stop) {
            WalkJudge(plan->judge, 0xFFFFFFFFu, NULL, plan->to_block);
            if (plan->judge)
                return POOL_WALK_STOPPED;
            if (plan->sealed)
                kprintf("[MetaPool] this volume's metadata chain %s at block %u, short of block "
                        "%u the seal it stands on names — it is not mounted\n", stop, block,
                        plan->to_block);
            else
                kprintf("[MetaPool] this volume's metadata chain %s at block %u after %u hops in "
                        "a data run of %u blocks — it is not mounted\n", stop, block, hops + 1,
                        w->run);
            return ERR_TAGFS_CORRUPTED;
        }
        block = next;
    }
}

/* The chain and the map as they were before a walk: a walk made again starts
 * from nothing. */
static void WalkForget(PoolWalk* w)
{
    TagFSState* fs = tagfs_get_state();
    kfree(g_pool_chain);
    g_pool_chain       = NULL;
    g_pool_chain_len   = 0;
    g_pool_chain_cap   = 0;
    g_pool_closed_fold = 0;
    g_pool_live_bytes  = 0;
    g_pool_tail        = NULL;
    FileMapDestroy(fs->files);
    fs->files = FileMapCreate();

    PoolMountOutcome* out = w->out;
    kfree(out->last_span);
    kfree(out->gone);
    out->last_span       = NULL;
    out->last_span_count = 0;
    out->gone            = NULL;
    out->gone_count      = 0;
    out->unreadable      = 0;
    w->span_cap          = 0;
    w->span_refused      = false;
    w->gone_cap          = 0;
    w->in_span           = false;
    w->disagreed         = 0;
    w->skipped           = 0;
    w->skipped_bytes     = 0;
    w->dropped           = 0;
}

static void PlanOfSeal(PoolPlan* plan, const SettleMark* m, bool torn)
{
    memset(plan, 0, sizeof(*plan));
    plan->start      = m->chain_first;
    plan->sealed     = true;
    plan->torn       = torn;
    plan->to_block   = m->pool_to_block;
    plan->to_bytes   = m->pool_to_bytes;
    plan->from_block = m->pool_from_block;
    plan->from_bytes = m->pool_from_bytes;
    plan->has_from   = m->pool_from_block != 0;
}

/*
 * Which seal the pool stands on, once the walk has held each against the
 * medium (meta_pool.h, THE POOL IS WALKED ONCE), `t[0]` the newest copy's and
 * `t[1]` the other's (no mark: none):
 *
 *   the newest borne out;
 *   the newest's closed blocks borne out and its open block not, while the
 *     other's open block is one of the newest's closed blocks — the walk met
 *     it short of the newest's own, and the newest cycle settled it: the
 *     newest, its open block rolled back (decision 5, the owner's B3), which
 *     keeps strictly more than falling back would, all of it behind the
 *     newest cycle's flush;
 *   else the other borne out, which is falling back — the newest's closed
 *     part not borne out, or the two sharing one open block;
 *   else a seal whose closed blocks are borne out and whose open block is not,
 *     the newest first — but only beside a second seal: with none, nothing
 *     says what the open block held before the newest cycle wrote it in place,
 *     and a kernel before the seal, which left the other copy unsealed, kept
 *     its records there — a roll back would take them with it;
 *   else none.
 *
 * The seal's index, or -1; *lied when the newest cycle is rolled back, which
 * format v1 cannot tell from a medium that lied about its flush.
 */
static int SealsDecide(const SealTry* t, PoolStood* stood, bool* fell_back, bool* lied)
{
    const SealTry* n = &t[0];
    const SealTry* o = &t[1];

    *fell_back = false;
    *lied      = false;
    *stood     = POOL_STOOD_SEAL;
    if (n->verdict == SEAL_BORNE)
        return 0;
    *lied  = true;
    *stood = POOL_STOOD_TORN_TAIL;
    if (n->verdict == SEAL_TAIL_TORN && o->mark && o->closed)
        return 0;
    *stood = POOL_STOOD_SEAL;
    if (o->mark && o->verdict == SEAL_BORNE) {
        *fell_back = true;
        return 1;
    }
    *stood = POOL_STOOD_TORN_TAIL;
    if (o->mark && n->verdict == SEAL_TAIL_TORN)
        return 0;
    if (o->mark && o->verdict == SEAL_TAIL_TORN)
        return 1;
    *stood = POOL_STOOD_NO_SEAL;
    *lied  = !o->mark && n->verdict == SEAL_TAIL_TORN;
    return -1;
}

static void SayFellBack(const PoolMountAsk* ask, const SealTry* t)
{
    kprintf("[TagFS] fell back: the newest seal (cycle %llu) names pool records the medium "
            "does not hold — %s — so the volume stands on cycle %llu, the one before it, "
            "and from now on a freed block rests one cycle longer on this medium "
            "(SETTLE_MEDIUM_LIED): the cycle was cut before its flush, or the medium said "
            "a flush was kept that was not\n",
            (unsigned long long)ask->seal[0]->settle,
            t[0].verdict == SEAL_TAIL_TORN ? "its open block does not bear the seal out"
                                           : "its closed blocks do not bear the seal out",
            (unsigned long long)ask->seal[1]->settle);
}

/* After the walks that did not stand on a seal borne out: which walk comes
 * next, said. */
static void WalkChoose(const PoolMountAsk* ask, const SealTry* t, PoolPlan* plan,
                       PoolMountOutcome* out)
{
    PoolStood stood;
    bool      fell_back, lied;
    int       at = SealsDecide(t, &stood, &fell_back, &lied);
    out->stood     = stood;
    out->seal      = at;
    out->fell_back = fell_back;
    out->lied      = lied;
    if (stood == POOL_STOOD_SEAL) {
        PlanOfSeal(plan, ask->seal[1], false);
        SayFellBack(ask, t);
        return;
    }
    if (stood == POOL_STOOD_TORN_TAIL) {
        const SettleMark* m = ask->seal[at];
        PlanOfSeal(plan, m, true);
        out->torn_block = m->pool_to_block;
        out->torn_end   = MPOOL_BLOCK_HEADER + m->pool_to_bytes;
        kprintf("[TagFS] the pool's open block %u does not hold what the seal of cycle %llu "
                "says — it was torn as it was written: its %u byte(s) of records are ROLLED "
                "BACK to the last closed block%s. Every file whose newest record was there "
                "stands on the record before it, a file with none before it is gone, the "
                "volume mounts writable, and from now on a freed block rests one cycle longer "
                "on this medium (SETTLE_MEDIUM_LIED)\n", m->pool_to_block,
                (unsigned long long)m->settle, m->pool_to_bytes,
                at == 0 && t[1].mark && t[1].closed
                    ? ", which holds the other seal's open block whole" : "");
        return;
    }
    memset(plan, 0, sizeof(*plan));
    plan->start = ask->head;
    if (lied)
        kprintf("[TagFS] the newest seal (cycle %llu) is the only one the Ledger holds, and "
                "the pool's open block %u does not hold what it says — the block was written "
                "in place over records a kernel before the seal may have kept there, so "
                "nothing is rolled back: the whole chain is read as such a kernel reads it, "
                "the file table says which files there are, and from now on a freed block "
                "rests one cycle longer on this medium (SETTLE_MEDIUM_LIED)\n",
                (unsigned long long)ask->seal[0]->settle, ask->seal[0]->pool_to_block);
    else
        kprintf("[TagFS] no copy's seal is borne out by the pool's closed blocks — a kernel "
                "before the seal rewrote records in them in place, or they rotted — so the "
                "whole chain is read as such a kernel reads it: the file table says which "
                "files there are, and nothing is rolled back\n");
}

static bool WalkJudged(error_t rc)
{
    return rc == OK || rc == POOL_WALK_STOPPED;
}

/* A walk made again starts from nothing: false when there is no memory for
 * the map it fills. */
static bool WalkAgain(PoolWalk* w)
{
    WalkForget(w);
    return tagfs_get_state()->files != NULL;
}

/*
 * The newest seal not borne out: the other copy's is judged, and the walk its
 * verdict and the newest's choose is made. When the two chains begin at
 * different blocks — the newest cycle's Glean took blocks off the front — the
 * walk from the newest one's first block never met the other's first ones, so
 * the other is judged by a walk from its own, which is the walk that stands on
 * it when the verdicts fall back to it: the blocks taken off rest in Fallow
 * until both copies of the Ledger have moved past them, so they are all there
 * to read.
 */
static error_t WalkAfterNewest(PoolWalk* w, const PoolMountAsk* ask, SealTry* tries,
                               PoolPlan* plan, PoolMountOutcome* out, bool apart)
{
    if (!WalkAgain(w))
        return ERR_NO_MEMORY;
    if (apart) {
        SealTry own[VOLUME_LEDGER_COPIES];
        memset(own, 0, sizeof(own));
        own[1].mark = ask->seal[1];
        PlanOfSeal(plan, ask->seal[1], false);
        plan->judge = own;
        error_t rc = WalkOnce(w, plan);
        own[1].closed = tries[1].closed;
        tries[1]      = own[1];
        if (!WalkJudged(rc))
            return rc;

        PoolStood stood;
        bool      fell_back, lied;
        if (SealsDecide(tries, &stood, &fell_back, &lied) == 1 && stood == POOL_STOOD_SEAL) {
            out->stood     = stood;
            out->seal      = 1;
            out->fell_back = fell_back;
            out->lied      = lied;
            SayFellBack(ask, tries);
            return OK;
        }
        if (!WalkAgain(w))
            return ERR_NO_MEMORY;
    }
    WalkChoose(ask, tries, plan, out);
    return WalkOnce(w, plan);
}

error_t PoolMount(const PoolMountAsk* ask, PoolMountOutcome* out)
{
    memset(out, 0, sizeof(*out));
    out->seal = -1;
    meta_pool_shutdown();

    PoolWalk w;
    memset(&w, 0, sizeof(w));
    w.out = out;
    w.run = tagfs_get_state()->layout.data_blocks;
    if (w.run == 0) {
        kprintf("[MetaPool] this volume states a data run of no blocks at all, so there is "
                "nowhere for a metadata pool to be — it is not mounted\n");
        return ERR_TAGFS_CORRUPTED;
    }
    /* The walk reads into the buffer that becomes the tail. */
    w.here = kmalloc(sizeof(MetaPoolBlock));
    if (!w.here) {
        kprintf("[MetaPool] there is no memory for the block this volume's metadata chain "
                "is walked through — it is not mounted\n");
        return ERR_NO_MEMORY;
    }

    SealTry  tries[VOLUME_LEDGER_COPIES];
    PoolPlan plan;
    memset(tries, 0, sizeof(tries));
    memset(&plan, 0, sizeof(plan));
    const SettleMark* s0 = ask->seal[0];
    const SettleMark* s1 = ask->seal[1];

    error_t rc;
    if (!s0) {
        plan.start = ask->head;
        out->stood = POOL_STOOD_NO_SEAL;
        rc         = WalkOnce(&w, &plan);
    } else {
        /* A seal whose chain begins elsewhere is judged by a walk of its own;
         * this one still says whether the newest closed its open block. */
        bool apart = s1 && s1->chain_first != s0->chain_first;
        tries[0].mark = s0;
        tries[1].mark = s1;
        PlanOfSeal(&plan, s0, false);
        plan.judge = tries;
        rc = WalkOnce(&w, &plan);
        if (WalkJudged(rc) && tries[0].verdict == SEAL_BORNE) {
            /* The common mount: one walk, and it stood on the newest seal. */
            out->stood = POOL_STOOD_SEAL;
            out->seal  = 0;
            rc         = OK;
        } else if (WalkJudged(rc)) {
            rc = WalkAfterNewest(&w, ask, tries, &plan, out, apart);
        }
    }
    if (rc != OK) {
        if (!g_pool_tail)
            kfree(w.here);
        meta_pool_shutdown();
        kfree(out->last_span);
        kfree(out->gone);
        out->last_span       = NULL;
        out->last_span_count = 0;
        out->gone            = NULL;
        out->gone_count      = 0;
        return rc == POOL_WALK_STOPPED ? ERR_TAGFS_CORRUPTED : rc;
    }

    /*
     * ‼ SAID, AND NOT WRITTEN BACK. The stored count is a convenience and the
     * scan is the truth; the next write of each block carries the right number
     * out by itself. A volume made before the count existed carries zero in
     * every block and says this once.
     */
    if (w.disagreed)
        kprintf("[MetaPool] %u of this volume's %u metadata block(s) state a kept-record "
                "count the records themselves do not bear out — the records are counted, "
                "and that is the number used\n", w.disagreed, g_pool_chain_len);
    if (out->unreadable)
        kprintf("[MetaPool] %u record(s) of this volume's pool do not check out — each file "
                "they belong to keeps the newest of its records that does\n", out->unreadable);
    if (w.skipped)
        kprintf("[MetaPool] %u block(s) of this volume's pool end in %u byte(s) that do not "
                "read as records — a record whose length does not fit stops its block, and "
                "nothing after it is read: the first at block %u, byte %u. Each file whose "
                "records lay there is judged by those read before them\n", w.skipped,
                w.skipped_bytes, w.skipped_block, w.skipped_at);
    if (w.dropped)
        kprintf("[MetaPool] %u record(s) of this volume's pool name a block outside its data "
                "run of %u blocks, or an extent of none — each is taken up naming no blocks, "
                "and nothing is written over its file: the first is file %u's\n", w.dropped,
                w.run, w.dropped_fid);

    out->from_block = g_pool_tail_block;
    out->from_bytes = g_pool_tail->used_bytes;
    return OK;
}

#ifdef CONFIG_VOLUME_TESTS
/* The seals of `tries` held against the chain walked from `mark`'s first block
 * up to its open one, reading only. */
static void JudgeWalk(SealTry* tries, const SettleMark* mark, MetaPoolBlock* blk, uint32_t run)
{
    uint32_t block = mark->chain_first;
    for (uint32_t hops = 0; hops < run; hops++) {
        bool ok = block < run && tagfs_read_block(block, blk) == OK &&
                  blk->magic == TAGFS_MPOOL_MAGIC;
        WalkJudge(tries, block, ok ? blk : NULL, mark->pool_to_block);
        if (!ok || block == mark->pool_to_block)
            break;
        if (blk->next_block == 0) {
            WalkJudge(tries, 0xFFFFFFFFu, NULL, mark->pool_to_block);
            break;
        }
        block = blk->next_block;
    }
}

PoolStood PoolSealsJudge(const PoolMountAsk* ask, int* out_seal, bool* out_fell_back,
                         bool* out_lied)
{
    SealTry        tries[VOLUME_LEDGER_COPIES];
    MetaPoolBlock* blk = kmalloc(sizeof(MetaPoolBlock));
    uint32_t       run = tagfs_get_state()->layout.data_blocks;
    memset(tries, 0, sizeof(tries));
    *out_seal      = -1;
    *out_fell_back = false;
    *out_lied      = false;
    if (!blk || !ask->seal[0]) {
        kfree(blk);
        return POOL_STOOD_NO_SEAL;
    }
    bool apart    = ask->seal[1] && ask->seal[1]->chain_first != ask->seal[0]->chain_first;
    tries[0].mark = ask->seal[0];
    tries[1].mark = ask->seal[1];
    JudgeWalk(tries, ask->seal[0], blk, run);
    if (apart && tries[0].verdict != SEAL_BORNE) {
        SealTry own[VOLUME_LEDGER_COPIES];
        memset(own, 0, sizeof(own));
        own[1].mark = ask->seal[1];
        JudgeWalk(own, ask->seal[1], blk, run);
        own[1].closed = tries[1].closed;
        tries[1]      = own[1];
    }
    kfree(blk);

    PoolStood stood;
    *out_seal = SealsDecide(tries, &stood, out_fell_back, out_lied);
    return stood;
}
#endif
