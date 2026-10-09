#include "fallow.h"
#include "../integrity/integrity.h"
#include "../helm/helm.h"
#include "../settle/settle.h"
#include "../use_memory/use_memory.h"
#include "../glean/glean.h"

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
    Strip       runs[];
};

/* Oldest first: nodes are added in the order their freers were made. */
static FallowNode* s_head;
static FallowNode* s_tail;
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
    return n;
}

FallowNode* FallowNodeMake(const Strip* runs, uint32_t count, FallowKind kind, uint64_t fid)
{
    FallowNode* n = NodeAlloc(count, kind, fid);
    if (n && count) {
        memcpy(n->runs, runs, sizeof(Strip) * count);
        n->count = count;
    }
    return n;
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

uint64_t FallowLag(void)
{
    return SettleMediumLied() ? FALLOW_LIED_LAG : (uint64_t)VOLUME_LEDGER_COPIES - 1u;
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
 * The node's runs back to the allocator, through the room for the map's
 * deltas made before the lock (FallowZeroDigests): a run that room does not
 * cover stays in the node, which rests on, and the next cycle gives it back.
 * A run the map refuses for any other reason — a block of it already free, or
 * past the data run — is said by the map, and no cycle could give it back: it
 * leaves the node. The blocks given back.
 */
static uint64_t NodeGiveBack(FallowNode* n)
{
    uint64_t given = 0;
    uint32_t kept  = 0;

    for (uint32_t i = 0; i < n->count; i++) {
        Strip   run  = n->runs[i];
        error_t back = BlockMapGiveHeldLocked(run.start, run.count);
        if (back == OK)
            given += run.count;
        else if (back == ERR_NO_MEMORY)
            n->runs[kept++] = run;
    }
    n->count = kept;
    return given;
}

uint64_t FallowRipen(uint64_t closed_gen, uint64_t cycle, FallowNode** spent)
{
    uint64_t    given  = 0;
    uint64_t    stayed = 0;          /* ripe, and resting on for want of the map's room */
    FallowNode* prev   = NULL;
    uint64_t    lag    = FallowLag();

    *spent = NULL;
    for (FallowNode* n = s_head; n; ) {
        FallowNode* next = n->next;
        if (n->ripe_at == 0) {
            if (n->kind != FALLOW_BY_RECORD && n->gen != 0)
                n->ripe_at = n->gen + lag;
            else if (n->kind == FALLOW_BY_RECORD && n->gen <= closed_gen)
                n->ripe_at = cycle + lag;
        }
        if (n->ripe_at == 0 || cycle < n->ripe_at || n->zeroed_in == 0 || n->zeroed_in > cycle ||
            (n->kind == FALLOW_BY_RECORD && n->gen > HelmViewFloor(n->fid)) ||
            (n->kind == FALLOW_BY_USE_CHAIN && UseMemoryNames(n->runs[0].start)) ||
            (n->kind == FALLOW_BY_UNLINK && n->gen > GleanHintCovers())) {
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
        s_unnamed -= n->unnamed;
        n->next = *spent;
        *spent  = n;
        n       = next;
    }
    if (stayed && !s_held_said)
        kprintf("[TagFS] Fallow: %llu block(s) rested their cycles and the block map had no "
                "room to write their gives down — they rest on, and a later cycle gives them "
                "back\n", (unsigned long long)stayed);
    s_held_said = stayed != 0;
    return given;
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
    s_head      = NULL;
    s_tail      = NULL;
    s_blocks    = 0;
    s_unnamed   = 0;
    s_held_said = false;
}
