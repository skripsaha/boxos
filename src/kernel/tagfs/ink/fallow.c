#include "fallow.h"
#include "commonhold.h"
#include "../helm/helm.h"
#include "../settle/settle.h"
#include "../settle/sewing.h"
#include "../file_map/file_record.h"
#include "../block_map/block_map_derived.h"
#include "../integrity/gloss.h"
#include "../settle/binder.h"

struct FallowNode {
    FallowNode* next;
    uint64_t    gen;        /* the freer: an Ink's gen, or a cycle's number */
    uint64_t    ripe_at;    /* the cycle it goes back at; 0 while its freer is unsettled */
    FallowKind  kind;
    uint64_t    fid;        /* the file a record freer freed it from; 0 for the others */
    uint32_t    count;
    uint32_t    room;       /* the runs runs[] has room for */
    uint64_t    stitch;     /* the seq of the stitch that names its freer, once it settled */
    bool        left;       /* the ribbon has left the Quire that stitch lies in */
    uint32_t    bar;        /* runs[count, count + bar) given back and kept from the ribbon */
    Strip       runs[];
};

/* Oldest first: nodes are added in the order their freers were made. */
static FallowNode* s_head;
static FallowNode* s_tail;
static FallowNode* s_barred;          /* given back whole, its ground kept from the ribbon (FallowBarsRun) */
static uint64_t    s_bar_slice = UINT64_MAX;   /* the stitch the oldest Tally a mount may stand on took in */
static uint64_t    s_blocks;

/* A cycle that could not give back all that ripened, for want of the derived
 * map's pages: said once, until a cycle gives back all of it. */
static bool        s_held_said;

static uint64_t NodeBlocks(const FallowNode* n)
{
    uint64_t blocks = 0;
    for (uint32_t i = 0; i < n->count; i++)
        blocks += n->runs[i].count;
    return blocks;
}

/* A node with room for `room` runs, none of them in it yet. */
static FallowNode* NodeAlloc(uint32_t room, FallowKind kind, uint64_t fid)
{
    FallowNode* n = kmalloc(sizeof(FallowNode) + sizeof(Strip) * room);
    if (!n)
        return NULL;
    n->next      = NULL;
    n->gen       = 0;
    n->ripe_at   = 0;
    n->kind      = kind;
    n->fid       = fid;
    n->count     = 0;
    n->room      = room;
    n->stitch    = 0;
    n->left      = false;
    n->bar       = 0;
    return n;
}

/* The room a node of `ground` runs of `runs` (`count`) takes once Commonhold
 * cuts them, that cut's room asked of Commonhold: 0 with no memory — or for a
 * room past what a node counts, said. */
static uint32_t NodeRoomCut(const Strip* runs, uint64_t count, uint64_t ground, uint64_t fid)
{
    uint64_t pieces = 0, splits = 0;
    CommonholdCuts(runs, count, &pieces, &splits);
    if (ground + pieces > UINT32_MAX) {
        kprintf("[TagFS] file %llu: its %llu runs of ground are more than a node of Fallow counts — the change "
                "that gives them up is refused\n", (unsigned long long)fid, (unsigned long long)(ground + pieces));
        return 0;
    }
    return CommonholdRoomFor(splits) == OK ? (uint32_t)(ground + pieces) : 0;
}

/* Only ground comes to rest: a hole (STRIP_HOLE) names no block, and a node
 * that kept one would give the allocator whatever its start reads as. */
FallowNode* FallowNodeMake(const Strip* runs, uint32_t count, FallowKind kind, uint64_t fid)
{
    uint32_t ground = 0;
    for (uint32_t i = 0; i < count; i++)
        ground += StripOnGround(runs[i].kind);
    uint32_t    room = ground ? NodeRoomCut(runs, count, ground, fid) : 0;
    FallowNode* n    = ground && !room ? NULL : NodeAlloc(room, kind, fid);
    for (uint32_t i = 0; n && i < count; i++)
        if (StripOnGround(runs[i].kind))
            n->runs[n->count++] = runs[i];
    return n;
}

