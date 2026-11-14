#include "use_memory_internal.h"
#include "../ink/fallow.h"
#include "../ledger/ledger.h"
#include "../mount/ground_census.h"

/*
 * What a mount reads of the volume's memory of the Use Context (use_memory.h),
 * with the door shut: the chains the copies of the Ledger name, which one the
 * memory stands on, the roll call of their blocks, and the orphans an older
 * kernel's Ledger stopped naming.
 */

void UseMemoryVerdictDrop(UseMemoryVerdict *v)
{
    UseChainForget(&v->chain);
    UseTextDrop(v->text);
    for (uint32_t i = 0; i < v->loose_count; i++)
        UseChainForget(&v->loose[i]);
    memset(v, 0, sizeof(*v));
}

static const char *UseStateSaid(UseChainState state)
{
    switch (state) {
    case USE_CHAIN_FOREIGN: return "a link's block still holds what it held before the link was written";
    case USE_CHAIN_ROTTEN:  return "its links are in their places and do not agree with it, or its text does not sum";
    case USE_CHAIN_UNREAD:  return "the medium would not give a link back";
    case USE_CHAIN_UNHELD:  return "this machine could not hold what reading it takes";
    default:                return "it reads whole";
    }
}

/* A chain some copy names that no memory stands on: read for its blocks, so
 * the roll call owns them and Fallow can let them go once no copy names them. */
static void UseJudgeLoose(const UseChainRef *ref, UseMemoryVerdict *v, UseChainState *state)
{
    UseChain chain;
    *state = UseChainRead(ref, NULL, &chain);
    v->loose[v->loose_count++] = chain;
}

/*
 * The newest copy's chain did not read whole: the memory rolls back to what the
 * copy before it remembers — its chain, or a text a kernel before the chain
 * kept in it — and the files do not roll back with it.
 */
static void UseJudgeOlder(const VolumeLedger *rec, const UseChainRef *newest, bool link,
                          const UseChainRef *ref, UseText *inherited, UseMemoryVerdict *v)
{
    if (!rec) {
        v->from = USE_FROM_NONE;
    } else if (link) {
        if (ref->first == VOLUME_LEDGER_NO_BLOCK || ref->first == newest->first) {
            v->from = USE_FROM_NONE;
            return;
        }
        v->older_state = UseChainRead(ref, &v->text, &v->chain);
        if (v->older_state == USE_CHAIN_WHOLE || v->older_state == USE_CHAIN_UNHELD) {
            v->from = USE_FROM_OLDER;
        } else {
            v->loose[v->loose_count++] = v->chain;
            memset(&v->chain, 0, sizeof(v->chain));
            v->from = USE_FROM_NONE;
        }
    } else if (inherited) {
        v->from = USE_FROM_INHERITED;
        v->text = UseTextHold(inherited);
    } else {
        v->from = USE_FROM_NONE;
    }
}

