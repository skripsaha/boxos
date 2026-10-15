#include "ground_census.h"
#include "../block_map/block_map_derived.h"

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
    uint32_t namer;     /* the Spine slot that gives it, + 1; 0 when no slot does */
} GroundRun;

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

static void GroundClaimAs(GroundOwner owner, uint64_t start, uint32_t count, uint64_t who, uint32_t namer)
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
    s_runs[s_count++] = (GroundRun){ start, who, count, (uint32_t)owner, namer };
}

void GroundClaim(GroundOwner owner, uint64_t start, uint32_t count, uint64_t who)
{
    GroundClaimAs(owner, start, count, who, 0);
}

void GroundClaimBySlot(GroundOwner owner, uint64_t start, uint32_t count, uint64_t who, uint32_t slot)
{
    GroundClaimAs(owner, start, count, who, slot < UINT32_MAX ? slot + 1u : UINT32_MAX);
}

/* Whether run a sorts after run b: by start — and `by_name`, then by the
 * rest of its name, so that one name given by several slots lies in a row. */
static bool RunAfter(const GroundRun* a, const GroundRun* b, bool by_name)
{
    if (a->start != b->start || !by_name)
        return a->start > b->start;
    if (a->count != b->count)
        return a->count > b->count;
    if (a->owner != b->owner)
        return a->owner > b->owner;
    if (a->who != b->who)
        return a->who > b->who;
    return a->namer > b->namer;
}

/* Heapsort: in place, and no deeper than one frame whatever the number of
 * runs. */
static void RunSiftDown(uint32_t at, uint32_t n, bool by_name)
{
    for (;;) {
        uint32_t big = at, l = 2u * at + 1u, r = l + 1u;
        if (l < n && RunAfter(&s_runs[l], &s_runs[big], by_name))
            big = l;
        if (r < n && RunAfter(&s_runs[r], &s_runs[big], by_name))
            big = r;
        if (big == at)
            return;
        GroundRun t  = s_runs[at];
        s_runs[at]   = s_runs[big];
        s_runs[big]  = t;
        at           = big;
    }
}

static void RunsSort(bool by_name)
{
    for (uint32_t i = s_count / 2u; i-- > 0; )
        RunSiftDown(i, s_count, by_name);
    for (uint32_t n = s_count; n > 1; n--) {
        GroundRun t = s_runs[0];
        s_runs[0]   = s_runs[n - 1];
        s_runs[n - 1] = t;
        RunSiftDown(0, n - 1, by_name);
    }
}

/* Whether two runs are one name but for the slot that gives it. */
static bool RunsOneNameOf(const GroundRun* a, const GroundRun* b)
{
    return a->start == b->start && a->count == b->count && a->owner == b->owner && a->who == b->who;
}

/*
 * The runs sorted by name, each name given by several slots kept as many
 * times as the slot that gives it most — and as many times as no slot gives
 * it — the rest let go, in place (GroundCensusBook).
 */
static void RunsOneName(void)
{
    uint32_t kept = 0;
    for (uint32_t i = 0; i < s_count; ) {
        uint32_t end = i, unnamed = 0, most = 0;
        while (end < s_count && RunsOneNameOf(&s_runs[i], &s_runs[end]))
            end++;
        for (uint32_t j = i; j < end; ) {
            uint32_t k = j;
            while (k < end && s_runs[k].namer == s_runs[j].namer)
                k++;
            if (s_runs[j].namer == 0)
                unnamed += k - j;
            else if (k - j > most)
                most = k - j;
            j = k;
        }
        for (uint32_t j = 0; j < unnamed + most; j++)
            s_runs[kept++] = s_runs[i + j];
        i = end;
    }
    s_count = kept;
}

/* An owner as the lines name it: a file by its id, any other by its kind. */
static void OwnerSay(uint32_t owner, uint64_t who, char* out, size_t room)
{
    if (owner == GROUND_FILE)
        ksnprintf(out, room, "file %llu", (unsigned long long)who);
    else
        ksnprintf(out, room, "the %s", GroundOwnerName((GroundOwner)owner));
}

