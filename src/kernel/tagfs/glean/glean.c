#include "glean.h"
#include "../metadata_pool/meta_pool.h"
#include "../file_map/file_map.h"
#include "../ink/ink.h"
#include "../ink/fallow.h"
#include "../mount/ground_census.h"
#include "../settle/settle.h"
#include "../block_map/block_map.h"   /* BlockMapGivesUnseenLocked — ground on its way */

/*
 * Glean (glean.h): the pass before each cycle's S0, the Deed's head written
 * again once the chain begins past it, and the mount's look at what the head
 * still names. The Settle's holder's alone — one cycle at a time — and the
 * mount's with the door shut.
 */

/* Where a record begins in its block: the header's bytes. */
#define GLEAN_RECORDS_AT  ((uint32_t)__builtin_offsetof(MetaPoolBlock, payload))

/* The head, and what the medium's copy of it names: `s_head_next` its
 * next_block there once the chain begins past it, `s_settled_first` the
 * chain's first block as the last settled seal named it, and `s_head_img` its
 * bytes, none of whose records change once it is off the chain. */
static bool          s_up;
static uint32_t      s_head;
static uint32_t      s_head_next;
static uint32_t      s_settled_first;
static uint64_t      s_covers;           /* GleanHintCovers */
static MetaPoolBlock s_head_img;
static bool          s_head_held;
static bool          s_head_said;        /* a refused write of it, said once an episode */

/* The front block read by one step, for the step after it that moves its
 * records: a closed block is never written again, so a link's image stays
 * true for as long as the link is on the chain. */
static MetaPoolBlock s_front;
static uint32_t      s_front_link;
static bool          s_front_held;
static bool          s_front_said;
static bool          s_drift_said;
static bool          s_unclosed_said;    /* a move with no Spare to close onto, said once an episode */

static volatile uint8_t s_short;
static volatile uint8_t s_sweep_asked;   /* a delete waits on the sweep: the next pass is one */

/* The pass of the cycle in flight. */
static struct {
    bool     short_room;       /* the allocator was short with nothing resting */
    bool     sweep;            /* a pass of the deletes' sweep */
    bool     dig;              /* no Spare could be made: Glean moves on what the pool holds */
    bool     counted;
    uint64_t moved;            /* GleanMovedGen: a record laid again, or a close bought */
    uint32_t moved_into;       /* the link that record lies in */
} s_pass;

/* The deletes' sweep (glean.h), under the state lock. `mark` is the tail's
 * link as it began — once the chain begins past it, every link it found is
 * off — and `from` the chain's blocks then; `idle` the passes in a row that
 * took nothing off and owed no close, `moved` whether the one in flight did.
 * Found to give nothing, it stays so until a block's worth of records has
 * died past `died` (PoolDiedBytes). */
static struct {
    bool     on;
    bool     moved;
    bool     fruitless;
    uint32_t mark;
    uint32_t from;
    uint32_t idle;
    uint64_t died;
} s_sweep;

/* The lowered slack, judged by what it did. `s_short_from`: the chain's length
 * as the last pass at the lowered slack began, 0 once judged. Lifted — a cycle
 * at the lowered slack left the chain no shorter — until a block's worth of
 * records has died past `s_lifted_died` (PoolDiedBytes). */
static uint32_t  s_short_from;
static bool      s_lifted;
static uint64_t  s_lifted_died;

/* The blocks the mount found the head naming before the chain's first, and
 * the node they rest in until the first write of the head names past them. */
static uint32_t*   s_prefix;
static uint32_t    s_prefix_count;
static FallowNode* s_prefix_node;

#ifdef CONFIG_VOLUME_TESTS
static GleanCounts s_counts;
#define GLEAN_COUNT(field, n) (s_counts.field += (n))

void GleanCountsRead(GleanCounts* out)
{
    *out = s_counts;
}
#else
#define GLEAN_COUNT(field, n) ((void)0)
#endif

static TagFSLock* GleanLock(void)
{
    return &tagfs_get_state()->lock;
}

uint64_t GleanHintCovers(void)
{
    return s_covers;
}

uint64_t GleanMovedGen(void)
{
    return s_pass.moved;
}

void GleanShort(void)
{
    __atomic_store_n(&s_short, 1u, __ATOMIC_RELEASE);
}

bool GleanHeadOffChain(void)
{
    return s_up && PoolChainBlocks() && PoolChainFirst() != s_head;
}