void UseMemoryJudge(const VolumeLedger *const rec[VOLUME_LEDGER_COPIES], uint32_t newest,
                    UseText *const inherited[VOLUME_LEDGER_COPIES], UseMemoryVerdict *v)
{
    uint32_t    older = (newest + 1u) % VOLUME_LEDGER_COPIES;
    UseChainRef ref[VOLUME_LEDGER_COPIES];
    bool        link[VOLUME_LEDGER_COPIES];

    memset(v, 0, sizeof(*v));
    memset(ref, 0, sizeof(ref));
    for (uint32_t c = 0; c < VOLUME_LEDGER_COPIES; c++) {
        link[c] = rec[c] && UseChainRefOf(rec[c], &ref[c]);
        if (!link[c])
            continue;
        v->named[c] = ref[c].first;
        if (ref[c].attempt > v->attempt)
            v->attempt = ref[c].attempt;
    }
    if (!rec[newest])
        return;

    bool older_read = false;
    if (!link[newest]) {
        /* A kernel before the chain wrote the newest copy: its text is newer
         * than any chain; with none, the census's orphans are searched. */
        if (inherited[newest]) {
            v->from = USE_FROM_INHERITED;
            v->text = UseTextHold(inherited[newest]);
        } else {
            v->from = USE_FROM_ORPHANS;
        }
    } else if (ref[newest].first == VOLUME_LEDGER_NO_BLOCK) {
        v->from = USE_FROM_NONE;
    } else {
        v->newest_state = UseChainRead(&ref[newest], &v->text, &v->chain);
        if (v->newest_state == USE_CHAIN_WHOLE || v->newest_state == USE_CHAIN_UNHELD ||
            v->newest_state == USE_CHAIN_UNREAD) {
            /* Whole — or whole for all this machine can tell: a poor machine,
             * or a medium that would not read now, does not erase what the
             * volume remembers. */
            v->from = USE_FROM_NEWEST;
        } else {
            /* ‼ MEDIUM_LIED ONLY FOR A LINK THAT NEVER LANDED, in the chain
             * this record's own cycle wrote: its FLUSH#1 said the links were on
             * the medium before the Ledger naming them was written. A chain
             * that sums wrong in links of its own is rot, or a page torn with
             * its neighbour — the medium did not lie. */
            v->rolled = true;
            v->lied   = v->newest_state == USE_CHAIN_FOREIGN && ref[newest].seq == rec[newest]->seq;
            v->loose[v->loose_count++] = v->chain;
            memset(&v->chain, 0, sizeof(v->chain));
            UseJudgeOlder(rec[older], &ref[newest], link[older], &ref[older], inherited[older], v);
            older_read = link[older];
        }
    }

    if (!older_read && link[older] && ref[older].first != VOLUME_LEDGER_NO_BLOCK &&
        !(link[newest] && ref[older].first == ref[newest].first))
        UseJudgeLoose(&ref[older], v, &v->older_state);
}

// ----------------------------------------------------------------------------
// The mount
// ----------------------------------------------------------------------------

static void UseLoadSay(const UseMemoryVerdict *v, const UseChainRef *newest)
{
    if (v->from == USE_FROM_NEWEST && v->newest_state == USE_CHAIN_UNHELD)
        kprintf("[TagFS] the volume remembers a Use Context of %llu bytes, and this machine "
                "cannot hold it — the volume keeps it, and this machine does not take it up\n",
                (unsigned long long)newest->bytes);
    else if (v->from == USE_FROM_NEWEST && v->newest_state == USE_CHAIN_UNREAD)
        kprintf("[TagFS] the medium would not give back the Use Context the volume remembers "
                "(%llu bytes, a chain from data block %u) — the volume keeps it, and this "
                "machine does not take it up\n", (unsigned long long)newest->bytes, newest->first);
    else if (v->rolled)
        kprintf("[TagFS] the Use Context the newest copy of the Ledger names — %llu bytes in a "
                "chain from data block %u — does not read whole: %s. The volume remembers what "
                "the copy before it named instead%s%s\n", (unsigned long long)newest->bytes,
                newest->first, UseStateSaid(v->newest_state),
                v->from == USE_FROM_NONE ? ", which is nothing" : "",
                v->lied ? " — and that link was the medium's word broken: its FLUSH said the "
                          "chain had landed before the Ledger naming it was written "
                          "(SETTLE_MEDIUM_LIED)" : "");
    if (v->from == USE_FROM_INHERITED)
        kprintf("[TagFS] the volume remembers its Use Context the way a kernel before the chain "
                "kept it — %llu bytes inside the Ledger — and its first Settle cycle carries it "
                "into a chain\n", (unsigned long long)v->text->bytes);
}

