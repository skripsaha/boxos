#include "use_memory_internal.h"
#include "../ink/fallow.h"
#include "../settle/settle.h"

/*
 * The volume's memory of the Use Context, as the machine changes it (use_memory.h):
 * the person's word handed over, and the Settle cycle's part in carrying it.
 */

UseMemoryState g_use_memory;

/*
 * What one Settle cycle carries — the Settle's holder's alone, from its
 * UseMemoryStake to its UseMemoryCycleOver, one cycle at a time.
 *
 *   NONE       no demand: the image names the chain the volume holds;
 *   KEEP       the word is what the volume holds: nothing is written, and the
 *              demand is answered when the image naming it has settled;
 *   CHAIN      a chain for the word — staked by this cycle, or kept from a
 *              refused one for the same demand — written at S1 and named;
 *   FORGET     the word is none: the image names none;
 *   NO_GROUND  the volume has no ground for the word, or this machine no
 *              memory to carry it: the image names none, and the volume
 *              forgets rather than remembering a word no longer said.
 */
typedef enum {
    USE_CYCLE_NONE = 0,
    USE_CYCLE_KEEP,
    USE_CYCLE_CHAIN,
    USE_CYCLE_FORGET,
    USE_CYCLE_NO_GROUND,
} UseCycleKind;

static struct {
    UseCycleKind kind;
    uint64_t     ticket;
    bool         carry;
    bool         fresh;        /* CHAIN: staked by this cycle */
    UseText     *text;         /* CHAIN: the word, held */
    UseChain     chain;        /* CHAIN: its name and blocks — the cycle's until S6 or the refusal */
    uint64_t     need;         /* NO_GROUND: blocks the word needs */
    uint64_t     got;          /* NO_GROUND: blocks the volume had */
    UseChainRef  image;        /* what the image names */
    uint32_t     slot;         /* the copy the image goes to */
    uint64_t     link_need;
    bool         replaces;     /* S6 lets the chain the volume held go */
    bool         rested;       /* and it could rest */
    UseChain     gone;         /* the chain let go, for UseMemoryCycleOver */
    UseText     *gone_text;
} s_cycle;

// ----------------------------------------------------------------------------
// The copies' names
// ----------------------------------------------------------------------------

bool UseNamedRoom(void)
{
    for (uint32_t c = 0; c < VOLUME_LEDGER_COPIES; c++) {
        UseNamed *n = &g_use_memory.named[c];
        TagFSLockTake(UseMemoryLock());
        uint32_t want = n->count + 1u;
        bool     room = n->cap >= want;
        TagFSLockDrop(UseMemoryLock());
        if (room)
            continue;

        uint32_t  cap  = want * 2u;
        uint32_t *more = kmalloc(sizeof(uint32_t) * cap);
        if (!more)
            return false;
        TagFSLockTake(UseMemoryLock());
        uint32_t *old = n->first;
        memcpy(more, n->first, sizeof(uint32_t) * n->count);
        n->first = more;
        n->cap   = cap;
        TagFSLockDrop(UseMemoryLock());
        kfree(old);
    }
    return true;
}

void UseNamedAdd(uint32_t c, uint32_t first)
{
    UseNamed *n = &g_use_memory.named[c];
    if (first == VOLUME_LEDGER_NO_BLOCK)
        return;
    for (uint32_t i = 0; i < n->count; i++)
        if (n->first[i] == first)
            return;
    if (n->count < n->cap)
        n->first[n->count++] = first;
}

void UseNamedOnly(uint32_t c, uint32_t first)
{
    UseNamed *n = &g_use_memory.named[c];
    n->count    = 0;
    if (first != VOLUME_LEDGER_NO_BLOCK && n->cap)
        n->first[n->count++] = first;
}

bool UseMemoryNames(uint32_t first)
{
    for (uint32_t c = 0; c < VOLUME_LEDGER_COPIES; c++)
        for (uint32_t i = 0; i < g_use_memory.named[c].count; i++)
            if (g_use_memory.named[c].first[i] == first)
                return true;
    return false;
}

uint64_t UseMemoryBlocksHeld(void)
{
    const UseChain *held[] = { &g_use_memory.settled, &g_use_memory.pending };
    uint64_t        blocks = 0;
    for (uint32_t i = 0; i < ARRAY_SIZE(held); i++)
        for (uint32_t r = 0; r < held[i]->run_count; r++)
            blocks += held[i]->runs[r].block_count;
    return blocks;
}