error_t FallowNodeOfRecord(const FileRecord* rec, FallowKind kind, FallowNode** out)
{
    StripWalk   walk;
    Strip       run;
    uint64_t    ground = 0;
    FallowNode* n      = NULL;

    *out = NULL;
    StripWalkBegin(rec, &walk);
    while (StripWalkNextGround(&walk, &run))
        ground++;
    if (ground == 0)
        return OK;
    if (ground > UINT32_MAX) {
        kprintf("[TagFS] file %llu: its %llu runs of ground are more than a node of Fallow "
                "counts — the change that gives them up is refused\n", rec->file_id,
                (unsigned long long)ground);
        return ERR_NO_MEMORY;
    }
    if (!(n = NodeAlloc((uint32_t)ground, kind, rec->file_id)))
        return ERR_NO_MEMORY;
    StripWalkBegin(rec, &walk);
    while (n->count < n->room && StripWalkNextGround(&walk, &run))
        n->runs[n->count++] = run;
    /* Laid again with the room Commonhold's cut takes only where its blocks
     * meet a block more than one names. */
    if (!FallowNodeCommonRoom(n)) {
        FallowNode* cut = FallowNodeMake(n->runs, n->count, kind, rec->file_id);
        kfree(n);
        n = cut;
    }
    *out = n;
    return n ? OK : ERR_NO_MEMORY;
}

/* Under the state lock: whether the node and Commonhold have the room the cut
 * of the node's ground takes as Commonhold stands now. */
bool FallowNodeCommonRoom(const FallowNode* node)
{
    uint64_t pieces = 0, splits = 0;
    CommonholdCuts(node->runs, node->count, &pieces, &splits);
    return node->count + pieces <= node->room && CommonholdRoomHolds(splits);
}

/* A block the change lets go, after the node's last run — joined to it where
 * it goes on from it. */
static void NodeFreed(void* ctx, uint64_t start, uint64_t count)
{
    FallowNode* n    = ctx;
    Strip*      last = n->count ? &n->runs[n->count - 1u] : NULL;
    if (last && last->start + last->count == start && last->count + count <= UINT32_MAX)
        last->count += (uint32_t)count;
    else
        n->runs[n->count++] = (Strip){ .start = start, .count = (uint32_t)count, .kind = STRIP_DATA };
}

/* The node's runs moved to the end of its room and read from there, the
 * blocks they let go written from its first: the room held
 * (FallowNodeCommonRoom) is the most the cut adds, so the runs written never
 * reach a run not yet read. */
uint32_t FallowNodeCommonheld(FallowNode* node, uint64_t gen)
{
    uint32_t in = node->count, tail = node->room - in;
    memmove(&node->runs[tail], &node->runs[0], sizeof(Strip) * in);
    node->count = 0;
    for (uint32_t i = 0; i < in; i++) {
        Strip run = node->runs[tail + i];
        CommonholdLetGoLocked(run.start, run.count, run.kind, gen, NodeFreed, node);
    }
    return node->count;
}

void FallowNodeLetGo(FallowNode* node)
{
    kfree(node);
}

#ifdef CONFIG_VOLUME_TESTS
uint64_t FallowNodeBlocks(const FallowNode* node)
{
    return NodeBlocks(node);
}
#endif

STATIC_ASSERT(FALLOW_LIED_LAG > FALLOW_LAG, "a medium that lied waits longer than one that keeps its FLUSH");

/* A medium is shown to lie by its ribbon (RibbonCursorLied, §16.2 п.10). */
uint64_t FallowLag(void)
{
    return RibbonCursorLied() ? FALLOW_LIED_LAG : FALLOW_LAG;
}

void FallowAdd(FallowNode* node, uint64_t gen)
{
    node->gen  = gen;
    node->next = NULL;
    if (s_tail)
        __atomic_store_n(&s_tail->next, node, __ATOMIC_RELEASE);
    else
        s_head = node;
    s_tail = node;
    s_blocks += NodeBlocks(node);
}

/*
 * ‼ GROUND A TALLY STILL NAMES GOES TO DATA, NOT TO THE RIBBON (§3.7's table).
 * A reader takes the ribbon's step into a next_quire only clear of the files
 * of the Tally the mount stands on (ribbon_collate.c RibbonNamed,
 * stitch_ribbon.py Named): it weighs the next_quire before the stitches past
 * that Tally are replayed, so a file the Tally holds that a stitch gave up
 * since still lies on its ground for it. Given to data, such ground is
 * nothing to the reader — the replay gives the old owner's up before the new
 * one's takes it; given to the ribbon, the reader refuses the step there, and
 * every stitch past it is lost to the next mount. So a record's freer that a
 * Tally a mount may stand on did not take in — its stitch past that Tally's
 * slice — keeps the ground it gives back from the ribbon (FallowBarsRun), the
 * node held for its runs until a Tally past the stitch stands where a mount
 * would (FallowBarLift). A freer of no record — a slot's Tally or Quire let
 * go — names no file's ground. Nothing on the medium changes.
 */
static bool NodeBarred(const FallowNode* n)
{
    return n->kind == FALLOW_BY_RECORD && n->stitch > s_bar_slice;
}