/* A lift holds until a block's worth of records has died since it came: till
 * then a pass at the lowered slack would find what it found, records that pack
 * no tighter than they lie. A cycle that closes the tail for a waiter grows
 * the chain without a record dying, and does not undo it. */
static bool GleanLiftedLocked(void)
{
    return s_lifted && PoolDiedBytes() - s_lifted_died < TAGFS_MPOOL_DATA_SIZE;
}

bool GleanMayFree(void)
{
    return s_up && PoolChainBlocks() > PoolLiveBlocks() + 1u && !GleanLiftedLocked();
}

uint32_t GleanStepNeedLocked(uint32_t room, const RecordWhere* killed)
{
    uint32_t  tail = PoolTailLink();
    uint32_t  need = 0;
    PoolFront f;

    if (!s_up)
        return 0;
    for (uint32_t i = 0; PoolChainLink(i, &f); i++) {
        uint32_t live  = f.live;
        bool     gives = f.block != s_head;
        if (killed && killed->len && killed->link == f.link)
            live = live > killed->len ? live - killed->len : 0;
        if (live == 0 && f.replaced_by < tail) {
            if (gives)
                return need;        /* taken off for nothing, and its block comes back */
            continue;
        }
        if (need == 0)
            need = 1;               /* the close: of the block the moved records land in, or the
                                     * one the record that emptied the front waits for */
        if (live > room) {
            need++;                 /* the turn they need first */
            room = TAGFS_MPOOL_DATA_SIZE;
        }
        room -= live;
        if (gives)
            return need;
    }
    return need;
}

bool GleanSweepAsk(void)
{
    TagFSLockTake(GleanLock());
    bool may = s_up && PoolChainBlocks() > PoolLiveBlocks() + 1u &&
               !(s_sweep.fruitless && PoolDiedBytes() - s_sweep.died < TAGFS_MPOOL_DATA_SIZE);
    if (may) {
        s_sweep.fruitless = false;
        if (!s_sweep.on) {
            s_sweep.on    = true;
            s_sweep.moved = false;
            s_sweep.idle  = 0;
            s_sweep.mark  = PoolTailLink();
            s_sweep.from  = PoolChainBlocks();
        }
        __atomic_store_n(&s_sweep_asked, 1u, __ATOMIC_RELEASE);
    }
    TagFSLockDrop(GleanLock());
    if (may)
        SettleDemand(SETTLE_DEMAND_ROOM);
    return may;
}

bool GleanSweepingLocked(void)
{
    return s_sweep.on;
}

// ----------------------------------------------------------------------------
// The pass
// ----------------------------------------------------------------------------

/*
 * The sweep judged at the pass after each of its own (glean.h), under the
 * state lock. It ends once the reserve is whole — the room its deletes waited
 * for came back, and a later stall begins one of its own — or once every link
 * the chain held when it began is off: a chain shorter then gave room, one no
 * shorter gave nothing. And it can do nothing when its passes in a row took
 * nothing off and owed no close while no ground rests, waits unseen or lies
 * free for a Spare: a link closed in one cycle is written by its S3 and may
 * be taken by the next, so two such passes are the judgment.
 */
static void GleanSweepJudgeLocked(bool swept)
{
    if (!s_sweep.on)
        return;
    if (swept)
        s_sweep.idle = s_sweep.moved ? 0u : s_sweep.idle + 1u;
    s_sweep.moved = false;

    PoolFront f;
    uint32_t  first  = PoolChainLink(0, &f) ? f.link : PoolTailLink();
    uint32_t  chain  = PoolChainBlocks();
    bool      around = first > s_sweep.mark;
    bool      stuck  = s_sweep.idle >= 2u && FallowBlocks() == 0 &&
                       !BlockMapGivesUnseenLocked() && tagfs_get_state()->ledger.free_blocks == 0;
    if (PoolSparesHeld(NULL, 0) >= (uint32_t)SPARE_RESERVE || (around && chain < s_sweep.from)) {
        s_sweep.on = false;
        return;
    }
    if (!around && !stuck)
        return;
    if (around)
        kprintf("[Glean] a sweep for the deletes took every block the pool's chain held off "
                "once and left it no shorter (%u block(s), its live records fill %u) — no room "
                "is Glean's to give, and a delete that frees no ground is refused until "
                "records die\n", chain, PoolLiveBlocks());
    else
        kprintf("[Glean] a sweep for the deletes can take no block off — the pool holds %u "
                "Spare(s), its next step needs %u, and no ground rests or lies free to make "
                "more — a delete that frees no ground is refused until records die\n",
                PoolSparesHeld(NULL, 0), GleanStepNeedLocked(PoolTailRoom(), NULL));
    s_sweep.on        = false;
    s_sweep.fruitless = true;
    s_sweep.died      = PoolDiedBytes();
}

