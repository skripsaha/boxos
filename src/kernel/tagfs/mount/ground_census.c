#include "ground_census.h"

/*
 * The census (ground_census.h). The claims are a list of runs, sorted once by
 * where they start, and swept a window at a time beside the medium's map: a
 * run that crosses the window's end is carried into the next, so every run is
 * looked at once for every window it lies in and never for any other.
 */

typedef struct {
    uint64_t start;
    uint64_t who;
    uint32_t count;
    uint32_t owner;
} GroundRun;

static const char* const g_owner_name[GROUND_OWNER_KINDS] = {
#define GROUND_OWNER_NAME(id, name, exclusive, formats) [id] = name,
    GROUND_OWNERS(GROUND_OWNER_NAME)
#undef GROUND_OWNER_NAME
};

static const bool g_owner_alone[GROUND_OWNER_KINDS] = {
#define GROUND_OWNER_ALONE(id, name, exclusive, formats) [id] = (exclusive) != 0,
    GROUND_OWNERS(GROUND_OWNER_ALONE)
#undef GROUND_OWNER_ALONE
};

/* The mount's alone (the door is shut). */
static GroundRun* s_runs;
static uint32_t   s_count;
static uint32_t   s_cap;
static bool       s_refused;       /* a claim found no memory: the census decides nothing */

/* The sweep: the next run not yet met, and the runs a window before this one
 * carried over its end. */
static uint32_t   s_next;
static uint32_t*  s_carried;
static uint32_t   s_carried_count;

/*
 * ‼ ON FORMAT v1 THE END OF THE GROUND IS THE END OF ITS DATA RUN, GROWN OR NOT.
 * A claim reaches block_end (ClaimEnd), which the LAYOUT gives at the Deed and
 * the growth moves with the data run it lengthens (ground_v1.c). Were the two
 * apart, every record naming the new ground would be cut from the census in
 * silence, and the block it names handed out again under it; a STRICT kernel
 * stops at that before the first claim.
 */
void GroundCensusBegin(void)
{
    GroundCensusEnd();
#if CONFIG_STRICT
    TagFSState* fs = tagfs_get_state();
    if (fs->format == DEED_FORMAT_V1 && fs->block_end != TagFSLayout()->data_blocks)
        panic("[TagFS] DEFECT: the census of a format-v1 volume would reach block %llu and its data run "
              "is %u blocks", (unsigned long long)fs->block_end, TagFSLayout()->data_blocks);
#endif
}

/* How far a claim reaches: the end of format v1's data run, which its bitmap
 * covers; all of a format-v2 volume, its far Deed's block among them — every
 * block of it is some owner's (ground_owner.h). */
static uint64_t ClaimEnd(void)
{
    TagFSState* fs = tagfs_get_state();
    return fs->format == DEED_FORMAT_V2 ? fs->volume_bytes / TAGFS_BLOCK_SIZE : fs->block_end;
}

void GroundClaim(GroundOwner owner, uint64_t start, uint32_t count, uint64_t who)
{
    uint64_t run = ClaimEnd();
    if (s_refused || count == 0 || start >= run || owner >= GROUND_OWNER_KINDS)
        return;
    if (count > run - start)
        count = (uint32_t)(run - start);

    if (s_count == s_cap) {
        uint32_t   cap  = s_cap ? s_cap * 2u : 64u;
        GroundRun* more = kmalloc(sizeof(GroundRun) * cap);
        if (!more) {
            if (tagfs_get_state()->format == DEED_FORMAT_V2)
                kprintf("[TagFS FSCK] no memory to hold the ground of %u owner run(s) — a book is "
                        "not mounted on a census it could not take\n", s_count + 1);
            else
                kprintf("[TagFS FSCK] no memory to hold the ground of %u owner run(s) — this "
                        "mount does not hold its block map against its owners, and takes "
                        "nothing back that reads free\n", s_count + 1);
            s_refused = true;
            return;
        }
        if (s_count)
            memcpy(more, s_runs, sizeof(GroundRun) * s_count);
        kfree(s_runs);
        s_runs = more;
        s_cap  = cap;
    }
    s_runs[s_count++] = (GroundRun){ start, who, count, (uint32_t)owner };
}

/* Heapsort by start: in place, and no deeper than one frame whatever the
 * number of runs. */
static void RunSiftDown(uint32_t at, uint32_t n)
{
    for (;;) {
        uint32_t big = at, l = 2u * at + 1u, r = l + 1u;
        if (l < n && s_runs[l].start > s_runs[big].start)
            big = l;
        if (r < n && s_runs[r].start > s_runs[big].start)
            big = r;
        if (big == at)
            return;
        GroundRun t  = s_runs[at];
        s_runs[at]   = s_runs[big];
        s_runs[big]  = t;
        at           = big;
    }
}

static void RunsSort(void)
{
    for (uint32_t i = s_count / 2u; i-- > 0; )
        RunSiftDown(i, s_count);
    for (uint32_t n = s_count; n > 1; n--) {
        GroundRun t = s_runs[0];
        s_runs[0]   = s_runs[n - 1];
        s_runs[n - 1] = t;
        RunSiftDown(0, n - 1);
    }
}

