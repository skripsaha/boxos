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

static void WalkNoteSpan(PoolWalk* w, uint32_t fid)
{
    PoolMountOutcome* out = w->out;
    if (w->span_refused)
        return;
    if (out->last_span_count == w->span_cap) {
        uint32_t  cap  = w->span_cap ? w->span_cap * 2u : 32u;
        uint32_t* more = kmalloc(sizeof(uint32_t) * cap);
        if (!more) {
            w->span_refused = true;
            kprintf("[MetaPool] no memory to note the files the last cycle wrote — their "
                    "digests are not set back to unknown on this unclean mount\n");
            return;
        }
        if (out->last_span_count)
            memcpy(more, out->last_span, sizeof(uint32_t) * out->last_span_count);
        kfree(out->last_span);
        out->last_span = more;
        w->span_cap    = cap;
    }
    out->last_span[out->last_span_count++] = fid;
}

/* The file ids the records of `blk` name from byte `from` on — records no seal
 * holds, whose ids the medium has seen all the same. */
static void WalkFidsPast(PoolWalk* w, const MetaPoolBlock* blk, uint32_t from)
{
    uint32_t used = blk->used_bytes <= TAGFS_MPOOL_DATA_SIZE ? blk->used_bytes
                                                             : TAGFS_MPOOL_DATA_SIZE;
    for (uint32_t pos = 0; pos + RECORD_HEADER_SIZE <= used; ) {
        uint16_t len;
        uint32_t fid;
        memcpy(&len, blk->payload + pos, sizeof(len));
        if (len < RECORD_HEADER_SIZE || len > used - pos)
            return;
        memcpy(&fid, blk->payload + pos + 2, sizeof(fid));
        if (pos >= from && fid > w->out->top_fid)
            w->out->top_fid = fid;
        pos += len;
    }
}

/* One record into the map through the mirror's door: an ACTIVE record becomes
 * its file's, a Quitclaim takes the file out. */
static error_t WalkPut(PoolWalk* w, const uint8_t* at, uint32_t len, uint32_t fid,
                       uint32_t link, uint32_t pos)
{
    TagFSMetadata meta;
    memset(&meta, 0, sizeof(meta));
    error_t unpacked = UnpackV1(at, len, &meta);
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
    for (uint32_t t = 0; t < meta.tag_count; t++)
        if (meta.tag_ids[t] > w->out->top_tag)
            w->out->top_tag = meta.tag_ids[t];

    bool         active = (meta.flags & TAGFS_FILE_ACTIVE) != 0;
    FileRecord*  rec    = active ? FileRecordOf(&meta) : NULL;
    FileMapStake stake  = { .count = 0 };
    RecordWhere  where  = { .link = link, .offset = MPOOL_BLOCK_HEADER + pos, .gen = 0, .shape = 0 };
    tagfs_metadata_free(&meta);
    if ((active && !rec) || PoolMirrorPut(fid, rec, &where, &stake) != OK) {
        kprintf("[MetaPool] there is no memory to hold file %u's record — the map of live "
                "files is what every read and write of this volume stands on, so it is not "
                "mounted\n", fid);
        return ERR_NO_MEMORY;
    }
    return OK;
}

/*
 * The records of `blk` up to byte `upto` into the map, in order — the walk is
 * in append order, so a file's last record put is its newest. A record past
 * `upto`, or one whose length does not fit, ends the block's records: a block
 * is packed from its start and nothing is read past a record that is not one.
 */
static error_t WalkRecords(PoolWalk* w, const PoolPlan* plan, const MetaPoolBlock* blk,
                           uint32_t block, uint32_t link, uint32_t upto)
{
    bool span_here = w->in_span;
    bool from_here = plan->has_from && block == plan->from_block;
    if (from_here)
        w->in_span = true;

    for (uint32_t pos = 0; pos + RECORD_HEADER_SIZE <= upto; ) {
        uint16_t len;
        uint32_t fid;
        memcpy(&len, blk->payload + pos, sizeof(len));
        if (len < RECORD_HEADER_SIZE || len > upto - pos)
            break;
        memcpy(&fid, blk->payload + pos + 2, sizeof(fid));
        if (fid > w->out->top_fid)
            w->out->top_fid = fid;
        if (fid) {
            error_t put = WalkPut(w, blk->payload + pos, len, fid, link, pos);
            if (put != OK)
                return put;
            if (span_here || (from_here && pos >= plan->from_bytes))
                WalkNoteSpan(w, fid);
        }
        pos += len;
    }
    return OK;
}