bool UseChainRest(const UseChain *chain, const char *what)
{
    if (!chain->run_count)
        return true;
    FallowNode *node = FallowNodeMake(chain->runs, chain->run_count, FALLOW_BY_USE_CHAIN, 0);
    if (!node) {
        kprintf("[TagFS] no memory to let %s — %u run(s) from data block %u — rest until no "
                "copy of the Ledger names it: it stays taken\n", what, chain->run_count,
                chain->runs[0].start_block);
        return false;
    }
    TagFSLockTake(UseMemoryLock());
    FallowAdd(node, 0);
    TagFSLockDrop(UseMemoryLock());
    return true;
}

// ----------------------------------------------------------------------------
// The person's word
// ----------------------------------------------------------------------------

/*
 * ‼ WHAT THE MEDIUM ALREADY HOLDS IS NOT WRITTEN AGAIN. A replug recalls the
 * context and remembers it at once; a memory the newest copy names, with no
 * demand on its way, answers at once and costs nothing — the texts are
 * compared outside the lock, and a demand made while they were is seen by
 * the second look.
 */
UseSaid UseMemorySay(UseText *said, uint64_t saying, uint64_t *ticket)
{
    UseMemoryState *g    = &g_use_memory;
    UseText        *keep = UseTextHold(said);
    UseText        *gone = NULL;

    TagFSLockTake(UseMemoryLock());
    bool     later  = saying < g->saying;
    UseText *memory = UseTextHold(g->memory);
    bool     quiet  = !g->owed && g->landed && UseMemoryChained();
    uint64_t seen   = g->ticket;
    TagFSLockDrop(UseMemoryLock());

    bool same = !later && quiet && UseTextSame(memory, keep);
    UseTextDrop(memory);

    uint64_t mine = 0;
    TagFSLockTake(UseMemoryLock());
    later = later || saying < g->saying;
    if (!later) {
        same = same && g->ticket == seen && !g->owed;
        gone        = g->said;
        g->said     = keep;
        keep        = NULL;
        g->saying   = saying;
        if (!same) {
            g->owed  = true;
            g->carry = false;
            mine     = ++g->ticket;
        }
    }
    TagFSLockDrop(UseMemoryLock());
    UseTextDrop(gone);
    UseTextDrop(keep);

    if (ticket) *ticket = mine;
    if (later)
        return USE_SAID_SUPERSEDED;
    return same ? USE_SAID_LANDED : USE_SAID_OWED;
}

bool UseMemoryKept(uint64_t ticket, const UseText *said)
{
    UseMemoryState *g = &g_use_memory;
    TagFSLockTake(UseMemoryLock());
    bool     answered = g->answered >= ticket && g->landed && UseMemoryChained();
    UseText *now      = UseTextHold(g->memory);
    TagFSLockDrop(UseMemoryLock());
    bool kept = answered && UseTextSame(now, said);
    UseTextDrop(now);
    return kept;
}

error_t tagfs_remember_use_context(UseText *said, uint64_t saying, bool *remembered)
{
    uint64_t ticket = 0;
    UseSaid  told   = UseMemorySay(said, saying, &ticket);
    if (told != USE_SAID_OWED) {
        if (remembered) *remembered = told == USE_SAID_LANDED;
        return OK;
    }

    /* A demand on the one writer: a cycle that began after it carries it. */
    error_t rc = TagFSSettle();
    if (remembered) *remembered = rc == OK && UseMemoryKept(ticket, said);
    return rc;
}

UseText *tagfs_recall_use_context(uint64_t *out_saying)
{
    TagFSLockTake(UseMemoryLock());
    UseText *memory = UseTextHold(g_use_memory.memory);
    uint64_t saying = g_use_memory.saying;
    TagFSLockDrop(UseMemoryLock());
    if (out_saying) *out_saying = saying;
    return memory;
}

// ----------------------------------------------------------------------------
// The Settle cycle's part
// ----------------------------------------------------------------------------

/* A chain kept from a refused cycle whose demand a later word replaced: no
 * cycle will name it now, and it rests until neither copy that may have been
 * given it names it. The Settle's holder's. */