static void RunSay(const GroundRun* r, char* out, size_t room)
{
    OwnerSay(r->owner, r->who, out, room);
}

/*
 * Two owners of one block: every exclusive run that begins before the
 * furthest end an exclusive run before it reached. The first 32 are named and
 * the rest counted — the owner's bound for a list said at mount (09-27).
 */
static void RunsSayDoubles(void)
{
    uint64_t reach = 0;
    uint32_t reach_run = 0, named = 0, more = 0;
    bool     have  = false;
    char     a[32], b[32];

    for (uint32_t i = 0; i < s_count; i++) {
        const GroundRun* r = &s_runs[i];
        if (!g_owner_alone[r->owner])
            continue;
        if (have && r->start < reach) {
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

/*
 * Format v2's census says two owners of one block as the blocks the two
 * name together: a meeting is the part of a run the furthest reach before it
 * covers, put to the pair of owners met — each a file by its id, any other by
 * its kind — and the meetings of one pair that go on without a gap are one
 * run of blocks, said once by its first block and its length, as
 * volume-read's census says them (bookcensus.py: a DOUBLE_OWNER by the owners'
 * labels, its blocks merged). A node whose pointer names ground other nodes
 * lie on is one finding, not one for each node under it.
 */
typedef struct {
    uint64_t start;
    uint64_t end;
    uint64_t who[2];
    uint32_t owner[2];
} GroundMeeting;

static bool MeetingSamePair(const GroundMeeting* a, const GroundMeeting* b)
{
    return a->owner[0] == b->owner[0] && a->who[0] == b->who[0] && a->owner[1] == b->owner[1] &&
           a->who[1] == b->who[1];
}

static void MeetingSay(const GroundMeeting* m, uint32_t* named, uint32_t* more)
{
    char a[32], b[32];
    if (*named >= 32u) {
        (*more)++;
        return;
    }
    (*named)++;
    OwnerSay(m->owner[0], m->who[0], a, sizeof(a));
    OwnerSay(m->owner[1], m->who[1], b, sizeof(b));
    kprintf("[TagFS FSCK] data block %llu is named by %s and %s at once, %llu block(s) from it — the "
            "medium kept less than a settled cycle said, deeper than the rest a freed block takes covers, "
            "or a writer gave one block twice\n", (unsigned long long)m->start, a, b,
            (unsigned long long)(m->end - m->start));
}

/* Run r meeting run `at` on [r's start, reach): the pair in one order, a
 * file's who kept and any other owner's let go — as the lines name them. */
static GroundMeeting MeetingOf(const GroundRun* at, const GroundRun* r, uint64_t reach)
{
    uint64_t      end = r->start + r->count < reach ? r->start + r->count : reach;
    uint64_t      wa  = at->owner == GROUND_FILE ? at->who : 0, wb = r->owner == GROUND_FILE ? r->who : 0;
    bool          swap = at->owner > r->owner || (at->owner == r->owner && wa > wb);
    GroundMeeting m   = { r->start, end, { swap ? wb : wa, swap ? wa : wb },
                          { swap ? r->owner : at->owner, swap ? at->owner : r->owner } };
    return m;
}

static void RunsSayDoublesBook(void)
{
    GroundMeeting* open  = kmalloc(sizeof(GroundMeeting) * (s_count ? s_count : 1u));
    uint64_t       reach = 0;
    uint32_t       reach_run = 0, nopen = 0, named = 0, more = 0;
    bool           have  = false;

    if (!open) {
        kprintf("[TagFS FSCK] no memory to join the blocks two owners name into runs — each is named as "
                "its run meets another\n");
        RunsSayDoubles();
        return;
    }
    for (uint32_t i = 0; i < s_count; i++) {
        const GroundRun* r = &s_runs[i];
        if (!g_owner_alone[r->owner])
            continue;
        if (have && r->start < reach) {
            GroundMeeting m    = MeetingOf(&s_runs[reach_run], r, reach);
            uint32_t      kept = 0, k;
            for (k = 0; k < nopen; k++) {
                if (open[k].end < m.start)
                    MeetingSay(&open[k], &named, &more);
                else
                    open[kept++] = open[k];
            }
            nopen = kept;
            for (k = 0; k < nopen && !MeetingSamePair(&open[k], &m); k++)
                ;
            if (k < nopen)
                open[k].end = m.end > open[k].end ? m.end : open[k].end;
            else
                open[nopen++] = m;
        }
        if (!have || r->start + r->count > reach) {
            reach     = r->start + r->count;
            reach_run = i;
            have      = true;
        }
    }
    for (uint32_t k = 0; k < nopen; k++)
        MeetingSay(&open[k], &named, &more);
    if (more)
        kprintf("[TagFS FSCK] and %u more run(s) of blocks named by two owners at once\n", more);
    kfree(open);
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
    RunsSort(false);
    RunsSayDoubles();
    s_next          = 0;
    s_carried_count = 0;
    error_t rc = BlockMapCensus(GroundOwns, orphan, NULL, out);
    kfree(s_carried);
    s_carried = NULL;
    return rc;
}

/* The claims of `want` — or, for GROUND_OWNER_KINDS, every exclusive one but
 * the held and `apart` — sorted, as the claims are, and laid end to end with
 * no two overlapping, into a new array (*n of them): a run that meets the one
 * before is joined to it while the join still counts in a u32, and one that
 * overlaps begins where it ends. NULL with no memory. */
static GroundRun* RunsMerged(GroundOwner want, GroundOwner apart, uint32_t* n)
{
    GroundRun* out = kmalloc(sizeof(GroundRun) * (s_count ? s_count : 1u));
    *n = 0;
    if (!out)
        return NULL;
    for (uint32_t i = 0; i < s_count; i++) {
        const GroundRun* r    = &s_runs[i];
        uint64_t         from = r->start, end = r->start + r->count;
        if (want != GROUND_OWNER_KINDS ? r->owner != (uint32_t)want
                                       : (r->owner == GROUND_HELD || r->owner == (uint32_t)apart ||
                                          !g_owner_alone[r->owner]))
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
        out[(*n)++] = (GroundRun){ from, 0, (uint32_t)(end - from), (uint32_t)want, 0 };
    }
    return out;
}

/* The claims of `owner` less every block an owner in `own` names, laid back
 * into the claims in place of the runs they were: the blocks left. */
static uint64_t RunsLess(GroundOwner owner, const GroundRun* want, uint32_t nwant, const GroundRun* own,
                         uint32_t nown)
{
    uint32_t kept = 0, o = 0;
    uint64_t left = 0;
    for (uint32_t i = 0; i < s_count; i++)
        if (s_runs[i].owner != (uint32_t)owner)
            s_runs[kept++] = s_runs[i];
    s_count = kept;
    for (uint32_t w = 0; w < nwant; w++) {
        uint64_t at = want[w].start, end = want[w].start + want[w].count;
        while (o < nown && own[o].start + own[o].count <= at)
            o++;
        for (uint32_t k = o; at < end; k++) {
            uint64_t stop = k < nown && own[k].start < end ? own[k].start : end;
            if (stop > at) {
                s_runs[s_count++] = (GroundRun){ at, 0, (uint32_t)(stop - at), (uint32_t)owner, 0 };
                left += stop - at;
            }
            if (k >= nown || own[k].start >= end)
                break;
            at = own[k].start + own[k].count > at ? own[k].start + own[k].count : at;
        }
    }
    return left;
}

/* The claims of `owner` — what may be someone's and is given to no one —
 * cut to the blocks no other owner names: those of every exclusive owner but
 * the held and `apart`. How many blocks are left in *left. ERR_NO_MEMORY. */
static error_t RunsOwnerLess(GroundOwner owner, GroundOwner apart, uint64_t* left)
{
    uint32_t   nwant = 0, nown = 0;
    GroundRun* want;
    GroundRun* own;
    RunsSort(false);
    want = RunsMerged(owner, apart, &nwant);
    own  = RunsMerged(GROUND_OWNER_KINDS, apart, &nown);
    /* A run left is cut at most once by each owned run, so the claims never
     * grow past the room they had: the runs out, their pieces in. */
    if (want && own && s_count + nown > s_cap) {
        GroundRun* more = kmalloc(sizeof(GroundRun) * (s_count + nown));
        if (more) {
            memcpy(more, s_runs, sizeof(GroundRun) * s_count);
            kfree(s_runs);
            s_runs = more;
            s_cap  = s_count + nown;
        }
    }
    if (!want || !own || s_count + nown > s_cap) {
        kfree(want);
        kfree(own);
        kprintf("[TagFS FSCK] no memory to hold %u owner run(s) against each other — a book is not "
                "mounted on a census it could not take\n", s_count);
        return ERR_NO_MEMORY;
    }
    *left = RunsLess(owner, want, nwant, own, nown);
    kfree(want);
    kfree(own);
    return OK;
}

bool GroundClaimedBy(GroundOwner owner, uint64_t start, uint64_t count)
{
    for (uint32_t i = 0; i < s_count; i++)
        if (s_runs[i].owner == (uint32_t)owner && s_runs[i].start < start + count &&
            start < s_runs[i].start + s_runs[i].count)
            return true;
    return false;
}

/* [from, end) laid into the block map under run `by`'s name. */
static error_t RunsLay(const GroundRun* by, uint64_t from, uint64_t end)
{
    uint64_t taken = 0;
    error_t  rc    = BlockMapDerivedClaim((GroundOwner)by->owner, by->who, from, end - from, &taken);
#if CONFIG_STRICT
    if (taken)
        panic("[TagFS] DEFECT: the census laid a claim into the block map over %llu block(s) a claim "
              "before it holds", (unsigned long long)taken);
#endif
    return rc;
}

/*
 * Every claim laid into the block map, sorted as they are: the part of each
 * past the furthest any claim before it reached, so the map meets no block
 * twice — the census named every two owners of one already. Parts that touch
 * are laid as one, so a window costs the map as many claims as it has gaps,
 * not as many as it has owners: thousands of small files back to back are one
 * claim. The joined claim goes under its first run's name — the map meets
 * nothing at its build (the parts lie apart, and it begins free), so that
 * name is never said; STRICT stops at a build that would say one.
 */
static error_t RunsDerive(const char** what)
{
    error_t  rc    = BlockMapDerivedBegin(tagfs_get_state()->block_end, what);
    uint64_t reach = 0, from = 0, to = 0;
    uint32_t by    = 0;
    for (uint32_t i = 0; rc == OK && i < s_count; i++) {
        uint64_t start = s_runs[i].start > reach ? s_runs[i].start : reach;
        uint64_t end   = s_runs[i].start + s_runs[i].count;
        if (end <= start)
            continue;
        reach = end;
        if (to && start == to) {
            to = end;
            continue;
        }
        if (to)
            rc = RunsLay(&s_runs[by], from, to);
        by   = i;
        from = start;
        to   = end;
    }
    if (rc == OK && to)
        rc = RunsLay(&s_runs[by], from, to);
    if (rc == OK)
        return BlockMapDerivedBuilt(what);
    BlockMapDerivedLetGo();
    return rc;
}

error_t GroundCensusBook(uint64_t* held, uint64_t* fallow, bool derive, const char** what)
{
    *held   = 0;
    *fallow = 0;
    *what   = "there is no memory to hold its census";
    if (s_refused)
        return ERR_NO_MEMORY;
    /* The ground the ribbon let go is a name's only where no living owner is
     * (M-1), and what is held for a rot only where no owner at all is — the
     * fallow ground among them (bookcensus.py _Claims, Owned). */
    error_t rc = RunsOwnerLess(GROUND_FALLOW, GROUND_FALLOW, fallow);
    if (rc == OK)
        rc = RunsOwnerLess(GROUND_HELD, GROUND_HELD, held);
    if (rc != OK)
        return rc;
    RunsSort(true);
    RunsOneName();
    RunsSayDoublesBook();
    return derive ? RunsDerive(what) : OK;
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