/*
 * ‼ THE LOWERED SLACK IS JUDGED BY THE CHAIN IT LEFT. A pass at the lowered
 * slack takes the front off and lays its live records at the tail, and the
 * cycle it runs in may close the tail onto a Spare the allocator then makes
 * again: when records do not pack into fewer blocks than they fill, or every
 * cycle closes the tail for a waiter, the chain comes out of the cycle as long
 * as it went in, and the ground the pass took off is the ground the Spare
 * took. Such a chain's slack is not Glean's to give, and a take short of room
 * must hear that there is none rather than wait on cycles that give nothing.
 * So the cycle after each pass at the lowered slack asks whether the chain is
 * shorter than when that pass began; no — the lowered slack is lifted, said
 * once an episode, until a block's worth of records dies: deleted, or
 * replaced by newer ones, which is what a pass could then take off.
 */
void GleanPassBegin(void)
{
    bool asked = __atomic_exchange_n(&s_short, 0u, __ATOMIC_ACQ_REL) != 0;
    bool sweep = __atomic_exchange_n(&s_sweep_asked, 0u, __ATOMIC_ACQ_REL) != 0;
    bool swept = s_pass.sweep;

    memset(&s_pass, 0, sizeof(s_pass));
    TagFSLockTake(GleanLock());
    GleanSweepJudgeLocked(swept);
    s_pass.sweep = sweep && s_sweep.on;
    uint32_t chain = PoolChainBlocks();
    if (s_short_from && chain >= s_short_from) {
        if (!s_lifted)
            kprintf("[Glean] a cycle at the lowered slack left the pool's chain no shorter (%u "
                    "block(s), its live records fill %u) — the slack is not Glean's to give, and "
                    "a take short of room hears there is none until records die\n", chain,
                    PoolLiveBlocks());
        s_lifted      = true;
        s_lifted_died = PoolDiedBytes();
    } else if (s_short_from) {
        s_lifted = false;
    }
    s_short_from = 0;
    if (asked && s_up && !GleanLiftedLocked()) {
        s_pass.short_room = true;
        s_short_from      = chain ? chain : 1u;
    }
    if (s_pass.sweep)
        s_pass.short_room = true;     /* whatever a lift said to the allocator (glean.h) */
    TagFSLockDrop(GleanLock());
}

/* Whether a chain of `chain` blocks is past its slack over `live`. */
static bool GleanWanted(uint32_t chain, uint32_t live)
{
    if (s_pass.short_room)
        return chain > live + 1u;
    return chain >= GLEAN_SLACK * live + 1u;
}

/* Under the state lock: whether link `f` may leave the chain under the seal of
 * the cycle in flight — every record that took the place of one of its own
 * lies in a block that seal counts closed: a link before the tail, or the tail
 * itself once it holds a record this pass laid again, which this cycle's S0
 * closes (GleanMovedGen). Otherwise the link waits for the tail's next close
 * (glean.h). */
static bool GleanMayTakeLocked(const PoolFront* f)
{
    uint32_t tail = PoolTailLink();
    return f->replaced_by < tail || (s_pass.moved && s_pass.moved_into == tail);
}

typedef struct {
    uint32_t  empties;     /* the links at the front the map names nothing in, free to go */
    bool      move;        /* the link after them is to be read and moved */
    bool      close;       /* ... or, emptied, it waits for the tail's close, which a sweep buys */
    uint32_t  need;        /* the Spares that step takes (GleanStepNeedLocked) */
    PoolFront front;       /* that link */
} GleanLook;

/* Under the state lock. A link the map names nothing in stays so — only the
 * tail takes new records, and Glean is the one who takes links off — and so
 * does the newest link holding what replaced its records: a close of the tail
 * is all that makes it free to go. A front emptied and not yet free is neither
 * taken nor moved, and the pass ends at it — but a sweep's, which buys the
 * close (glean.h). */