static void UseStaleRest(uint64_t ticket)
{
    UseMemoryState *g = &g_use_memory;
    if (g->pending.ref.first == VOLUME_LEDGER_NO_BLOCK || g->pending_ticket == ticket)
        return;
    UseChain stale = g->pending;
    TagFSLockTake(UseMemoryLock());
    memset(&g->pending, 0, sizeof(g->pending));
    g->pending_ticket = 0;
    TagFSLockDrop(UseMemoryLock());
    (void)UseChainRest(&stale, "a Use Context chain a refused cycle wrote for a word since replaced");
    UseChainForget(&stale);
}

/* The word's chain: the one a refused cycle kept for the same demand, or new
 * blocks — none found, or no memory for them, is no ground. */
static void UseStakeChain(uint64_t ticket)
{
    UseMemoryState *g = &g_use_memory;
    if (g->pending.ref.first != VOLUME_LEDGER_NO_BLOCK && g->pending_ticket == ticket) {
        s_cycle.kind  = USE_CYCLE_CHAIN;
        s_cycle.chain = g->pending;
        return;
    }
    s_cycle.need = UseChainLinksFor(s_cycle.text->bytes);
    error_t rc   = UseChainStake(s_cycle.need, &s_cycle.chain, &s_cycle.got);
    if (rc != OK) {
        s_cycle.kind = USE_CYCLE_NO_GROUND;
        if (rc == ERR_NO_MEMORY)
            s_cycle.need = 0;
        return;
    }
    s_cycle.kind            = USE_CYCLE_CHAIN;
    s_cycle.fresh           = true;
    s_cycle.chain.ref.first = s_cycle.chain.runs[0].start_block;
    s_cycle.chain.ref.bytes = s_cycle.text->bytes;
    s_cycle.chain.ref.crc32 = UseTextCrc32(s_cycle.text);
}

void UseMemoryStake(void)
{
    UseMemoryState *g = &g_use_memory;

    /* A cycle abandoned mid-way — the halt took it over (ScribeWaitTakingOver)
     * — is a refused one: its chain is kept, not lost. */
    if (s_cycle.kind != USE_CYCLE_NONE)
        UseMemoryCycleOver(ERR_TAGFS_METADATA_ERROR);
    memset(&s_cycle, 0, sizeof(s_cycle));

    TagFSLockTake(UseMemoryLock());
    bool     owed    = g->owed;
    uint64_t ticket  = g->ticket;
    bool     carry   = g->carry;
    UseText *text    = UseTextHold(g->said);
    UseText *memory  = UseTextHold(g->memory);
    bool     chained = UseMemoryChained();
    TagFSLockDrop(UseMemoryLock());

    UseStaleRest(owed ? ticket : 0);
    if (owed) {
        s_cycle.ticket = ticket;
        s_cycle.carry  = carry;
        s_cycle.text   = text;
        text           = NULL;
        if (!s_cycle.text || s_cycle.text->bytes == 0)
            s_cycle.kind = USE_CYCLE_FORGET;
        else if (chained && UseTextSame(memory, s_cycle.text))
            s_cycle.kind = USE_CYCLE_KEEP;
        else
            UseStakeChain(ticket);
    }
    UseTextDrop(text);
    UseTextDrop(memory);

    /* Room for the image's name in the copy it goes to, whichever that is;
     * without it the chain is not named at all — the volume forgets. */
    if (!UseNamedRoom() && s_cycle.kind == USE_CYCLE_CHAIN) {
        if (s_cycle.fresh)
            UseChainUnstake(&s_cycle.chain);
        memset(&s_cycle.chain, 0, sizeof(s_cycle.chain));
        s_cycle.kind  = USE_CYCLE_NO_GROUND;
        s_cycle.fresh = false;
        s_cycle.need  = 0;
    }
    s_cycle.replaces = s_cycle.kind >= USE_CYCLE_CHAIN &&
                       g->settled.ref.first != VOLUME_LEDGER_NO_BLOCK &&
                       g->settled.ref.first != s_cycle.chain.ref.first;
}