error_t UseMemoryLoad(void)
{
    UseMemoryState     *g = &g_use_memory;
    const VolumeLedger *rec[VOLUME_LEDGER_COPIES];
    UseText            *inherited[VOLUME_LEDGER_COPIES];
    UseChainRef         newest_ref;
    uint32_t            newest = LedgerNewestCopy();
    UseMemoryVerdict    v;

    UseMemoryLetGo();
    for (uint32_t c = 0; c < VOLUME_LEDGER_COPIES; c++) {
        rec[c]       = LedgerCopyRead(c);
        inherited[c] = LedgerCopyInherited(c);
    }
    UseMemoryJudge(rec, newest, inherited, &v);
    for (uint32_t c = 0; c < VOLUME_LEDGER_COPIES; c++)
        UseTextDrop(inherited[c]);

    for (uint32_t c = 0; c < VOLUME_LEDGER_COPIES; c++) {
        g->named[c].first = kmalloc(sizeof(uint32_t) * 2u);
        if (!g->named[c].first) {
            UseMemoryVerdictDrop(&v);
            UseMemoryLetGo();
            return ERR_NO_MEMORY;
        }
        g->named[c].cap = 2u;
        UseNamedOnly(c, v.named[c]);
    }

    memset(&newest_ref, 0, sizeof(newest_ref));
    bool newest_link = rec[newest] && UseChainRefOf(rec[newest], &newest_ref);
    UseLoadSay(&v, &newest_ref);

    g->attempt = v.attempt;
    g->lied    = v.lied;
    switch (v.from) {
    case USE_FROM_NEWEST:
    case USE_FROM_OLDER:
        g->settled = v.chain;
        g->memory  = v.text;
        g->landed  = v.from == USE_FROM_NEWEST;
        memset(&v.chain, 0, sizeof(v.chain));
        v.text = NULL;
        break;
    case USE_FROM_INHERITED:
        g->memory      = v.text;
        g->said        = UseTextHold(v.text);
        g->owed        = true;
        g->carry       = true;
        g->ticket      = 1;
        g->carry_known = !newest_link;
        v.text = NULL;
        break;
    case USE_FROM_ORPHANS:
        g->search = true;
        break;
    default:
        g->landed = newest_link && newest_ref.first == VOLUME_LEDGER_NO_BLOCK;
        break;
    }
    for (uint32_t i = 0; i < v.loose_count; i++)
        g->loose[i] = v.loose[i];
    g->loose_count = v.loose_count;
    v.loose_count  = 0;
    UseMemoryVerdictDrop(&v);
    return OK;
}

static void UseClaim(const UseChain *chain)
{
    for (uint32_t r = 0; r < chain->run_count; r++)
        GroundClaim(GROUND_USE_CHAIN, chain->runs[r].start, chain->runs[r].count, 0);
}

void UseRollCall(void)
{
    UseClaim(&g_use_memory.settled);
    for (uint32_t i = 0; i < g_use_memory.loose_count; i++)
        UseClaim(&g_use_memory.loose[i]);
}

bool UseMemoryWantsOrphans(void)
{
    return g_use_memory.search || g_use_memory.carry_known;
}

void UseMemoryOrphanRun(uint64_t start, uint32_t count, void *ctx)
{
    UseMemoryState *g = &g_use_memory;
    (void)ctx;
    if (g->orphans_refused)
        return;
    if (g->orphan_count == g->orphan_cap) {
        uint32_t      grown = g->orphan_cap ? g->orphan_cap * 2u : 16u;
        UseOrphanRun *more  = kmalloc(sizeof(UseOrphanRun) * grown);
        if (!more) {
            g->orphans_refused = true;
            return;
        }
        if (g->orphan_count)
            memcpy(more, g->orphans, sizeof(UseOrphanRun) * g->orphan_count);
        kfree(g->orphans);
        g->orphans    = more;
        g->orphan_cap = grown;
    }
    g->orphans[g->orphan_count++] = (UseOrphanRun){ start, count };
}