static bool RunsBar(const FallowNode* n, uint64_t start, uint64_t end, uint64_t* past)
{
    bool met = false;
    for (uint32_t i = n->count; i < n->count + n->bar; i++) {
        uint64_t s = n->runs[i].start, e = s + n->runs[i].count;
        if (s < end && start < e) {
            met   = true;
            *past = e > *past ? e : *past;
        }
    }
    return met;
}

#ifdef CONFIG_VOLUME_TESTS
static volatile uint64_t s_stand_bar;

void FallowStandBar(uint64_t stitch)
{
    __atomic_store_n(&s_stand_bar, stitch, __ATOMIC_RELEASE);
}
#endif

bool FallowBarsRun(uint64_t start, uint64_t count, uint64_t* past)
{
    bool met = false;
    *past    = start;
#ifdef CONFIG_VOLUME_TESTS
    if (__atomic_load_n(&s_stand_bar, __ATOMIC_ACQUIRE) > s_bar_slice) {
        *past = start + count;
        return true;
    }
#endif
    for (FallowNode* n = s_head; n; n = n->next)
        met = RunsBar(n, start, start + count, past) || met;
    for (FallowNode* n = s_barred; n; n = n->next)
        met = RunsBar(n, start, start + count, past) || met;
    return met;
}

FallowNode* FallowBarLift(uint64_t slice)
{
    FallowNode*  spent = NULL;
    FallowNode** at    = &s_barred;
    s_bar_slice        = slice;
    for (FallowNode* n = s_head; n; n = n->next)
        if (!NodeBarred(n))
            n->bar = 0;
    while (*at) {
        FallowNode* n = *at;
        if (NodeBarred(n)) {
            at = &n->next;
            continue;
        }
        *at     = n->next;
        n->next = spent;
        spent   = n;
    }
    return spent;
}

/*
 * The node's runs back to the allocator, through the pages held for the
 * derived map (FallowGiveRoomHold): a run that room does not cover stays in
 * the node, which rests on, and the next cycle gives it back. A run the map
 * refuses for any other reason — a block of it already free, or past the data
 * run — is said by the map, and no cycle could give it back: it leaves the
 * node. A run given back takes the digests the Gloss holds for it out with it
 * (§8.1). The blocks given back.
 */
static uint64_t NodeGiveBack(FallowNode* n)
{
    uint64_t given = 0;
    uint32_t kept  = 0, count = n->count;
    bool     bar   = NodeBarred(n);

    for (uint32_t i = 0; i < count; i++) {
        Strip    run  = n->runs[i];
        uint64_t met  = 0;
        error_t  back = BlockMapDerivedRelease(n->fid ? GROUND_FILE : GROUND_FALLOW, n->fid, run.start,
                                               run.count, &met);
        if (back == OK) {
            GlossForget(run.start, run.count);    /* its next owner is held to its own digest */
            BirthsForget(run.start, run.count);   /* ... and born where its own stitch puts it */
        }
        if (back == OK)
            given += run.count - met;
        if (back == ERR_NO_MEMORY) {
            n->runs[i]      = n->runs[kept];
            n->runs[kept++] = run;
        }
    }
    /* The runs given back lie in [kept, count), beside those an earlier give
     * kept from the ribbon at [count, count + bar): one stretch while the bar
     * holds, none once it is lifted. */
    n->bar   = bar ? n->bar + (count - kept) : 0;
    n->count = kept;
    return given;
}

/* Whether the rule holds it at the end of the `cycle`-th settled stitch
 * (§3.4): until a stitch named its freer, until the ribbon left the Quire that
 * stitch lies in, until FallowLag() stitches settled after it, and while an
 * open handle views the file as it was before (3). No digest is zeroed for it:
 * the digest lives in the record, and a block's next owner brings its own (5). */
static bool NodeHeld(const FallowNode* n, uint64_t cycle)
{
    return n->ripe_at == 0 || !n->left || cycle < n->ripe_at || n->gen > HelmViewFloor(n->fid);
}