static void GleanLookLocked(GleanLook* look)
{
    PoolFront f;
    memset(look, 0, sizeof(*look));
    while (PoolFrontLink(look->empties, &f) && f.live == 0 && GleanMayTakeLocked(&f))
        look->empties++;
    if (!PoolFrontLink(look->empties, &f))
        return;
    look->move  = f.live != 0 && GleanWanted(PoolChainBlocks() - look->empties, PoolLiveBlocks());
    look->close = f.live == 0 && s_pass.sweep;
    look->front = f;
    if (look->move || look->close)
        look->need = GleanStepNeedLocked(PoolTailRoom(), NULL);
}

/* The front block read into s_front: false, said once an episode, when it does
 * not read or is not the block its link says — rot, which the next mount meets
 * too; the chain stays as it is. */
static bool GleanReadFront(const PoolFront* f)
{
    error_t rc = TagFSMetaBlockRead(f->block, &s_front);
    if (rc != OK || !PoolFrontReadsTrue(f, &s_front)) {
        if (!s_front_said)
            kprintf("[Glean] block %u of the pool %s — its records are not gathered, and it "
                    "stays on the chain\n", f->block,
                    rc != OK ? "would not read" : "does not hold what the chain says it does");
        s_front_said = true;
        s_front_held = false;
        return false;
    }
    s_front_said = false;
    s_front_held = true;
    s_front_link = f->link;
    GLEAN_COUNT(reads, 1);
    return true;
}

/* The Spares a step of this pass leaves the pool: the reserve while free ground
 * could make Spares past it, none once the volume has none to give and the
 * pass is a sweep for the deletes — the reserve is theirs and Glean's to make
 * their room with (meta_pool.h), never to spend on a chain merely past its
 * slack, nor on room for a write, which the reserve is not for. */
static uint32_t GleanFloor(void)
{
    return s_pass.dig && s_pass.sweep ? 0u : (uint32_t)SPARE_RESERVE;
}

/* Under the state lock, with the Spares the step needs held (GleanMayStepLocked):
 * the records of the front block that are still their files' newest, laid
 * again at the tail's end. A turn the tail needs on the way leaves the close
 * its Spare. False when the turn could not be had: what moved stands moved,
 * and is closed at S0 like the rest; the block stays on the chain. */
static bool GleanMoveLocked(const PoolFront* f)
{
    FileMap* files = tagfs_get_state()->files;
    uint32_t used  = s_front.used_bytes <= TAGFS_MPOOL_DATA_SIZE ? s_front.used_bytes
                                                                 : TAGFS_MPOOL_DATA_SIZE;
    for (uint32_t pos = 0; pos + RECORD_HEADER_SIZE <= used; ) {
        uint16_t len;
        uint32_t fid;
        memcpy(&len, s_front.payload + pos, sizeof(len));
        if (len < RECORD_HEADER_SIZE || len > used - pos)
            break;
        memcpy(&fid, s_front.payload + pos + 2, sizeof(fid));
        uint16_t at = (uint16_t)(GLEAN_RECORDS_AT + pos);
        if (fid && FileMapRecordAt(files, fid, f->link, at)) {
            if (PoolTailRoom() < len && !PoolTailTurn(GleanFloor() + 1u))
                return false;
            RecordWhere from = { .link = f->link, .offset = at, .len = len };
            InkGleanLocked(fid, &from, s_front.payload + pos);
            s_pass.moved      = InkedGen();
            s_pass.moved_into = PoolTailLink();
            GLEAN_COUNT(moved_records, 1);
            GLEAN_COUNT(moved_bytes, len);
        }
        pos += len;
    }
    return true;
}

/* Under the state lock: up to `count` links off the chain's front — as long as
 * the map names nothing in them and they are free to go (GleanMayTakeLocked) —
 * their blocks into `node`, the head's excepted: the head only says, from its
 * close on, the block after it. The links taken, and in *kept the blocks the
 * node holds. */
static uint32_t GleanTakeLocked(uint32_t count, FallowNode* node, uint32_t* kept)
{
    PoolFront f;
    uint32_t  taken = 0;
    *kept = 0;
    for (; taken < count && PoolFrontLink(taken, &f) && f.live == 0 && GleanMayTakeLocked(&f);
         taken++) {
        if (f.block == s_head)
            s_head_next = f.next;
        else
            *kept = FallowNodeKeep(node, f.block);
    }
    PoolUnlinkFront(taken);
    return taken;
}