static void RunSay(const GroundRun* r, char* out, size_t room)
{
    if (r->owner == GROUND_FILE)
        ksnprintf(out, room, "file %llu", (unsigned long long)r->who);
    else
        ksnprintf(out, room, "the %s", g_owner_name[r->owner]);
}

/*
 * Two owners of one block: every exclusive run that begins before the
 * furthest end an exclusive run before it reached. The first 32 are named and
 * the rest counted — the owner's bound for a list said at mount (09-27).
 *
 * `by_claimant`: a run that meets one of the same owner and the same `who` is
 * one claimant naming its own ground twice, not two owners — format v2's
 * census, where every Tally a slot names claims its nodes and two Tallies
 * name an unchanged node alike (spec_v2 §2.9), as volume-read's census holds
 * each owner's ground as one claim (bookcensus.py). Format v1's names every
 * meeting, as it always did.
 */
static void RunsSayDoubles(bool by_claimant)
{
    uint64_t reach = 0;
    uint32_t reach_run = 0, named = 0, more = 0;
    bool     have  = false;
    char     a[32], b[32];

    for (uint32_t i = 0; i < s_count; i++) {
        const GroundRun* r = &s_runs[i];
        if (!g_owner_alone[r->owner])
            continue;
        const GroundRun* at = &s_runs[reach_run];
        bool one = by_claimant && have && at->owner == r->owner && at->who == r->who;
        if (have && r->start < reach && !one) {
            if (named < 32u) {
                RunSay(&s_runs[reach_run], a, sizeof(a));
                RunSay(r, b, sizeof(b));
                kprintf("[TagFS FSCK] data block %llu is named by %s and %s at once — the "
                        "medium kept less than a settled cycle said, deeper than the rest "
                        "a freed block takes covers, or a writer gave one block twice\n",
                        (unsigned long long)r->start, a, b);
                named++;
            } else {
                more++;
            }
        }
        if (!have || r->start + r->count > reach) {
            reach     = r->start + r->count;
            reach_run = i;
            have      = true;
        }
    }
    if (more)
        kprintf("[TagFS FSCK] and %u more block(s) named by two owners at once\n", more);
}

/* Bits [from, to) of a window's map set. */
static void BitsSet(uint8_t* bits, uint32_t from, uint32_t to)
{
    while (from < to && (from % 8u)) {
        bits[from / 8u] |= (uint8_t)(1u << (from % 8u));
        from++;
    }
    while (from + 8u <= to) {
        bits[from / 8u] = 0xFFu;
        from += 8u;
    }
    while (from < to) {
        bits[from / 8u] |= (uint8_t)(1u << (from % 8u));
        from++;
    }
}

/* One run laid into window [base, base + W); true when it reaches past it. */
static bool RunLay(const GroundRun* r, uint64_t base, uint8_t* bits)
{
    uint64_t end  = r->start + r->count;
    uint64_t stop = base + BLOCK_MAP_WINDOW_BLOCKS;
    uint64_t from = r->start > base ? r->start : base;
    uint64_t to   = end < stop ? end : stop;
    if (from < to)
        BitsSet(bits, (uint32_t)(from - base), (uint32_t)(to - base));
    return end > stop;
}

/* BlockMapOwnsFn: the windows come in order, and every run is met in the
 * window it starts in and carried while it reaches past one. */
static void GroundOwns(uint32_t window, uint8_t* bits, void* ctx)
{
    (void)ctx;
    uint64_t base = (uint64_t)window * BLOCK_MAP_WINDOW_BLOCKS;
    uint64_t stop = base + BLOCK_MAP_WINDOW_BLOCKS;
    memset(bits, 0, BLOCK_MAP_WINDOW_BLOCKS / 8u);

    uint32_t kept = 0;
    for (uint32_t i = 0; i < s_carried_count; i++)
        if (RunLay(&s_runs[s_carried[i]], base, bits))
            s_carried[kept++] = s_carried[i];
    s_carried_count = kept;

    for (; s_next < s_count && s_runs[s_next].start < stop; s_next++)
        if (RunLay(&s_runs[s_next], base, bits))
            s_carried[s_carried_count++] = s_next;
}

error_t GroundCensusRun(BlockMapOrphanFn orphan, BlockMapCensusCount* out)
{
    if (s_refused)
        return BlockMapCensus(NULL, NULL, NULL, out);

    /* A run is carried at most once at a time, so the carried list never
     * holds more than there are runs. */
    s_carried = s_count ? kmalloc(sizeof(uint32_t) * s_count) : NULL;
    if (s_count && !s_carried) {
        kprintf("[TagFS FSCK] no memory to sweep %u owner run(s) past the block map — this "
                "mount does not hold its block map against its owners\n", s_count);
        return BlockMapCensus(NULL, NULL, NULL, out);
    }
    RunsSort();
    RunsSayDoubles(false);
    s_next          = 0;
    s_carried_count = 0;
    error_t rc = BlockMapCensus(GroundOwns, orphan, NULL, out);
    kfree(s_carried);
    s_carried = NULL;
    return rc;
}