uint64_t FallowRipen(uint64_t cycle, FallowNode** spent)
{
    uint64_t    given  = 0;
    uint64_t    stayed = 0;          /* ripe, and resting on for want of the map's room */
    FallowNode* prev   = NULL;

    *spent = NULL;
    for (FallowNode* n = s_head; n; ) {
        FallowNode* next = n->next;
        if (NodeHeld(n, cycle)) {
            prev = n;
            n    = next;
            continue;
        }

        uint64_t blocks = NodeBlocks(n);
        given    += NodeGiveBack(n);
        s_blocks -= blocks - NodeBlocks(n);
        if (n->count) {
            stayed += NodeBlocks(n);
            prev    = n;
            n       = next;
            continue;
        }

        if (prev)
            prev->next = next;
        else
            s_head = next;
        if (s_tail == n)
            s_tail = prev;
        if (n->bar) {
            n->next  = s_barred;
            s_barred = n;
        } else {
            n->next = *spent;
            *spent  = n;
        }
        n = next;
    }
    if (stayed && !s_held_said)
        kprintf("[TagFS] Fallow: %llu block(s) rested their cycles and the block map had no "
                "room to write their gives down — they rest on, and a later cycle gives them "
                "back\n", (unsigned long long)stayed);
    s_held_said = stayed != 0;
    return given;
}

// ----------------------------------------------------------------------------
// The freers: the stitch that names them, and the Quire it lies in
// ----------------------------------------------------------------------------

void FallowStitchSettled(uint64_t target, uint64_t seq, uint64_t cycle)
{
    uint64_t lag = FallowLag();
    for (FallowNode* n = s_head; n; n = n->next) {
        if (n->ripe_at == 0 && n->gen <= target) {
            n->stitch  = seq;
            n->ripe_at = cycle + lag;
        }
    }
}

void FallowQuireLeft(uint64_t seq)
{
    for (FallowNode* n = s_head; n; n = n->next)
        if (n->ripe_at != 0 && n->stitch < seq)
            n->left = true;
}

void FallowSpineAdd(FallowNode* node, uint64_t cycle)
{
    node->ripe_at = cycle + FallowLag();
    node->left    = true;
    node->stitch  = 0;
    FallowAdd(node, 0);
}

uint64_t FallowRestsPast(uint64_t block)
{
    for (FallowNode* n = s_head; n; n = n->next)
        for (uint32_t i = 0; n->ripe_at != 0 && i < n->count; i++)
            if (block >= n->runs[i].start && block - n->runs[i].start < n->runs[i].count)
                return n->runs[i].start + n->runs[i].count;
    return block;
}

uint64_t FallowSpineRipeFrom(void)
{
    uint64_t from = UINT64_MAX;
    for (FallowNode* n = s_head; n; n = n->next)
        if (n->kind == FALLOW_BY_SPINE && n->ripe_at < from)
            from = n->ripe_at;
    return from;
}

bool FallowAwaitsQuire(void)
{
    for (FallowNode* n = s_head; n; n = n->next)
        if (n->ripe_at != 0 && !n->left)
            return true;
    return false;
}

uint64_t FallowLeftBlocks(void)
{
    uint64_t blocks = 0;
    for (FallowNode* n = s_head; n; n = n->next)
        if (n->left && n->gen <= HelmViewFloor(n->fid))
            blocks += NodeBlocks(n);
    return blocks;
}

/* A run leaves a window mixed at its two ends at the most: a page for each,
 * where it reaches into two windows or more. */
static uint64_t NodeEdges(const FallowNode* n)
{
    uint64_t edges = 0;
    for (uint32_t i = 0; i < n->count; i++) {
        uint64_t a = n->runs[i].start / BLOCK_MAP_WINDOW_BLOCKS;
        uint64_t z = (n->runs[i].start + n->runs[i].count - 1u) / BLOCK_MAP_WINDOW_BLOCKS;
        edges += a == z ? 1u : 2u;
    }
    return edges;
}

/* A node a stitch named that may go back at this cycle's end is one whose lag
 * this cycle's stitch completes — its Quire left before, or left by this very
 * stitch, which TAKE does not know yet: both are held for. */
void FallowGiveRoomHold(uint64_t cycle)
{
    TagFSLock* lock  = &tagfs_get_state()->lock;
    uint64_t   pages = 0;

    TagFSLockTake(lock);
    for (FallowNode* n = s_head; n; n = n->next)
        if (n->ripe_at != 0 && n->ripe_at <= cycle)
            pages += NodeEdges(n);
    TagFSLockDrop(lock);
    /* Refused, it is said by the gives it leaves resting (FallowRipen). */
    (void)BlockMapDerivedHold(pages < UINT32_MAX ? (uint32_t)pages : UINT32_MAX);
}

void FallowNodesFree(FallowNode* list)
{
    while (list) {
        FallowNode* next = list->next;
        kfree(list);
        list = next;
    }
}

uint64_t FallowBlocks(void)
{
    return s_blocks;
}

void FallowDropAll(void)
{
    FallowNodesFree(s_head);
    FallowNodesFree(s_barred);
    s_barred    = NULL;
    s_head      = NULL;
    s_tail      = NULL;
    s_blocks    = 0;
    s_held_said = false;
}