/* A step wants the Spares it takes held past the reserve: one made here with
 * no lock held — the step's one command. When the volume has no ground to
 * give, the step stands on what the pool holds, Glean's share among it. True
 * when a command was made. */
static bool GleanSpareFor(uint32_t need)
{
    if (s_pass.dig)
        return false;
    TagFSLockTake(GleanLock());
    bool short_of = PoolSparesHeld(NULL, 0) < GleanFloor() + need;
    TagFSLockDrop(GleanLock());
    if (!short_of)
        return false;
    if (PoolSpareMake() != OK) {
        s_pass.dig = true;
        return false;
    }
    return true;
}

/* Under the state lock: whether the step at the front may be taken now — the
 * Spares it needs are held (glean.h); said once an episode when not. The
 * episode is the volume's with no ground to make Spares from: a step it
 * affords now and then between two it does not is not its end. */
static bool GleanMayStepLocked(const PoolFront* f, uint32_t need)
{
    uint32_t held = PoolSparesHeld(NULL, 0);
    if (held >= GleanFloor() + need) {
        if (!s_pass.dig)
            s_unclosed_said = false;
        return true;
    }
    if (!s_unclosed_said)
        kprintf("[Glean] block %u of the pool is not gathered: its step needs %u Spare(s) past "
                "the %u this pass leaves the pool, which holds %u and could make no more — it "
                "stays on the chain until ground comes back\n", f->block, need, GleanFloor(),
                held);
    s_unclosed_said = true;
    return false;
}

/* A sweep's step at an emptied front: the close the record that emptied it
 * waits for, bought — this cycle's S0 closes the tail (GleanMovedGen) — so
 * the front is free to go in this very pass. False when its Spare is not
 * held, or the tail holds nothing a close would keep. */
static bool GleanBuyCloseLocked(const GleanLook* look)
{
    uint64_t newest = PoolTailGen();
    if (!newest || !GleanMayStepLocked(&look->front, look->need))
        return false;
    s_pass.moved      = newest;
    s_pass.moved_into = PoolTailLink();
    s_sweep.moved     = true;
    return true;
}

void GleanStep(bool* done)
{
    GleanLook look;
    *done = true;
    if (!s_up || TagFSMediumLeft() || TagFSReadOnly())
        return;

    TagFSLockTake(GleanLock());
    GleanLookLocked(&look);
    TagFSLockDrop(GleanLock());
    if (!look.empties && !look.move && !look.close)
        return;

    /* The links the map names nothing in go first, in a step of their own;
     * a block to move is read by the step before the one that moves it. */
    bool moving  = look.move && look.empties == 0;
    bool closing = look.close && look.empties == 0;
    if (moving && !(s_front_held && s_front_link == look.front.link)) {
        *done = !GleanReadFront(&look.front);
        return;
    }
    if ((moving || closing) && GleanSpareFor(look.need)) {
        *done = false;
        return;
    }
    if (closing) {
        TagFSLockTake(GleanLock());
        *done = !GleanBuyCloseLocked(&look);
        TagFSLockDrop(GleanLock());
        return;
    }

    uint32_t    want = moving ? 1u : look.empties;
    FallowNode* node = FallowNodeRoom(want, FALLOW_BY_UNLINK);
    if (!node)
        return;     /* no memory for the node: the chain stays as it is, for the next pass */

    uint32_t kept = 0, taken = 0;
    TagFSLockTake(GleanLock());
    bool may   = !moving || GleanMayStepLocked(&look.front, look.need);
    bool moved = may && (!moving || GleanMoveLocked(&look.front));
    if (moved)
        taken = GleanTakeLocked(want, node, &kept);
    if (taken)
        s_sweep.moved = true;
    if (kept)
        FallowAdd(node, 0);
    TagFSLockDrop(GleanLock());
    if (!kept)
        FallowNodeLetGo(node);

    if (moving && moved && !taken && !s_drift_said) {
        s_drift_said = true;
        kprintf("[Glean] block %u of the pool still counts records of live files after they "
                "were laid again — it stays on the chain, and nothing of it is let go\n",
                look.front.block);
    }
    if (moving && taken)
        s_front_held = false;
    if (taken) {
        GLEAN_COUNT(unlinked, taken);
        if (!s_pass.counted)
            GLEAN_COUNT(passes, 1);
        s_pass.counted = true;
    }
    *done = taken == 0;
}

