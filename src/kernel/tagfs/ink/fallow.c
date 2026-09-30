#include "fallow.h"
#include "../integrity/integrity.h"
#include "../helm/helm.h"
#include "../settle/settle.h"
#include "../use_memory/use_memory.h"

struct FallowNode {
    FallowNode* next;
    uint64_t    gen;        /* the freer: an Ink's gen, or a cycle's number */
    uint64_t    ripe_at;    /* the cycle it goes back at; 0 while its freer is unsettled */
    uint64_t    zeroed_in;  /* the cycle that zeroed its digests; 0 while none has */
    FallowKind  kind;
    uint32_t    fid;        /* the file a record freer freed it from; 0 for the others */
    uint32_t    count;
    FileExtent  runs[];
};

/* Oldest first: nodes are added in the order their freers were made. */
static FallowNode* s_head;
static FallowNode* s_tail;
static uint64_t    s_blocks;

static uint64_t NodeBlocks(const FallowNode* n)
{
    uint64_t blocks = 0;
    for (uint32_t i = 0; i < n->count; i++)
        blocks += n->runs[i].block_count;
    return blocks;
}

FallowNode* FallowNodeMake(const FileExtent* runs, uint32_t count, FallowKind kind, uint32_t fid)
{
    FallowNode* n = kmalloc(sizeof(FallowNode) + sizeof(FileExtent) * count);
    if (!n)
        return NULL;
    n->next      = NULL;
    n->gen       = 0;
    n->ripe_at   = 0;
    n->zeroed_in = 0;
    n->kind      = kind;
    n->fid       = fid;
    n->count     = count;
    if (count)
        memcpy(n->runs, runs, sizeof(FileExtent) * count);
    return n;
}

void FallowNodeLetGo(FallowNode* node)
{
    kfree(node);
}

uint64_t FallowLag(void)
{
    return (uint64_t)VOLUME_LEDGER_COPIES - 1u + (SettleMediumLied() ? 1u : 0u);
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

/* The walk reads nodes with the lock let go: a node is freed only by
 * FallowRipen, which the Settle's holder calls — this caller — and a node added
 * meanwhile hangs past `last`, where the walk stops. */
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
                IntegrityForget(n->runs[i].start_block, n->runs[i].block_count);
        if (n == last)
            break;
    }

    TagFSLockTake(lock);
    for (FallowNode* n = first; n; n = n->next) {
        if (!n->zeroed_in)
            n->zeroed_in = cycle;
        if (n == last)
            break;
    }
    TagFSLockDrop(lock);
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

uint64_t FallowRipen(uint64_t closed_gen, uint64_t cycle, FallowNode** spent)
{
    uint64_t    given = 0;
    FallowNode* prev  = NULL;

    uint64_t    lag   = FallowLag();

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
            (n->kind == FALLOW_BY_USE_CHAIN && UseMemoryNames(n->runs[0].start_block))) {
            prev = n;
            n    = next;
            continue;
        }

        for (uint32_t i = 0; i < n->count; i++) {
            error_t back = BlockMapGiveLocked(n->runs[i].start_block, n->runs[i].block_count);
            if (back != OK)
                kprintf("[TagFS] Fallow: blocks %u..+%u rested their cycles and would not go "
                        "back to the allocator (error %u) — they stay taken, for the mount's "
                        "roll call to find\n", n->runs[i].start_block,
                        (unsigned)n->runs[i].block_count, (unsigned)back);
        }
        uint64_t blocks = NodeBlocks(n);
        s_blocks -= blocks;
        given    += blocks;

        if (prev)
            prev->next = next;
        else
            s_head = next;
        if (s_tail == n)
            s_tail = prev;
        n->next = *spent;
        *spent  = n;
        n       = next;
    }
    return given;
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
    s_head   = NULL;
    s_tail   = NULL;
    s_blocks = 0;
}
