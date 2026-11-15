#include "ground_census_internal.h"
#include "../ink/commonhold.h"

/*
 * Commonhold counted from the census's claims (GroundSharesCounted) — the one
 * thing a mount derives it from: no tree of it lies on the medium.
 *
 * Every claim of a file — its DATA, its runs SHARED — and of the held that
 * meets a block a run SHARED names is cut into its two edges, and the edges
 * swept in order: between two edges, the files' runs over the blocks are as
 * many owners each, and the held one owner more however many claims it lays
 * there — a rotten range's blocks and a locked file's are what no name that
 * reads owns, and a block in common is kept for them until an explicit act
 * lets them go. A volume no run SHARED names costs one walk of the claims.
 */

typedef struct {
    uint64_t at;
    uint32_t owner;
    int32_t  step;
} ShareEdge;

static int EdgeOrder(const void* a, const void* b, void* ctx)
{
    uint64_t x = ((const ShareEdge*)a)->at, y = ((const ShareEdge*)b)->at;
    (void)ctx;
    return x < y ? -1 : x > y;
}

/* A span of the ground runs SHARED claim. */
typedef struct {
    uint64_t start;
    uint64_t end;
} ShareSpan;

static int SpanOrder(const void* a, const void* b, void* ctx)
{
    uint64_t x = ((const ShareSpan*)a)->start, y = ((const ShareSpan*)b)->start;
    (void)ctx;
    return x < y ? -1 : x > y;
}

/* The ground the runs SHARED claim, in order, the runs that meet joined, into
 * a new array (*ns): NULL with *ns 0 for none; NULL with *ns 1 for no memory. */
static ShareSpan* SharesLaid(const GroundRun* runs, uint32_t n, uint64_t* ns)
{
    uint64_t m = 0;
    *ns = 0;
    for (uint32_t i = 0; i < n; i++)
        m += runs[i].owner == GROUND_SHARED;
    if (m == 0)
        return NULL;
    ShareSpan* s = kmalloc(sizeof(ShareSpan) * m);
    if (!s) {
        *ns = 1;
        return NULL;
    }
    m = 0;
    for (uint32_t i = 0; i < n; i++)
        if (runs[i].owner == GROUND_SHARED)
            s[m++] = (ShareSpan){ runs[i].start, runs[i].start + runs[i].count };
    KSort(s, m, sizeof(ShareSpan), SpanOrder, NULL);
    for (uint64_t i = 0; i < m; i++) {
        ShareSpan* last = *ns ? &s[*ns - 1u] : NULL;
        if (last && s[i].start <= last->end) {
            last->end = s[i].end > last->end ? s[i].end : last->end;
            continue;
        }
        s[(*ns)++] = s[i];
    }
    return s;
}

/* Whether [start, end) meets the ground `s` (`ns` spans, in order, none meeting). */
static bool SharesMeet(const ShareSpan* s, uint64_t ns, uint64_t start, uint64_t end)
{
    uint64_t lo = 0, hi = ns;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2u;
        if (s[mid].end <= start)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo < ns && s[lo].start < end;
}

/* The count's runs, as the sweep finds them: the one in hand grown while the
 * next goes on from it with the same owners, added once it does not. */
typedef struct {
    uint64_t start;
    uint64_t count;
    uint32_t owners;
    error_t  rc;
} SharePiece;

static void PieceFound(SharePiece* p, uint64_t start, uint64_t end, uint32_t owners)
{
    if (p->count && p->start + p->count == start && p->owners == owners) {
        p->count += end - start;
        return;
    }
    if (p->count && p->rc == OK)
        p->rc = CommonholdMountAdd(p->start, p->count, p->owners);
    *p = (SharePiece){ start, end - start, owners, p->rc };
}

/* The edges of every claim of a file or the held that meets the ground
 * `s` (`ns`) into `e`: how many. */
static uint64_t EdgesOf(const GroundRun* runs, uint32_t n, const ShareSpan* s, uint64_t ns, ShareEdge* e)
{
    uint64_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t owner = runs[i].owner;
        uint64_t end   = runs[i].start + runs[i].count;
        if ((owner != GROUND_FILE && owner != GROUND_SHARED && owner != GROUND_HELD) ||
            !SharesMeet(s, ns, runs[i].start, end))
            continue;
        e[k++] = (ShareEdge){ runs[i].start, owner, 1 };
        e[k++] = (ShareEdge){ end, owner, -1 };
    }
    return k;
}

error_t GroundSharesCounted(void)
{
    uint32_t   n    = 0;
    uint64_t   ns   = 0;
    GroundRun* runs = GroundCensusRuns(&n);
    CommonholdLetGoAll();
    ShareSpan* s    = SharesLaid(runs, n, &ns);
    ShareEdge* e    = s ? kmalloc(sizeof(ShareEdge) * 2u * (uint64_t)n) : NULL;
    if (!s && ns == 0)
        return OK;
    if (!e) {
        kfree(s);
        kprintf("[TagFS FSCK] no memory to count the owners of the blocks runs SHARED name — a book is not "
                "mounted on a census it could not take\n");
        return ERR_NO_MEMORY;
    }
    uint64_t   k        = EdgesOf(runs, n, s, ns, e);
    int64_t    files    = 0, shared = 0, held = 0;
    SharePiece p        = { 0, 0, 0, OK };
    KSort(e, k, sizeof(ShareEdge), EdgeOrder, NULL);
    for (uint64_t i = 0; i < k; i++) {
        if (i && e[i].at > e[i - 1u].at && shared > 0)
            PieceFound(&p, e[i - 1u].at, e[i].at, (uint32_t)(files + shared + (held > 0)));
        int64_t* c = e[i].owner == GROUND_FILE ? &files : e[i].owner == GROUND_SHARED ? &shared : &held;
        *c += e[i].step;
    }
    if (p.count && p.rc == OK)
        p.rc = CommonholdMountAdd(p.start, p.count, p.owners);
    kfree(e);
    kfree(s);
    if (p.rc != OK)
        kprintf("[TagFS FSCK] no memory to keep the owners of the blocks runs SHARED name — a book is not "
                "mounted on a census it could not take\n");
    return p.rc;
}