// ----------------------------------------------------------------------------
// The Deed's head
// ----------------------------------------------------------------------------

/* What the write names past is named by nothing on the medium from this
 * cycle's S1 on: the blocks taken off up to the last settled cycle, whose seal
 * began the chain at `to`, and the ones the mount found the head naming. */
void GleanHeadTake(GleanHead* h)
{
    h->write   = s_up && s_settled_first != s_head && s_head_next != s_settled_first;
    h->written = false;
    h->to      = s_settled_first;
    if (h->write) {
        FallowUnlinkedUnnamed(SettledCycles(), s_prefix_node);
        s_prefix_node = NULL;
    }
}

/* Its records are left as they are: no seal has counted them since the chain
 * began past it, and every one of them that was a file's newest lies again
 * further on. One that does not read, or is not a block of the pool, is
 * written anew holding none — what it names is all it is for. */
void GleanHeadStep(GleanHead* h, bool* done)
{
    *done = true;
    if (!h->write)
        return;
    if (!s_head_held) {
        error_t rc = TagFSMetaBlockRead(s_head, &s_head_img);
        if (rc != OK || s_head_img.magic != TAGFS_MPOOL_MAGIC) {
            kprintf("[Glean] the pool's head, block %u, %s — it is written anew holding no "
                    "records, naming block %u, where the chain begins\n", s_head,
                    rc != OK ? "would not read" : "holds no block of the pool", h->to);
            memset(&s_head_img, 0, sizeof(s_head_img));
            s_head_img.magic = TAGFS_MPOOL_MAGIC;
        }
        s_head_held = true;
        *done       = false;
        return;
    }
    s_head_img.next_block = h->to;
    error_t rc = TagFSMetaBlockWrite(s_head, &s_head_img);
    if (rc != OK) {
        if (!s_head_said)
            kprintf("[Glean] the pool's head, block %u, would not be written to name block %u "
                    "(error %u) — the blocks taken off before it stay taken, and the next "
                    "cycle writes it again\n", s_head, h->to, (unsigned)rc);
        s_head_said = true;
        return;
    }
    s_head_said = false;
    h->written  = true;
}

void GleanHeadSettled(const GleanHead* h, uint64_t cycle, uint32_t first)
{
    if (h->write && h->written) {
        s_head_next = h->to;
        GLEAN_COUNT(head_writes, 1);
    }
    s_settled_first = first;
    if (first == s_head || s_head_next == first)
        s_covers = cycle;
    else if (h->write && h->written)
        s_covers = cycle - 1u;      /* it names the first block of the seal before this one */
}

// ----------------------------------------------------------------------------
// The mount
// ----------------------------------------------------------------------------

/* Why block `b`, met walking from the head towards the chain's first block,
 * is not one the head holds before the chain; NULL when it is — read into
 * s_front. */
static const char* GleanPrefixRefuses(uint32_t b, uint32_t run)
{
    if (b >= run)
        return "lies outside the data run";
    if (b == s_head || PoolChainHolds(b))
        return "is a block of the chain";
    if (TagFSMetaBlockRead(b, &s_front) != OK)
        return "would not read";
    if (s_front.magic != TAGFS_MPOOL_MAGIC)
        return "holds no block of the pool";
    return NULL;
}

/* One block more in the prefix: false, said, when there is no memory for it. */
static bool GleanPrefixKeep(uint32_t b, uint32_t* cap)
{
    if (s_prefix_count == *cap) {
        uint32_t  grown = *cap ? *cap * 2u : 8u;
        uint32_t* more  = kmalloc(sizeof(uint32_t) * grown);
        if (!more) {
            kprintf("[Glean] no memory to hold the blocks the pool's head names before the "
                    "chain past the first %u — they stay taken, for a later mount's census\n",
                    s_prefix_count);
            return false;
        }
        if (s_prefix_count)
            memcpy(more, s_prefix, sizeof(uint32_t) * s_prefix_count);
        kfree(s_prefix);
        s_prefix = more;
        *cap     = grown;
    }
    s_prefix[s_prefix_count++] = b;
    return true;
}

static bool GleanPrefixHolds(uint32_t b)
{
    for (uint32_t i = 0; i < s_prefix_count; i++)
        if (s_prefix[i] == b)
            return true;
    return false;
}

