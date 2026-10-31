#include "fallow.h"
#include "../integrity/integrity.h"
#include "../helm/helm.h"
#include "../settle/settle.h"
#include "../settle/sewing.h"
#include "../use_memory/use_memory.h"
#include "../glean/glean.h"
#include "../file_map/file_record.h"
#include "../block_map/block_map_derived.h"
#include "../integrity/gloss.h"
#include "../settle/binder.h"

struct FallowNode {
    FallowNode* next;
    uint64_t    gen;        /* the freer: an Ink's gen, or a cycle's number */
    uint64_t    ripe_at;    /* the cycle it goes back at; 0 while its freer is unsettled */
    uint64_t    zeroed_in;  /* the cycle that zeroed its digests; 0 while none has */
    FallowKind  kind;
    uint64_t    fid;        /* the file a record freer freed it from; 0 for the others */
    uint32_t    count;
    uint32_t    room;       /* the runs runs[] has room for */
    bool        unnamed;    /* nothing on the medium names it: the map's images leave it free */
    uint64_t    stitch;     /* a book's: the seq of the stitch that names its freer, once it settled */
    bool        left;       /* a book's: the ribbon has left the Quire that stitch lies in */
    uint32_t    bar;        /* a book's: runs[count, count + bar) given back and kept from the ribbon */
    Strip       runs[];
};

/* Oldest first: nodes are added in the order their freers were made. */
static FallowNode* s_head;
static FallowNode* s_tail;
static FallowNode* s_barred;          /* given back whole, its ground kept from the ribbon (FallowBarsRun) */
static uint64_t    s_bar_slice = UINT64_MAX;   /* the stitch the oldest Tally a mount may stand on took in */
static uint64_t    s_blocks;
static uint32_t    s_unnamed;        /* nodes nothing on the medium names */

/* A cycle that could not give back all that ripened, for want of room for the
 * map's deltas: said once, until a cycle gives back all of it. */
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
    n->zeroed_in = 0;
    n->kind      = kind;
    n->fid       = fid;
    n->count     = 0;
    n->room      = room;
    n->unnamed   = false;
    n->stitch    = 0;
    n->left      = false;
    n->bar       = 0;
    return n;
}

/* Only ground comes to rest: a hole (STRIP_HOLE) names no block, and a node
 * that kept one would give the allocator whatever its start reads as. */
FallowNode* FallowNodeMake(const Strip* runs, uint32_t count, FallowKind kind, uint64_t fid)
{
    uint32_t ground = 0;
    for (uint32_t i = 0; i < count; i++)
        ground += runs[i].kind == STRIP_DATA;
    FallowNode* n = NodeAlloc(ground, kind, fid);
    for (uint32_t i = 0; n && i < count; i++)
        if (runs[i].kind == STRIP_DATA)
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
    *out = n;
    return OK;
}

FallowNode* FallowNodeRoom(uint32_t blocks, FallowKind kind)
{
    return NodeAlloc(blocks, kind, 0);
}

/* At most `room` blocks are kept, each one block: the count fits the u32 it
 * is handed back in. */
uint32_t FallowNodeKeep(FallowNode* node, uint64_t block)
{
    Strip* last = node->count ? &node->runs[node->count - 1u] : NULL;
    if (last && last->start + last->count == block && last->count < UINT32_MAX) {
        last->count++;
    } else if (node->count < node->room) {
        node->runs[node->count] = (Strip){ .start = block, .count = 1, .kind = STRIP_DATA };
        node->count++;
    }
    return (uint32_t)NodeBlocks(node);
}

void FallowNodeLetGo(FallowNode* node)
{
    kfree(node);
}

STATIC_ASSERT(FALLOW_LIED_LAG > VOLUME_LEDGER_COPIES - 1u,
              "a medium that lied waits longer than one that keeps its FLUSH");

/* A book's medium is shown to lie by its ribbon (RibbonCursorLied, §16.2
 * п.10), format v1's by its seals (SettleMediumLied): each format asks its own,
 * so a book never lags by a word a format-v1 volume mounted before it left. */