/* The seals not yet met, held against the block the walk is at — NULL when it
 * would not read as a block of the pool. */
static void WalkJudge(SealTry* judge, uint32_t block, const MetaPoolBlock* img)
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

/* Why the walk cannot read `block` as a block of the pool, or NULL. */
static const char* WalkRead(PoolWalk* w, uint32_t block)
{
    if (block >= w->run)
        return "lies outside the volume's data run";
    if (tagfs_read_block(block, w->here) != OK)
        return "would not read";
    if (w->here->magic != TAGFS_MPOOL_MAGIC)
        return "holds no metadata pool";
    return NULL;
}

/* One walk by `plan`. OK with the tail laid; POOL_WALK_STOPPED when a first
 * walk met a block it cannot read or ran out of chain before its `to` — the
 * seals' verdicts say what comes next; ERR_TAGFS_CORRUPTED (said) or
 * ERR_NO_MEMORY otherwise. */
static error_t WalkOnce(PoolWalk* w, const PoolPlan* plan)
{
    uint32_t block = plan->start;
    for (uint32_t hops = 0; ; hops++) {
        bool at_to = plan->sealed && block == plan->to_block;
        if (at_to && plan->torn) {
            if (!WalkChainAppend(block))
                return ERR_NO_MEMORY;
            WalkTailAt(w, plan, block, 0);
            return OK;
        }

        const char* why = WalkRead(w, block);
        WalkJudge(plan->judge, block, why ? NULL : w->here);
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
            WalkJudge(plan->judge, 0xFFFFFFFFu, NULL);
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
    g_pool_tail        = NULL;
    FileMapDestroy(fs->files);
    fs->files = FileMapCreate();

    PoolMountOutcome* out = w->out;
    kfree(out->last_span);
    out->last_span       = NULL;
    out->last_span_count = 0;
    out->unreadable      = 0;
    w->span_cap          = 0;
    w->span_refused      = false;
    w->in_span           = false;
    w->disagreed         = 0;
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
 * medium (meta_pool.h, THE POOL IS WALKED ONCE): the newest borne out; else
 * the other borne out, which is falling back; else a seal whose closed blocks
 * are borne out and whose open block is not, the newest first, which is the
 * rolled-back tail of decision 5; else none. The seal's index, or -1.
 */
static int SealsDecide(const SealTry* t, PoolStood* stood, bool* fell_back)
{
    *fell_back = false;
    *stood     = POOL_STOOD_SEAL;
    if (t[0].mark && t[0].verdict == SEAL_BORNE)
        return 0;
    if (t[1].mark && t[1].verdict == SEAL_BORNE) {
        *fell_back = true;
        return 1;
    }
    *stood = POOL_STOOD_TORN_TAIL;
    for (int i = 0; i < (int)VOLUME_LEDGER_COPIES; i++)
        if (t[i].mark && t[i].verdict == SEAL_TAIL_TORN)
            return i;
    *stood = POOL_STOOD_NO_SEAL;
    return -1;
}

/* After a first walk that did not stand on the newest seal: which walk comes
 * next, said. */
static void WalkChoose(const PoolMountAsk* ask, const SealTry* t, PoolPlan* plan,
                       PoolMountOutcome* out)
{
    const SettleMark* s0 = ask->seal[0];
    const SettleMark* s1 = ask->seal[1];
    PoolStood         stood;
    bool              fell_back;
    int               at = SealsDecide(t, &stood, &fell_back);
    out->stood     = stood;
    out->seal      = at;
    out->fell_back = fell_back;
    if (stood == POOL_STOOD_SEAL) {
        PlanOfSeal(plan, s1, false);
        kprintf("[TagFS] fell back: the newest seal (cycle %llu) names pool records the medium "
                "does not hold — %s — so the volume stands on cycle %llu, the one before it, "
                "and from now on a freed block rests one cycle longer on this medium "
                "(SETTLE_MEDIUM_LIED): the cycle was cut before its flush, or the medium said "
                "a flush was kept that was not\n",
                (unsigned long long)s0->settle,
                t[0].verdict == SEAL_TAIL_TORN ? "its open block does not bear the seal out"
                                               : "its closed blocks do not bear the seal out",
                (unsigned long long)s1->settle);
        return;
    }
    if (stood == POOL_STOOD_TORN_TAIL) {
        const SettleMark* m = ask->seal[at];
        PlanOfSeal(plan, m, true);
        out->torn_bytes = m->pool_to_bytes;
        kprintf("[TagFS] the pool's open block %u does not hold what %s says — it was torn "
                "as it was written again: its %u byte(s) of records are ROLLED BACK to the "
                "last closed block. Every file whose newest record was there stands on the "
                "record before it, a file with none before it is gone, and the volume mounts "
                "writable\n", m->pool_to_block,
                s1 ? "either copy's seal" : "the seal", m->pool_to_bytes);
        return;
    }
    memset(plan, 0, sizeof(*plan));
    plan->start = ask->head;
    kprintf("[TagFS] no copy's seal is borne out by the pool's closed blocks — a kernel "
            "before the seal rewrote records in them in place, or they rotted — so the whole "
            "chain is read as such a kernel reads it: the file table says which files "
            "there are, and nothing is rolled back\n");
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
        tries[0].mark = s0;
        /* A seal the walk from the newest seal's chain_first cannot reach from
         * its own start is not judged by it. */
        tries[1].mark = s1 && s1->chain_first == s0->chain_first ? s1 : NULL;
        PlanOfSeal(&plan, s0, false);
        plan.judge = tries;
        rc = WalkOnce(&w, &plan);
        if ((rc == OK || rc == POOL_WALK_STOPPED) && tries[0].verdict == SEAL_BORNE) {
            /* The common mount: one walk, and it stood on the newest seal. */
            out->stood = POOL_STOOD_SEAL;
            out->seal  = 0;
            rc         = OK;
        } else if (rc == OK || rc == POOL_WALK_STOPPED) {
            WalkForget(&w);
            if (!tagfs_get_state()->files) {
                kfree(w.here);
                return ERR_NO_MEMORY;
            }
            WalkChoose(ask, tries, &plan, out);
            rc = WalkOnce(&w, &plan);
        }
    }
    if (rc != OK) {
        if (!g_pool_tail)
            kfree(w.here);
        meta_pool_shutdown();
        kfree(out->last_span);
        out->last_span       = NULL;
        out->last_span_count = 0;
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

    out->from_block = g_pool_tail_block;
    out->from_bytes = g_pool_tail->used_bytes;
    return OK;
}

#ifdef CONFIG_VOLUME_TESTS
PoolStood PoolSealsJudge(const PoolMountAsk* ask, int* out_seal, bool* out_fell_back)
{
    SealTry        tries[VOLUME_LEDGER_COPIES];
    MetaPoolBlock* blk = kmalloc(sizeof(MetaPoolBlock));
    uint32_t       run = tagfs_get_state()->layout.data_blocks;
    memset(tries, 0, sizeof(tries));
    *out_seal      = -1;
    *out_fell_back = false;
    if (!blk || !ask->seal[0]) {
        kfree(blk);
        return POOL_STOOD_NO_SEAL;
    }
    tries[0].mark = ask->seal[0];
    tries[1].mark = ask->seal[1] && ask->seal[1]->chain_first == ask->seal[0]->chain_first
                        ? ask->seal[1] : NULL;

    uint32_t block = ask->seal[0]->chain_first;
    for (uint32_t hops = 0; hops < run; hops++) {
        bool ok = block < run && tagfs_read_block(block, blk) == OK &&
                  blk->magic == TAGFS_MPOOL_MAGIC;
        WalkJudge(tries, block, ok ? blk : NULL);
        if (!ok || block == ask->seal[0]->pool_to_block)
            break;
        if (blk->next_block == 0) {
            WalkJudge(tries, 0xFFFFFFFFu, NULL);
            break;
        }
        block = blk->next_block;
    }
    kfree(blk);

    PoolStood stood;
    *out_seal = SealsDecide(tries, &stood, out_fell_back);
    return stood;
}
#endif
