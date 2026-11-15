#include "ground_census_internal.h"
#include "../block_map/block_map_derived.h"

/*
 * The census (ground_census.h). The claims are a list of runs, sorted once by
 * where they start, and walked in that order: two exclusive owners of one
 * block are found where their runs meet.
 */

static const bool g_owner_alone[GROUND_OWNER_KINDS] = {
#define GROUND_OWNER_ALONE(id, name, exclusive) [id] = (exclusive) != 0,
    GROUND_OWNERS(GROUND_OWNER_ALONE)
#undef GROUND_OWNER_ALONE
};

/* The mount's alone (the door is shut). */
static GroundRun* s_runs;
static uint32_t   s_count;
static uint32_t   s_cap;
static bool       s_refused;       /* a claim found no memory: the census decides nothing */
static uint64_t   s_reach;         /* past the farthest block an owner but the far Deed names */
static uint64_t   s_doubled;       /* blocks the census said two owners name */

void GroundCensusBegin(void)
{
    GroundCensusEnd();
}

/* How far a claim reaches: all of the volume, its far Deed's block among them
 * — every block of it is some owner's (ground_owner.h). */
static uint64_t ClaimEnd(void)
{
    return tagfs_get_state()->volume_bytes / TAGFS_BLOCK_SIZE;
}