/* What the head names, followed up to the chain's first block: blocks of the
 * pool no seal holds any more and the head still does. A block of the chain
 * met on the way is not one of them — a head naming past the chain's first
 * block is said, and the walk stops there; so is a link that names no block,
 * and one that names a block the walk met already, which would take it round
 * for ever. Every block walked is a distinct block of the data run. */
static void GleanWalkPrefix(uint32_t first)
{
    uint32_t run = tagfs_get_state()->layout.data_blocks;
    uint32_t cap = 0;
    uint32_t b   = s_head_next;
    while (b != first) {
        if (b == 0) {
            kprintf("[Glean] the pool's head names blocks before the chain's first, block %u, "
                    "and the walk from it ends at a link that names no block, short of the "
                    "chain — the head is written again to name the chain\n", first);
            return;
        }
        const char* why = GleanPrefixHolds(b) ? "is met a second time: the walk goes round"
                                              : GleanPrefixRefuses(b, run);
        if (why) {
            kprintf("[Glean] the pool's head names blocks before the chain's first, block %u, "
                    "and block %u of them %s — the head is written again to name the chain\n",
                    first, b, why);
            return;
        }
        if (!GleanPrefixKeep(b, &cap))
            return;
        b = s_front.next_block;
    }
}

void GleanMount(uint32_t head, uint32_t first)
{
    kfree(s_prefix);
    s_prefix        = NULL;
    s_prefix_count  = 0;
    s_prefix_node   = NULL;
    s_short_from    = 0;
    s_lifted        = false;
    s_up            = true;
    s_head          = head;
    s_head_next     = first;
    s_settled_first = first;
    s_covers        = 0;
    s_head_held     = false;
    s_head_said     = false;
    s_front_held    = false;
    s_front_said    = false;
    s_drift_said    = false;
    s_unclosed_said = false;
    memset(&s_sweep, 0, sizeof(s_sweep));
    __atomic_store_n(&s_short, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&s_sweep_asked, 0u, __ATOMIC_RELEASE);
#ifdef CONFIG_VOLUME_TESTS
    memset(&s_counts, 0, sizeof(s_counts));
#endif
    if (first == head)
        return;

    /* The chain begins past the head: its next_block is what the medium says,
     * and the head is written to name `first` unless it does already. */
    error_t rc = TagFSMetaBlockRead(head, &s_head_img);
    if (rc != OK || s_head_img.magic != TAGFS_MPOOL_MAGIC) {
        kprintf("[Glean] the pool's head, block %u, %s — the chain begins at block %u by its "
                "seal, and the head is written anew to name it\n", head,
                rc != OK ? "would not read" : "holds no block of the pool", first);
        s_head_next = head;         /* names nothing the chain has: owed */
        return;
    }
    s_head_held = true;
    s_head_next = s_head_img.next_block;
    GleanWalkPrefix(first);
}

void GleanClaimGround(void)
{
    uint32_t count = s_prefix_count;
    if (!count)
        return;
    for (uint32_t i = 0; i < count; i++)
        GroundClaim(GROUND_POOL, s_prefix[i], 1, 0);

    FallowNode* node = FallowNodeRoom(count, FALLOW_BY_UNLINK);
    if (node) {
        TagFSLockTake(GleanLock());
        for (uint32_t i = 0; i < count; i++)
            (void)FallowNodeKeep(node, s_prefix[i]);
        FallowAdd(node, 0);
        s_prefix_node = node;
        TagFSLockDrop(GleanLock());
        kprintf("[Glean] %u block(s) the pool's head still names lie before the chain's first "
                "— they rest until the head is written to name past them\n", count);
    } else {
        kprintf("[Glean] no memory to rest the %u block(s) the pool's head names before the "
                "chain — they stay taken, for a later mount's census\n", count);
    }
    kfree(s_prefix);
    s_prefix       = NULL;
    s_prefix_count = 0;
}

void GleanUnmount(void)
{
    kfree(s_prefix);
    s_prefix       = NULL;
    s_prefix_count = 0;
    s_prefix_node  = NULL;
    s_up           = false;
    s_head_held    = false;
    s_front_held   = false;
    memset(&s_sweep, 0, sizeof(s_sweep));
    __atomic_store_n(&s_sweep_asked, 0u, __ATOMIC_RELEASE);
}