/* The held claims (`held`), or every other exclusive one, sorted — the
 * claims are — and laid end to end with no two overlapping, into a new array
 * (*n of them): a run that meets the one before is joined to it while the
 * join still counts in a u32, and one that overlaps begins where it ends.
 * NULL with no memory. */
static GroundRun* RunsMerged(bool held, uint32_t* n)
{
    GroundRun* out = kmalloc(sizeof(GroundRun) * (s_count ? s_count : 1u));
    *n = 0;
    if (!out)
        return NULL;
    for (uint32_t i = 0; i < s_count; i++) {
        const GroundRun* r    = &s_runs[i];
        uint64_t         from = r->start, end = r->start + r->count;
        if ((r->owner == GROUND_HELD) != held || (!held && !g_owner_alone[r->owner]))
            continue;
        GroundRun* last = *n ? &out[*n - 1u] : NULL;
        if (last) {
            uint64_t reach = last->start + last->count;
            if (end <= reach)
                continue;
            if (from < reach)
                from = reach;
            if (from == reach && reach - last->start + (end - from) <= UINT32_MAX) {
                last->count += (uint32_t)(end - from);
                continue;
            }
        }
        out[(*n)++] = (GroundRun){ from, 0, (uint32_t)(end - from), (uint32_t)GROUND_HELD };
    }
    return out;
}

/* The held runs less every block an owner names, laid back into the claims in
 * place of the held runs they were: the blocks held. */
static uint64_t RunsHoldLess(const GroundRun* want, uint32_t nwant, const GroundRun* own, uint32_t nown)
{
    uint32_t kept = 0, o = 0;
    uint64_t held = 0;
    for (uint32_t i = 0; i < s_count; i++)
        if (s_runs[i].owner != GROUND_HELD)
            s_runs[kept++] = s_runs[i];
    s_count = kept;
    for (uint32_t w = 0; w < nwant; w++) {
        uint64_t at = want[w].start, end = want[w].start + want[w].count;
        while (o < nown && own[o].start + own[o].count <= at)
            o++;
        for (uint32_t k = o; at < end; k++) {
            uint64_t stop = k < nown && own[k].start < end ? own[k].start : end;
            if (stop > at) {
                s_runs[s_count++] = (GroundRun){ at, 0, (uint32_t)(stop - at), (uint32_t)GROUND_HELD };
                held += stop - at;
            }
            if (k >= nown || own[k].start >= end)
                break;
            at = own[k].start + own[k].count > at ? own[k].start + own[k].count : at;
        }
    }
    return held;
}

bool GroundClaimedBy(GroundOwner owner, uint64_t start, uint64_t count)
{
    for (uint32_t i = 0; i < s_count; i++)
        if (s_runs[i].owner == (uint32_t)owner && s_runs[i].start < start + count &&
            start < s_runs[i].start + s_runs[i].count)
            return true;
    return false;
}

error_t GroundCensusBook(uint64_t* held)
{
    *held = 0;
    if (s_refused)
        return ERR_NO_MEMORY;
    RunsSort();
    uint32_t   nwant = 0, nown = 0;
    GroundRun* want  = RunsMerged(true, &nwant);
    GroundRun* own   = RunsMerged(false, &nown);
    if (!want || !own) {
        kfree(want);
        kfree(own);
        kprintf("[TagFS FSCK] no memory to hold %u owner run(s) against each other — a book is not "
                "mounted on a census it could not take\n", s_count);
        return ERR_NO_MEMORY;
    }
    /* A held run is cut at most once by each owned run, so the claims never
     * grow past the room they had: the held runs out, their pieces in. */
    if (s_count + nown > s_cap) {
        GroundRun* more = kmalloc(sizeof(GroundRun) * (s_count + nown));
        if (!more) {
            kfree(want);
            kfree(own);
            return ERR_NO_MEMORY;
        }
        memcpy(more, s_runs, sizeof(GroundRun) * s_count);
        kfree(s_runs);
        s_runs = more;
        s_cap  = s_count + nown;
    }
    *held = RunsHoldLess(want, nwant, own, nown);
    kfree(want);
    kfree(own);
    RunsSort();
    RunsSayDoubles(true);
    return OK;
}

#ifdef CONFIG_VOLUME_TESTS
GroundOwner GroundClaimOf(uint64_t block)
{
    for (uint32_t i = 0; i < s_count; i++)
        if (block >= s_runs[i].start && block - s_runs[i].start < s_runs[i].count)
            return (GroundOwner)s_runs[i].owner;
    return GROUND_OWNER_KINDS;
}
#endif

void GroundCensusEnd(void)
{
    kfree(s_runs);
    kfree(s_carried);
    s_runs          = NULL;
    s_carried       = NULL;
    s_count         = 0;
    s_cap           = 0;
    s_refused       = false;
    s_next          = 0;
    s_carried_count = 0;
}