static void GroundClaimAs(GroundOwner owner, uint64_t start, uint32_t count, uint64_t who, uint32_t namer)
{
    uint64_t run = ClaimEnd();
    if (owner != GROUND_FAR_DEED)
        GroundCensusNamed(start, count);
    if (s_refused || count == 0 || start >= run || owner >= GROUND_OWNER_KINDS)
        return;
    if (count > run - start)
        count = (uint32_t)(run - start);

    if (s_count == s_cap) {
        uint32_t   cap  = s_cap ? s_cap * 2u : 64u;
        GroundRun* more = kmalloc(sizeof(GroundRun) * cap);
        if (!more) {
            kprintf("[TagFS FSCK] no memory to hold the ground of %u owner run(s) — a book is "
                    "not mounted on a census it could not take\n", s_count + 1);
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

/* Whether an owner of kind `owner` is a file's — its DATA, or a run SHARED. */
static bool OwnerIsFile(uint32_t owner)
{
    return owner == GROUND_FILE || owner == GROUND_SHARED;
}

/* An owner as the lines name it: a file by its id, any other by its kind. */
static void OwnerSay(uint32_t owner, uint64_t who, char* out, size_t room)
{
    if (OwnerIsFile(owner))
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
    s_doubled += m->end - m->start;
    OwnerSay(m->owner[0], m->who[0], a, sizeof(a));
    OwnerSay(m->owner[1], m->who[1], b, sizeof(b));
    kprintf("[TagFS FSCK] data block %llu is named by %s and %s at once, %llu block(s) from it — the "
            "medium kept less than a settled cycle said, deeper than the rest a freed block takes covers, "
            "or a writer gave one block twice\n", (unsigned long long)m->start, a, b,
            (unsigned long long)(m->end - m->start));
}

/* Run r meeting run `at` on [r's start, reach): the pair in one order, a
 * file's who kept and any other owner's let go — as the lines name them, a
 * file's DATA and its runs SHARED one owner. */
static GroundMeeting MeetingOf(const GroundRun* at, const GroundRun* r, uint64_t reach)
{
    uint64_t      end = r->start + r->count < reach ? r->start + r->count : reach;
    uint64_t      wa  = OwnerIsFile(at->owner) ? at->who : 0, wb = OwnerIsFile(r->owner) ? r->who : 0;
    uint32_t      oa  = OwnerIsFile(at->owner) ? GROUND_FILE : at->owner;
    uint32_t      ob  = OwnerIsFile(r->owner) ? GROUND_FILE : r->owner;
    bool          swap = oa > ob || (oa == ob && wa > wb);
    GroundMeeting m   = { r->start, end, { swap ? wb : wa, swap ? wa : wb }, { swap ? ob : oa, swap ? oa : ob } };
    return m;
}

/* The meetings said so far, joined pair by pair while they go on without a
 * gap: they come in order of where they begin, so one that ends before the
 * next begins is said then. */
typedef struct {
    GroundMeeting* open;
    uint32_t       nopen;
    uint32_t       named;
    uint32_t       more;
} GroundMeetings;

static void MeetingJoined(GroundMeetings* g, GroundMeeting m)
{
    uint32_t kept = 0, k;
    for (k = 0; k < g->nopen; k++) {
        if (g->open[k].end < m.start)
            MeetingSay(&g->open[k], &g->named, &g->more);
        else
            g->open[kept++] = g->open[k];
    }
    g->nopen = kept;
    for (k = 0; k < g->nopen && !MeetingSamePair(&g->open[k], &m); k++)
        ;
    if (k < g->nopen)
        g->open[k].end = m.end > g->open[k].end ? m.end : g->open[k].end;
    else
        g->open[g->nopen++] = m;
}

/* The furthest a sweep's runs of one class reached, and the run that did. */
typedef struct {
    uint64_t reach;
    uint32_t run;
    bool     have;
} GroundReach;

static void ReachMoved(GroundReach* r, uint32_t i, uint64_t end)
{
    if (!r->have || end > r->reach) {
        r->reach = end;
        r->run   = i;
        r->have  = true;
    }
}

/*
 * Two owners of one block, swept once in order of where the claims begin:
 * every exclusive run that begins inside the furthest exclusive run before
 * it; and — ‼ A RUN SHARED NAMES A FILE'S GROUND, NEVER ANOTHER OWNER'S (§8.2)
 * — a run SHARED that begins inside the furthest exclusive run of another
 * kind than a file's before it, and such a run that begins inside the
 * furthest run SHARED before it: a run SHARED lies over the one DATA run of
 * its block, or over none, beside other runs SHARED, and on a node, a Quire,
 * a slot, the front or the far Deed it is two owners of one block
 * (bookcensus.py _Sweep, GroundClaim.Allows). Room for one meeting a claim.
 */
static void RunsSayDoublesBook(void)
{
    GroundMeetings g  = { kmalloc(sizeof(GroundMeeting) * (s_count ? 2u * s_count : 1u)), 0, 0, 0 };
    GroundReach    ex = { 0, 0, false }, other = { 0, 0, false }, shared = { 0, 0, false };

    if (!g.open) {
        kprintf("[TagFS FSCK] no memory to join the blocks two owners name into runs — each is named as "
                "its run meets another\n");
        RunsSayDoubles();
        return;
    }
    for (uint32_t i = 0; i < s_count; i++) {
        const GroundRun* r     = &s_runs[i];
        uint64_t         end   = r->start + r->count;
        bool             alone = g_owner_alone[r->owner], foreign = alone && r->owner != GROUND_FILE;
        if (alone && ex.have && r->start < ex.reach)
            MeetingJoined(&g, MeetingOf(&s_runs[ex.run], r, ex.reach));
        if (r->owner == GROUND_SHARED && other.have && r->start < other.reach)
            MeetingJoined(&g, MeetingOf(&s_runs[other.run], r, other.reach));
        if (foreign && shared.have && r->start < shared.reach)
            MeetingJoined(&g, MeetingOf(&s_runs[shared.run], r, shared.reach));
        if (alone)
            ReachMoved(&ex, i, end);
        if (foreign)
            ReachMoved(&other, i, end);
        if (r->owner == GROUND_SHARED)
            ReachMoved(&shared, i, end);
    }
    for (uint32_t k = 0; k < g.nopen; k++)
        MeetingSay(&g.open[k], &g.named, &g.more);
    if (g.more)
        kprintf("[TagFS FSCK] and %u more run(s) of blocks named by two owners at once\n", g.more);
    kfree(g.open);
}

/* The claims of `want` — or, for GROUND_OWNER_KINDS, every exclusive one but
 * the held and `apart`, and every run SHARED: an owner, though not alone
 * (ground_owner.h) — sorted, as the claims are, and laid end to end with
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
                                          (!g_owner_alone[r->owner] && r->owner != GROUND_SHARED)))
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

uint64_t GroundClaimsOf(GroundOwner owner, Strip* out, uint64_t room)
{
    uint64_t n = 0;
    for (uint32_t i = 0; i < s_count; i++) {
        if (s_runs[i].owner != (uint32_t)owner)
            continue;
        if (n < room)
            out[n] = (Strip){ .start = s_runs[i].start, .count = s_runs[i].count, .kind = STRIP_DATA };
        n++;
    }
    return n;
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

/* The Quires of `quire` blocks the Tallies' node claims lie in — on the grid
 * of the volume's blocks — those that meet joined, in order: *n of them, into
 * a new array; NULL with no memory. The claims sorted by their first block. */
static GroundRun* TallyQuires(uint64_t quire, uint32_t* n)
{
    GroundRun* out = kmalloc(sizeof(GroundRun) * (s_count ? s_count : 1u));
    *n = 0;
    for (uint32_t i = 0; out && i < s_count; i++) {
        if (s_runs[i].owner != (uint32_t)GROUND_TALLY)
            continue;
        uint64_t from = s_runs[i].start / quire * quire;
        uint64_t end  = (s_runs[i].start + s_runs[i].count + quire - 1u) / quire * quire;
        GroundRun* last = *n ? &out[*n - 1u] : NULL;
        if (last && from <= last->start + last->who) {
            last->who = end > last->start + last->who ? end - last->start : last->who;
            continue;
        }
        out[(*n)++] = (GroundRun){ from, end - from, 0, (uint32_t)GROUND_TALLY, 0 };
    }
    return out;
}

/* The blocks of the Quires `q` (*nq, each run's length in its `who`) no claim
 * names, as runs into `gaps` (room for *nq + s_count): how many. */
static uint32_t QuireGaps(const GroundRun* q, uint32_t nq, GroundRun* gaps)
{
    uint32_t r = 0, n = 0;
    uint64_t reach = 0;
    for (uint32_t i = 0; i < nq; i++) {
        uint64_t at = q[i].start, end = q[i].start + q[i].who;
        while (at < end) {
            while (r < s_count && s_runs[r].start <= at) {
                if (s_runs[r].start + s_runs[r].count > reach)
                    reach = s_runs[r].start + s_runs[r].count;
                r++;
            }
            if (reach > at) {
                at = reach;
                continue;
            }
            uint64_t stop = r < s_count && s_runs[r].start < end ? s_runs[r].start : end;
            gaps[n++] = (GroundRun){ at, stop - at, 0, (uint32_t)GROUND_TALLY, 0 };
            at = stop;
        }
    }
    return n;
}

/*
 * ‼ A TALLY-QUIRE IS HELD WHOLE WHILE ONE NODE IN IT LIVES (spec_v2 §16.5
 * п.17, Р-d3 (б)). The Binder takes its Tally-Quires whole on the grid of
 * `quire` (settle/binder.c TallyRunsTake), and a slot holds every one a node
 * of its Tally lies in: so every block of such a Quire that no owner names —
 * a dead node's, the tail no Tally reached — is the Tallies' too, given to no
 * one, and the Quire goes back whole once no node of it lives (§4.8). Only
 * what no claim at all names is taken: a Quire cut on another grid holds
 * nothing an owner holds. One rule with volume-read (bookcensus.py _Quires),
 * and the block log by its census (blklog_book.py). Before the cut of the
 * fallow and the held: every name as claimed. ERR_NO_MEMORY, said.
 */
error_t GroundQuiresHeld(uint64_t quire)
{
    uint32_t nq = 0;
    if (!quire || s_refused)
        return OK;
    RunsSort(false);
    GroundRun* q    = TallyQuires(quire, &nq);
    GroundRun* gaps = q ? kmalloc(sizeof(GroundRun) * ((uint64_t)nq + s_count + 1u)) : NULL;
    if (!gaps) {
        kfree(q);
        kprintf("[TagFS FSCK] no memory to hold the Tallies' Quires whole — a book is not mounted on a census it "
                "could not take\n");
        return ERR_NO_MEMORY;
    }
    uint32_t n = QuireGaps(q, nq, gaps);
    for (uint32_t i = 0; i < n; i++)
        for (uint64_t at = gaps[i].start, left = gaps[i].who; left;) {
            uint32_t c = left > UINT32_MAX ? UINT32_MAX : (uint32_t)left;
            GroundClaimAs(GROUND_TALLY, at, c, 0, 0);
            at   += c;
            left -= c;
        }
    kfree(q);
    kfree(gaps);
    return s_refused ? ERR_NO_MEMORY : OK;
}

error_t GroundCensusBook(uint64_t* held, uint64_t* fallow, bool derive, const char** what)
{
    *held   = 0;
    *fallow = 0;
    *what   = "there is no memory to hold its census";
    if (s_refused)
        return ERR_NO_MEMORY;
    /* How many name each block in common, counted before the cut below
     * takes from the held what a run SHARED names (ground_shares.c). */
    error_t rc = GroundSharesCounted();
    if (rc != OK)
        return rc;
    /* The ground the ribbon let go is a name's only where no living owner is
     * (M-1), and what is held for a rot only where no owner at all is — the
     * fallow ground among them, a run SHARED among the owners (bookcensus.py
     * _Claims, Owned). */
    rc = RunsOwnerLess(GROUND_FALLOW, GROUND_FALLOW, fallow);
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

void GroundCensusNamed(uint64_t start, uint64_t count)
{
    uint64_t end = start + count < start ? UINT64_MAX : start + count;
    if (count && end > s_reach)
        s_reach = end;
}

uint64_t GroundCensusReach(void)
{
    return s_reach;
}

uint64_t GroundCensusDoubled(void)
{
    return s_doubled;
}

GroundRun* GroundCensusRuns(uint32_t* n)
{
    *n = s_count;
    return s_runs;
}

void GroundCensusEnd(void)
{
    kfree(s_runs);
    s_runs    = NULL;
    s_count   = 0;
    s_cap     = 0;
    s_refused = false;
    s_reach   = 0;
    s_doubled = 0;
}
