#include "ground_census.h"

/*
 * The census (ground_census.h). The claims are a list of runs, sorted once by
 * where they start, and swept a window at a time beside the medium's map: a
 * run that crosses the window's end is carried into the next, so every run is
 * looked at once for every window it lies in and never for any other.
 */

typedef struct {
    uint32_t start;
    uint32_t count;
    uint32_t who;
    uint32_t owner;
} GroundRun;

static const char* const g_owner_name[GROUND_OWNER_KINDS] = {
#define GROUND_OWNER_NAME(id, name, exclusive) [id] = name,
    GROUND_OWNERS(GROUND_OWNER_NAME)
#undef GROUND_OWNER_NAME
};

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

/* The sweep: the next run not yet met, and the runs a window before this one
 * carried over its end. */
static uint32_t   s_next;
static uint32_t*  s_carried;
static uint32_t   s_carried_count;

void GroundCensusBegin(void)
{
    GroundCensusEnd();
}

void GroundClaim(GroundOwner owner, uint32_t start, uint32_t count, uint32_t who)
{
    uint32_t run = tagfs_get_state()->layout.data_blocks;
    if (s_refused || count == 0 || start >= run || owner >= GROUND_OWNER_KINDS)
        return;
    if (count > run - start)
        count = run - start;

    if (s_count == s_cap) {
        uint32_t   cap  = s_cap ? s_cap * 2u : 64u;
        GroundRun* more = kmalloc(sizeof(GroundRun) * cap);
        if (!more) {
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
    s_runs[s_count++] = (GroundRun){ start, count, who, (uint32_t)owner };
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
        ksnprintf(out, room, "file %u", r->who);
    else
        ksnprintf(out, room, "the %s", g_owner_name[r->owner]);
}

/*
 * Two owners of one block: every exclusive run that begins before the
 * furthest end an exclusive run before it reached. The first 32 are named and
 * the rest counted — the owner's bound for a list said at mount (09-27).
 */
static void RunsSayDoubles(void)
{
    uint32_t reach = 0, reach_run = 0, named = 0, more = 0;
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
                kprintf("[TagFS FSCK] data block %u is named by %s and %s at once — the "
                        "medium kept less than a settled cycle said, deeper than the rest "
                        "a freed block takes covers, or a writer gave one block twice\n",
                        r->start, a, b);
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
static bool RunLay(const GroundRun* r, uint32_t base, uint8_t* bits)
{
    uint32_t end  = r->start + r->count;
    uint32_t stop = base + BLOCK_MAP_WINDOW_BLOCKS;
    uint32_t from = r->start > base ? r->start : base;
    uint32_t to   = end < stop ? end : stop;
    if (from < to)
        BitsSet(bits, from - base, to - base);
    return end > stop;
}

/* BlockMapOwnsFn: the windows come in order, and every run is met in the
 * window it starts in and carried while it reaches past one. */
static void GroundOwns(uint32_t window, uint8_t* bits, void* ctx)
{
    (void)ctx;
    uint32_t base = window * BLOCK_MAP_WINDOW_BLOCKS;
    uint32_t stop = base + BLOCK_MAP_WINDOW_BLOCKS;
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
    RunsSayDoubles();
    s_next          = 0;
    s_carried_count = 0;
    error_t rc = BlockMapCensus(GroundOwns, orphan, NULL, out);
    kfree(s_carried);
    s_carried = NULL;
    return rc;
}

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