uint64_t FallowLag(void)
{
    bool lied = tagfs_get_state()->format == DEED_FORMAT_V2 ? RibbonCursorLied() : SettleMediumLied();
    return lied ? FALLOW_LIED_LAG : (uint64_t)VOLUME_LEDGER_COPIES - 1u;
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

/* Whether the node may go back at the end of cycle `cycle`, as far as its
 * freer's cycle tells — the rest of FallowRipen's rule only holds it longer. A
 * record freer not yet given its cycle is given one past `cycle` at the
 * earliest. */
static bool NodeMayRipen(const FallowNode* n, uint64_t cycle, uint64_t lag)
{
    uint64_t at = n->ripe_at;
    if (at == 0 && n->kind != FALLOW_BY_RECORD && n->gen != 0)
        at = n->gen + lag;
    return at != 0 && at <= cycle;
}

/* The most deltas the map can need to write the node's gives down: one for
 * every window each of its runs reaches into. */
static uint64_t NodeWindows(const FallowNode* n)
{
    uint64_t windows = 0;
    for (uint32_t i = 0; i < n->count; i++) {
        uint64_t a = n->runs[i].start;
        uint64_t z = a + n->runs[i].count - 1u;
        windows += z / BLOCK_MAP_WINDOW_BLOCKS - a / BLOCK_MAP_WINDOW_BLOCKS + 1u;
    }
    return windows;
}

/* The walk reads nodes with the lock let go: a node is freed only by
 * FallowRipen, which the Settle's holder calls — this caller — and a node added
 * meanwhile hangs past `last`, where the walk stops; nor can such a node go
 * back at this cycle's end, which is why the room counted for the ones that
 * may is all of it. */
void FallowZeroDigests(uint64_t cycle)
{
    TagFSLock* lock = &tagfs_get_state()->lock;

    TagFSLockTake(lock);
    FallowNode* first = s_head;
    FallowNode* last  = s_tail;
    TagFSLockDrop(lock);

    for (FallowNode* n = first; n; n = __atomic_load_n(&n->next, __ATOMIC_ACQUIRE)) {
        if (!n->zeroed_in)
            for (uint32_t i = 0; i < n->count; i++)
                IntegrityForget(n->runs[i].start, n->runs[i].count);
        if (n == last)
            break;
    }

    uint64_t lag   = FallowLag();
    uint64_t gives = 0;
    TagFSLockTake(lock);
    for (FallowNode* n = first; n; n = n->next) {
        if (!n->zeroed_in)
            n->zeroed_in = cycle;
        if (NodeMayRipen(n, cycle, lag))
            gives += NodeWindows(n);
        if (n == last)
            break;
    }
    TagFSLockDrop(lock);

    /* Refused, it is said by the gives it leaves resting (FallowRipen). */
    (void)BlockMapGiveRoomHold(gives < UINT32_MAX ? (uint32_t)gives : UINT32_MAX);
}

void FallowLedgerTaken(uint64_t cycle)
{
    for (FallowNode* n = s_head; n; n = n->next)
        if (n->kind != FALLOW_BY_RECORD && n->gen == 0)
            n->gen = cycle;
}

void FallowRetag(uint64_t from, uint64_t to)
{
    for (FallowNode* n = s_head; n; n = n->next)
        if (n->kind == FALLOW_BY_RECORD && n->ripe_at == 0 && n->gen == from)
            n->gen = to;
}

/* The node left free in every image of the map from here on, every window it
 * reaches owed to the medium now. Under the state lock. */
static void NodeUnname(FallowNode* n)
{
    n->unnamed = true;
    s_unnamed++;
    for (uint32_t i = 0; i < n->count; i++) {
        uint64_t a = n->runs[i].start;
        uint64_t z = a + n->runs[i].count;
        for (uint64_t b = a; b < z; b = (b / BLOCK_MAP_WINDOW_BLOCKS + 1u) * BLOCK_MAP_WINDOW_BLOCKS)
            BlockMapOweLocked(b);
    }
}

void FallowUnlinkedUnnamed(uint64_t upto, FallowNode* also)
{
    for (FallowNode* n = s_head; n; n = n->next) {
        if (n->unnamed || n->kind != FALLOW_BY_UNLINK ||
            (n != also && (n->gen == 0 || n->gen > upto)))
            continue;
        NodeUnname(n);
    }
}

/* Ripe by the settled cycle `settled` — the S6 that would have given it back
 * did not only for the view (FallowRipen) — and held by nothing else. */
void FallowViewedUnnamed(uint64_t settled)
{
    for (FallowNode* n = s_head; n; n = n->next) {
        if (n->unnamed || n->kind != FALLOW_BY_RECORD || n->ripe_at == 0 ||
            n->ripe_at > settled || n->zeroed_in == 0 || n->zeroed_in > settled ||
            n->gen <= HelmViewFloor(n->fid))
            continue;
        NodeUnname(n);
    }
}

uint32_t FallowUnnamedLeftOut(uint64_t first, uint32_t count, uint8_t* bits)
{
    uint64_t end  = first + count;
    uint32_t left = 0;

    for (FallowNode* n = s_unnamed ? s_head : NULL; n; n = n->next) {
        if (!n->unnamed)
            continue;
        for (uint32_t i = 0; i < n->count; i++) {
            uint64_t a = n->runs[i].start;
            uint64_t z = a + n->runs[i].count;
            for (uint64_t b = a > first ? a : first; b < z && b < end; b++) {
                uint32_t off = (uint32_t)(b - first);
                bits[off / 8] &= (uint8_t)~(1u << (off % 8));
                left++;
            }
        }
    }
    return left;
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

bool FallowBarsRun(uint64_t start, uint64_t count, uint64_t* past)
{
    bool met = false;
    *past    = start;
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
 * The node's runs back to the allocator, through the room for the map's
 * deltas made before the lock (FallowZeroDigests) — on a book, through the
 * pages held for the derived map (FallowGiveRoomHold): a run that room does
 * not cover stays in the node, which rests on, and the next cycle gives it
 * back. A run the map refuses for any other reason — a block of it already
 * free, or past the data run — is said by the map, and no cycle could give it
 * back: it leaves the node. On a book, a run given back takes the digests the
 * Gloss holds for it out with it (§8.1). The blocks given back.
 */
static uint64_t NodeGiveBack(FallowNode* n, bool book)
{
    uint64_t given = 0;
    uint32_t kept  = 0, count = n->count;
    bool     bar   = book && NodeBarred(n);

    for (uint32_t i = 0; i < count; i++) {
        Strip    run  = n->runs[i];
        uint64_t met  = 0;
        error_t  back = book ? BlockMapDerivedRelease(n->fid ? GROUND_FILE : GROUND_FALLOW, n->fid, run.start,
                                                      run.count, &met)
                             : BlockMapGiveHeldLocked(run.start, run.count);
        if (back == OK && book) {
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

/* Whether format v1's rule holds the node at the end of cycle `cycle` — its
 * freer's cycle given first, once the freer lies in a closed block. */
static bool NodeHeld(FallowNode* n, uint64_t closed_gen, uint64_t cycle, uint64_t lag)
{
    if (n->ripe_at == 0) {
        if (n->kind != FALLOW_BY_RECORD && n->gen != 0)
            n->ripe_at = n->gen + lag;
        else if (n->kind == FALLOW_BY_RECORD && n->gen <= closed_gen)
            n->ripe_at = cycle + lag;
    }
    return n->ripe_at == 0 || cycle < n->ripe_at || n->zeroed_in == 0 || n->zeroed_in > cycle ||
           (n->kind == FALLOW_BY_RECORD && n->gen > HelmViewFloor(n->fid)) ||
           (n->kind == FALLOW_BY_USE_CHAIN && UseMemoryNames(n->runs[0].start)) ||
           (n->kind == FALLOW_BY_UNLINK && n->gen > GleanHintCovers());
}

/* Whether a book's rule holds it at the end of its `cycle`-th settled stitch
 * (§3.4): until a stitch named its freer, until the ribbon left the Quire that
 * stitch lies in, until FallowLag() stitches settled after it, and while an
 * open handle views the file as it was before (3). No digest is zeroed for it:
 * the digest lives in the record, and a block's next owner brings its own (5). */
static bool NodeHeldOnBook(const FallowNode* n, uint64_t cycle)
{
    return n->ripe_at == 0 || !n->left || cycle < n->ripe_at || n->gen > HelmViewFloor(n->fid);
}

uint64_t FallowRipen(uint64_t closed_gen, uint64_t cycle, FallowNode** spent)
{
    uint64_t    given  = 0;
    uint64_t    stayed = 0;          /* ripe, and resting on for want of the map's room */
    FallowNode* prev   = NULL;
    uint64_t    lag    = FallowLag();
    bool        book   = tagfs_get_state()->format == DEED_FORMAT_V2;

    *spent = NULL;
    for (FallowNode* n = s_head; n; ) {
        FallowNode* next = n->next;
        if (book ? NodeHeldOnBook(n, cycle) : NodeHeld(n, closed_gen, cycle, lag)) {
            prev = n;
            n    = next;
            continue;
        }

        uint64_t blocks = NodeBlocks(n);
        given    += NodeGiveBack(n, book);
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
        s_unnamed -= n->unnamed;
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
// A book's freers: the stitch that names them, and the Quire it lies in
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

bool FallowRestsOnTail(uint64_t closed_gen)
{
    for (FallowNode* n = s_head; n; n = n->next)
        if (n->kind == FALLOW_BY_RECORD && n->ripe_at == 0 && n->gen > closed_gen)
            return true;
    return false;
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

uint64_t FallowBlocksMapTaken(void)
{
    uint64_t left_out = 0;
    for (FallowNode* n = s_unnamed ? s_head : NULL; n; n = n->next)
        if (n->unnamed)
            left_out += NodeBlocks(n);
    return s_blocks - left_out;
}

void FallowDropAll(void)
{
    FallowNodesFree(s_head);
    FallowNodesFree(s_barred);
    s_barred    = NULL;
    s_head      = NULL;
    s_tail      = NULL;
    s_blocks    = 0;
    s_unnamed   = 0;
    s_held_said = false;
}