void UseMemoryTake(void)
{
    UseMemoryState *g = &g_use_memory;
    s_cycle.slot = LedgerImageSlot();
    if (s_cycle.kind == USE_CYCLE_CHAIN && s_cycle.fresh) {
        s_cycle.chain.ref.seq     = LedgerImageSeq();
        s_cycle.chain.ref.attempt = ++g->attempt;
    }
    if (s_cycle.kind == USE_CYCLE_CHAIN)
        s_cycle.image = s_cycle.chain.ref;
    else if (s_cycle.kind == USE_CYCLE_FORGET || s_cycle.kind == USE_CYCLE_NO_GROUND)
        memset(&s_cycle.image, 0, sizeof(s_cycle.image));
    else
        s_cycle.image = g->settled.ref;
    LedgerImageUseChain(&s_cycle.image);
}

error_t UseMemoryWriteLinks(void)
{
    if (s_cycle.kind != USE_CYCLE_CHAIN)
        return OK;
    error_t rc = UseChainWrite(&s_cycle.chain, s_cycle.text);
    if (rc == OK)
        s_cycle.link_need = VolumeFlushNeed();
    return rc;
}

uint64_t UseMemoryLinkNeed(void)
{
    return s_cycle.link_need;
}

void UseMemoryLedgerWritten(void)
{
    TagFSLockTake(UseMemoryLock());
    UseNamedAdd(s_cycle.slot, s_cycle.image.first);
    TagFSLockDrop(UseMemoryLock());
}

void UseMemorySettled(void)
{
    UseMemoryState *g = &g_use_memory;
    UseNamedOnly(s_cycle.slot, s_cycle.image.first);
    g->landed = true;
    if (s_cycle.kind == USE_CYCLE_NONE)
        return;

    if (s_cycle.kind != USE_CYCLE_KEEP) {
        s_cycle.gone      = g->settled;
        s_cycle.gone_text = g->memory;
        memset(&g->settled, 0, sizeof(g->settled));
        g->memory = NULL;
        if (s_cycle.kind == USE_CYCLE_CHAIN) {
            g->settled = s_cycle.chain;
            g->memory  = s_cycle.text;
            s_cycle.text = NULL;
            if (g->pending.ref.first == s_cycle.chain.ref.first) {
                memset(&g->pending, 0, sizeof(g->pending));
                g->pending_ticket = 0;
            }
            memset(&s_cycle.chain, 0, sizeof(s_cycle.chain));
        }
    }
    if (g->ticket == s_cycle.ticket) {
        g->owed  = false;
        g->carry = false;
    }
    if (s_cycle.ticket > g->answered)
        g->answered = s_cycle.ticket;
}

/* What the volume forgot at S6, said once the lock is let go. */
static void UseForgetSay(void)
{
    if (s_cycle.kind != USE_CYCLE_NO_GROUND)
        return;
    if (s_cycle.carry && s_cycle.need)
        kprintf("[TagFS] the volume had no room to carry its remembered Use Context into its "
                "new form — it needs %llu block(s), there are %llu — it is forgotten\n",
                (unsigned long long)s_cycle.need, (unsigned long long)s_cycle.got);
    else if (s_cycle.carry)
        kprintf("[TagFS] this machine had no memory to carry the volume's remembered Use "
                "Context into its new form — it is forgotten\n");
    else if (s_cycle.need)
        kprintf("[TagFS] the volume forgot the previous Use Context: the new one needs %llu "
                "block(s), there are %llu\n",
                (unsigned long long)s_cycle.need, (unsigned long long)s_cycle.got);
    else
        kprintf("[TagFS] the volume forgot the previous Use Context: this machine had no memory "
                "to carry the new one to it\n");
}

void UseMemoryCycleOver(error_t rc)
{
    UseMemoryState *g = &g_use_memory;
    if (rc != OK) {
        /* Refused: a new chain is kept for its demand's next cycle — the copy
         * the image went to may name it now, so it is never staked again but
         * written again, the same bytes to the same blocks. */
        if (s_cycle.kind == USE_CYCLE_CHAIN && s_cycle.fresh) {
            TagFSLockTake(UseMemoryLock());
            g->pending        = s_cycle.chain;
            g->pending_ticket = s_cycle.ticket;
            TagFSLockDrop(UseMemoryLock());
        }
    } else {
        UseForgetSay();
        if (s_cycle.replaces)
            (void)UseChainRest(&s_cycle.gone, "the Use Context chain the volume stopped remembering");
    }
    UseChainForget(&s_cycle.gone);
    UseTextDrop(s_cycle.gone_text);
    UseTextDrop(s_cycle.text);
    memset(&s_cycle, 0, sizeof(s_cycle));
}