/* The whole chain the memory takes up from among the orphans — or from the
 * copy before the newest, which an older kernel's write left naming one —
 * newest (seq, attempt) first. */
static bool UseAdopt(UseChain *found, UseText *text)
{
    UseMemoryState *g = &g_use_memory;
    g->settled = *found;
    g->memory  = text;
    g->landed  = false;
    memset(found, 0, sizeof(*found));
    kprintf("[TagFS] the newest copy of the Ledger was written by a kernel that does not know the "
            "Use Context's chain, and remembers none; a whole chain of %llu bytes (seq %llu, "
            "attempt %u) nothing names now is taken up again — a `use clear` said under that "
            "kernel reads the same way, and is undone here\n",
            (unsigned long long)g->settled.ref.bytes, (unsigned long long)g->settled.ref.seq,
            g->settled.ref.attempt);
    return true;
}

/* A loose chain the older copy names, read whole with its text: the orphans'
 * rival. */
static bool UseLooseCandidate(uint32_t *at, UseChain *chain, UseText **text)
{
    UseMemoryState *g = &g_use_memory;
    for (uint32_t i = 0; i < g->loose_count; i++) {
        if (UseChainRead(&g->loose[i].ref, text, chain) == USE_CHAIN_WHOLE) {
            *at = i;
            return true;
        }
        UseChainForget(chain);
    }
    return false;
}

static void UseOrphansJudge(void)
{
    UseMemoryState *g = &g_use_memory;
    UseChain       *found = NULL;
    UseText       **texts = NULL;
    uint32_t        n     = 0;
    uint32_t        taken = UINT32_MAX;

    if (g->orphans_refused)
        kprintf("[TagFS] no memory to hold the census's orphans — none of them is looked through "
                "for a Use Context chain\n");
    else if (g->orphan_count)
        n = UseChainFindOrphans(g->orphans, g->orphan_count, &found, &texts);

    if (g->search) {
        uint32_t at = 0;
        UseChain rival;
        UseText *rival_text = NULL;
        bool     has_rival  = UseLooseCandidate(&at, &rival, &rival_text);
        if (has_rival && (n == 0 || rival.ref.seq > found[0].ref.seq ||
                          (rival.ref.seq == found[0].ref.seq && rival.ref.attempt > found[0].ref.attempt))) {
            UseChainForget(&g->loose[at]);
            g->loose[at] = g->loose[--g->loose_count];
            UseAdopt(&rival, rival_text);
        } else {
            if (has_rival) {
                UseTextDrop(rival_text);
                UseChainForget(&rival);
            }
            if (n) {
                taken = 0;
                UseAdopt(&found[0], texts[0]);
                texts[0] = NULL;
            }
        }
    }

    uint32_t rested = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (i != taken && UseChainRest(&found[i], "a Use Context chain an older kernel's Ledger "
                                                  "stopped naming"))
            rested++;
        UseChainForget(&found[i]);
        UseTextDrop(texts[i]);
    }
    if (rested)
        kprintf("[TagFS] %u whole Use Context chain(s) among the blocks nothing names are older "
                "than what the volume remembers — they rest, and go back\n", rested);
    kfree(found);
    kfree(texts);
}

void UseMemoryAfterCensus(void)
{
    UseMemoryState *g = &g_use_memory;
    if (g->search || g->carry_known)
        UseOrphansJudge();
    for (uint32_t i = 0; i < g->loose_count; i++) {
        (void)UseChainRest(&g->loose[i], "a Use Context chain a copy of the Ledger names and the "
                                         "volume no longer remembers");
        UseChainForget(&g->loose[i]);
    }
    g->loose_count = 0;
    kfree(g->orphans);
    g->orphans         = NULL;
    g->orphan_count    = 0;
    g->orphan_cap      = 0;
    g->orphans_refused = false;
    g->search          = false;
    g->carry_known     = false;
}

bool UseMemoryMediumLied(void)
{
    return g_use_memory.lied;
}
