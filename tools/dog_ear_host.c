/*
 * dog_ear_host — DogEar, the keys of a book's trees changed since the Tally
 * the next one is laid against (spec_v2 §16.5 п.9), held on the host with the
 * kernel's own settle/dog_ear.c and file_map/ (`make dog-ear-host`;
 * tools/dog_ear_host.h stands in for the rest of the kernel): seconds, no
 * machine.
 *
 *   - spans of keys put into DogEar join the ones they meet or overlap, tree
 *     by tree, and a Tally takes what is held: given back when it is refused,
 *     under what came since; let go at its DONE;
 *   - a heap that refuses DogEar room loses it whole — everything is changed —
 *     and demands a Tally, said; the Tally that takes the loss ends it;
 *   - a change of a file puts in exactly the keys of FILES it changed — its
 *     header, and every Parcel of either record not shared at one place of the
 *     file with the other — and exactly the windows of DIGESTS whose ground
 *     one record names and the other does not: a Parcel written over in the
 *     middle, the file grown at its end, a splice that moves every block past
 *     it, a file born and one given up. Each held to the two records walked
 *     Parcel by Parcel and block by block here, apart from the kernel's walk;
 *   - ground let go with no record to say so (Escheat's) puts in every window
 *     of DIGESTS it lies in and no other key: ground ended inside a window,
 *     on its last block, a window's first block alone, none, and the end of
 *     the keys — each key held to the window it lies in, counted here.
 */

#include "../src/kernel/tagfs/settle/dog_ear.h"
#include "tract.h"

ParcelHostHeap  g_parcel_host_heap;
static uint64_t s_fail_at;      /* the allocation that is refused: 1 for the next, 0 for none */
static unsigned s_failed;
static unsigned s_demanded;

void* ParcelHostAlloc(size_t bytes)
{
    if (s_fail_at && --s_fail_at == 0)
        return NULL;
    uint64_t* raw = malloc(bytes + sizeof(uint64_t));
    if (!raw)
        return NULL;
    raw[0] = bytes;
    g_parcel_host_heap.allocs++;
    g_parcel_host_heap.bytes += bytes;
    g_parcel_host_heap.live  += bytes;
    return raw + 1;
}

void ParcelHostFree(const void* p)
{
    if (!p)
        return;
    uint64_t* raw = (uint64_t*)p - 1;
    g_parcel_host_heap.frees++;
    g_parcel_host_heap.live -= raw[0];
    free(raw);
}

const TailSlip* TailSlipHold(const TailSlip* s)
{
    return s;
}

/* The host's records carry no Slip: a splice keeps none (file_map/tail_slip.c). */
error_t TailSlipKept(const FileRecord* old, FileRecord* rec, bool spliced)
{
    (void)old;
    (void)rec;
    (void)spliced;
    return OK;
}

/* The host stands on no Tally's DIGESTS: no reads' table, no leaf of it
 * (mount/tally_read.c) — ground let go is a key that may have gone. */
uint32_t TallyDigestLeaves(void)
{
    return 0;
}

bool TallyDigestLeafAt(uint32_t index, TallyPlace* out, bool* rotten)
{
    (void)index;
    (void)out;
    (void)rotten;
    return false;
}

void TailSlipLetGo(const TailSlip* s)
{
    (void)s;
}

void KSort(void* base, size_t count, size_t size, KSortOrder order, void* ctx)
{
    char* b = base;
    char* t = malloc(size);
    for (size_t i = 1; i < count; i++)
        for (size_t j = i; j > 0 && order(b + (j - 1) * size, b + j * size, ctx) > 0; j--) {
            memcpy(t, b + j * size, size);
            memcpy(b + j * size, b + (j - 1) * size, size);
            memcpy(b + (j - 1) * size, t, size);
        }
    free(t);
}

void BinderDemanded(void)
{
    s_demanded++;
}

static void Say(bool held, const char* what)
{
    printf("dog_ear_host: %s — %s\n", held ? "PASS" : "FAIL", what);
    s_failed += !held;
}

static TallyKey K(uint64_t k1, uint64_t k2)
{
    return (TallyKey){ k1, k2 };
}

// ----------------------------------------------------------------------------
// DogEar as the stand sees it
// ----------------------------------------------------------------------------

#define SPANS_MOST 4096u

static DogEarSpan s_spans[SPANS_MOST];

static bool KeyBefore(TallyKey a, TallyKey b)
{
    return a.k1 < b.k1 || (a.k1 == b.k1 && a.k2 < b.k2);
}

/* Whether the spans DogEar holds of `tree` — the ones a Tally took, or the
 * ones since — hold `key`. */
static bool Holds(uint64_t tree, bool taken, TallyKey key)
{
    uint32_t n = DogEarStand(tree, taken, s_spans, SPANS_MOST);
    for (uint32_t i = 0; i < n && i < SPANS_MOST; i++)
        if (!KeyBefore(key, s_spans[i].lo) && KeyBefore(key, s_spans[i].hi))
            return true;
    return false;
}

static bool SpanIs(uint32_t i, TallyKey lo, TallyKey hi)
{
    return s_spans[i].lo.k1 == lo.k1 && s_spans[i].lo.k2 == lo.k2 && s_spans[i].hi.k1 == hi.k1 &&
           s_spans[i].hi.k2 == hi.k2;
}

static void Spans(void)
{
    DogEarLetGo();
    DogEarPut(TALLY_FILES, K(5, 0), K(6, 0));
    DogEarPut(TALLY_FILES, K(7, 0), K(8, 0));
    uint32_t n = DogEarStand(TALLY_FILES, false, s_spans, SPANS_MOST);
    Say(n == 2u, "two spans apart are two");
    DogEarPut(TALLY_FILES, K(6, 0), K(7, 0));
    n = DogEarStand(TALLY_FILES, false, s_spans, SPANS_MOST);
    Say(n == 1u && SpanIs(0, K(5, 0), K(8, 0)), "the span between them joins all three into one");
    DogEarPut(TALLY_FILES, K(1, 0), K(2, 0));
    DogEarPut(TALLY_FILES, K(7, 4), K(12, 1));
    n = DogEarStand(TALLY_FILES, false, s_spans, SPANS_MOST);
    Say(n == 2u && SpanIs(0, K(1, 0), K(2, 0)) && SpanIs(1, K(5, 0), K(12, 1)),
        "one overlapping the last grows it; one before them all stands apart, first");
    DogEarPut(TALLY_FILES, K(0, 0), K(20, 0));
    n = DogEarStand(TALLY_FILES, false, s_spans, SPANS_MOST);
    Say(n == 1u && SpanIs(0, K(0, 0), K(20, 0)), "one over every span swallows them");
    DogEarPut(TALLY_TAGS, K(3, 0), K(4, 0));
    Say(DogEarStand(TALLY_TAGS, false, s_spans, SPANS_MOST) == 1u &&
        DogEarStand(TALLY_FILES, false, s_spans, SPANS_MOST) == 1u && !Holds(TALLY_USE, false, K(0, 0)) &&
        !Holds(TALLY_DIGESTS, false, K(3, 0)), "a tree's spans are its own");
    for (uint64_t k = 0; k < 2000u; k++)
        DogEarPut(TALLY_DIGESTS, K(k * 1000u, 0), K(k * 1000u + 1u, 0));
    n = DogEarStand(TALLY_DIGESTS, false, s_spans, SPANS_MOST);
    Say(n == 2000u && Holds(TALLY_DIGESTS, false, K(1999000u, 0)) && !Holds(TALLY_DIGESTS, false, K(1999001u, 0)),
        "two thousand spans apart, put one by one, are held each where it is");
}

static void Taken(void)
{
    DogEarLetGo();
    DogEarPut(TALLY_FILES, K(5, 0), K(9, 0));
    DogEarPut(TALLY_USE, K(0, 0), K(1, 0));
    DogEarTake();
    Say(DogEarStand(TALLY_FILES, false, s_spans, SPANS_MOST) == 0 && DogEarTakenHolds(TALLY_FILES, K(5, 3)) &&
        DogEarTakenHolds(TALLY_USE, K(0, 0)) && !DogEarTakenHolds(TALLY_FILES, K(9, 0)),
        "a Tally takes what is held, and nothing is held past it");
    DogEarPut(TALLY_FILES, K(20, 0), K(21, 0));
    DogEarPut(TALLY_FILES, K(8, 0), K(10, 0));
    Say(!DogEarTakenHolds(TALLY_FILES, K(20, 0)), "a key changed after the slice is not the Tally's");
    DogEarTakenBack();
    uint32_t n = DogEarStand(TALLY_FILES, false, s_spans, SPANS_MOST);
    Say(n == 2u && SpanIs(0, K(5, 0), K(10, 0)) && SpanIs(1, K(20, 0), K(21, 0)) && Holds(TALLY_USE, false, K(0, 0)) &&
        DogEarStand(TALLY_FILES, true, s_spans, SPANS_MOST) == 0,
        "a Tally refused gives what it took back, joined under what came since");
    DogEarTake();
    DogEarPut(TALLY_TAGS, K(7, 0), K(8, 0));
    DogEarTakenLetGo();
    Say(DogEarStand(TALLY_FILES, true, s_spans, SPANS_MOST) == 0 &&
        DogEarStand(TALLY_FILES, false, s_spans, SPANS_MOST) == 0 && Holds(TALLY_TAGS, false, K(7, 0)),
        "its DONE lets go of what it took; a key changed since stays");
}

static void Lost(void)
{
    DogEarLetGo();
    unsigned demanded = s_demanded;
    s_fail_at = 1;
    DogEarPut(TALLY_FILES, K(30, 0), K(31, 0));
    s_fail_at = 0;
    Say(DogEarStandLost(false) && s_demanded == demanded + 1u,
        "the heap refused DogEar room: everything is changed, and a Tally is demanded");
    DogEarPut(TALLY_FILES, K(40, 0), K(41, 0));
    Say(s_demanded == demanded + 1u, "once: a loss is not said again until a Tally ends it");
    DogEarTake();
    Say(DogEarTakenLost() && !DogEarStandLost(false), "the Tally takes the loss whole");
    DogEarTakenBack();
    Say(DogEarStandLost(false) && !DogEarTakenLost(), "refused, it gives the loss back");
    DogEarTake();
    DogEarTakenLetGo();
    Say(!DogEarStandLost(false) && !DogEarTakenLost(), "and its DONE ends it");
}

// ----------------------------------------------------------------------------
// A change of a file, held to the two records
// ----------------------------------------------------------------------------

#define FILE_FID     9u
#define FILE_STRIPS  600u           /* ~5 Parcels of 128 */
#define GROUND_MOST  (1u << 20)

static Strip* Runs(uint32_t n, uint64_t base)
{
    Strip* s = malloc(sizeof(Strip) * n);
    for (uint32_t i = 0; i < n; i++)
        s[i] = (Strip){ .start = base + i * 3u, .count = 2, .kind = i % 37u == 36u ? STRIP_HOLE : STRIP_DATA,
                        .birth = 0 };
    for (uint32_t i = 0; i < n; i++)
        if (s[i].kind == STRIP_HOLE)
            s[i].start = 0;
    return s;
}

static FileRecord* Built(uint64_t fid, uint32_t n, uint64_t base)
{
    Strip*        s    = Runs(n, base);
    uint64_t      blks = 2u * (uint64_t)n;
    TagFSMetadata meta = { .file_id = fid, .flags = 1, .size = blks * 4096u, .filename = "f", .extents = s,
                           .extent_count = n };
    FileRecord*   rec  = FileRecordBuild(&meta);
    free(s);
    return rec;
}

/* The Parcels of a record, each by the block it begins at and its Strips. */
typedef struct {
    uint64_t     pos[4096];
    const Strip* at[4096];
    uint32_t     n;
} Heads;

static void HeadsOf(const FileRecord* rec, Heads* h)
{
    StripWalk    w;
    const Strip* s;
    uint32_t     count;
    uint64_t     pos = 0;
    h->n = 0;
    if (!rec)
        return;
    StripWalkBegin(rec, &w);
    while (StripWalkParcel(&w, &s, &count)) {
        h->pos[h->n] = pos;
        h->at[h->n++] = s;
        for (uint32_t i = 0; i < count; i++)
            pos += s[i].count;
    }
}

static bool Shares(const Heads* h, uint64_t pos, const Strip* at)
{
    for (uint32_t i = 0; i < h->n; i++)
        if (h->pos[i] == pos)
            return h->at[i] == at;
    return false;
}

static void GroundOf(const FileRecord* rec, uint8_t* mark, uint8_t bit)
{
    StripWalk w;
    Strip     s;
    if (!rec)
        return;
    StripWalkBegin(rec, &w);
    while (StripWalkNextGround(&w, &s))
        for (uint64_t b = s.start; b < s.start + s.count && b < GROUND_MOST; b++)
            mark[b] |= bit;
}

/* Whether the ground DogEar's Tally took (`gone`, or born) is exactly the
 * blocks marked `want` alone (1 the old record's, 2 the new one's) and the
 * ones marked 4: let go and taken again, their data new — in both. */
static bool GroundIs(bool gone, const uint8_t* mark, uint8_t want)
{
    uint64_t            n = 0, i = 0;
    const BinderGround* r = DogEarTakenGround(gone, &n);
    for (uint64_t b = 0; b < GROUND_MOST; b++) {
        while (i < n && r[i].start + r[i].count <= b)
            i++;
        if ((i < n && r[i].start <= b) != ((mark[b] & 3u) == want || (mark[b] & 4u)))
            return false;
    }
    return n == 0 || r[n - 1u].start + r[n - 1u].count <= GROUND_MOST;
}

/* DogEar after `was` became `now` held to the two: the header, each Parcel
 * key dirty unless shared at one place, nothing else of FILES; each DIGESTS
 * window whose ground one names and the other does not, and no other. */
static void Changed(const char* what, uint64_t fid, FileRecord* was, FileRecord* now)
{
    static Heads a, b;
    static uint8_t mark[GROUND_MOST];
    char     line[256];
    bool     files = true, digests = true;
    DogEarLetGo();
    DogEarFileChanged(fid, was, now);
    HeadsOf(was, &a);
    HeadsOf(now, &b);
    files &= Holds(TALLY_FILES, false, K(fid, 0)) && !Holds(TALLY_FILES, false, K(fid - 1u, UINT64_MAX)) &&
             !Holds(TALLY_FILES, false, K(fid + 1u, 0));
    for (uint32_t i = 0; i < a.n; i++)
        files &= Holds(TALLY_FILES, false, K(fid, 1u + a.pos[i])) == !Shares(&b, a.pos[i], a.at[i]);
    for (uint32_t i = 0; i < b.n; i++)
        files &= Holds(TALLY_FILES, false, K(fid, 1u + b.pos[i])) == !Shares(&a, b.pos[i], b.at[i]);
    memset(mark, 0, sizeof(mark));
    GroundOf(was, mark, 1u);
    GroundOf(now, mark, 2u);
    for (uint64_t wnd = 0; wnd < GROUND_MOST / DIGEST_ITEM_MOST; wnd++) {
        bool diff = false;
        for (uint64_t k = wnd * DIGEST_ITEM_MOST; k < (wnd + 1u) * DIGEST_ITEM_MOST; k++)
            diff |= mark[k] == 1u || mark[k] == 2u;
        for (uint64_t k = wnd * DIGEST_ITEM_MOST; k < (wnd + 1u) * DIGEST_ITEM_MOST; k += 97u)
            digests &= Holds(TALLY_DIGESTS, false, K(k, 0)) == diff;
    }
    snprintf(line, sizeof(line), "%s: FILES holds the header and the Parcels not shared, nothing else (%u and %u "
             "Parcels)", what, a.n, b.n);
    Say(files, line);
    snprintf(line, sizeof(line), "%s: DIGESTS holds the windows of the ground one record names and the other "
             "does not, and no other", what);
    Say(digests, line);
    DogEarTake();
    snprintf(line, sizeof(line), "%s: the ground born is the blocks the new record names alone, the ground gone "
             "the old one's alone, a block exactly", what);
    Say(GroundIs(false, mark, 2u) && GroundIs(true, mark, 1u), line);
    DogEarTakenLetGo();
}

/* A Tally takes `was` → `mid`, `mid` → `now` comes after it, and it is
 * refused: given back, DogEar holds `was` → `now` — the ground born and gone
 * of the two changes as one, a block `again` let go and taken back in both. */
static void Moved(const char* what, FileRecord* was, FileRecord* mid, FileRecord* now, const Strip* again)
{
    static uint8_t mark[GROUND_MOST];
    char           line[256];
    DogEarLetGo();
    DogEarFileChanged(FILE_FID, was, mid);
    DogEarTake();
    DogEarFileChanged(FILE_FID, mid, now);
    DogEarTakenBack();
    DogEarTake();
    memset(mark, 0, sizeof(mark));
    GroundOf(was, mark, 1u);
    GroundOf(now, mark, 2u);
    for (uint64_t b = again ? again->start : 0; again && b < again->start + again->count; b++)
        mark[b] |= 4u;
    snprintf(line, sizeof(line), "%s: given back under what came since, the ground born and gone is the two "
             "changes as one", what);
    Say(GroundIs(false, mark, 2u) && GroundIs(true, mark, 1u), line);
    DogEarTakenLetGo();
}

static FileRecord* Spliced(const FileRecord* was, uint64_t at, uint64_t remove, const Strip* ins, uint64_t nins,
                           uint64_t blocks)
{
    BookSplice  sp  = { .at = at, .remove = remove, .ninsert = nins, .insert = ins };
    FileRecord* now = NULL;
    if (FileRecordSplice(was, &sp, 1, blocks * 4096u, 7, &now, NULL, NULL) != OK)
        return NULL;
    return now;
}

static void Files(void)
{
    FileRecord* was  = Built(FILE_FID, FILE_STRIPS, 1000u);
    Strip       one  = { .start = 500000u, .count = 1, .kind = STRIP_DATA, .birth = 0 };
    Strip       tail[3] = { { 600000u, 4, STRIP_DATA, 0 }, { 0, 2, STRIP_HOLE, 0 }, { 700001u, 3, STRIP_DATA, 0 } };
    uint64_t    blocks = 2u * FILE_STRIPS;
    FileRecord* over = was ? Spliced(was, 301u, 1, &one, 1, blocks) : NULL;
    FileRecord* grow = was ? Spliced(was, blocks, 0, tail, 3, blocks + 9u) : NULL;
    FileRecord* move = was ? Spliced(was, 520u, 2, &one, 1, blocks - 1u) : NULL;
    Say(was && over && grow && move, "the records are built and spliced");
    if (was && over && grow && move) {
        Changed("a block in the middle written over", FILE_FID, was, over);
        Changed("the file grown at its end", FILE_FID, was, grow);
        Changed("two blocks made one, every block past them moved", FILE_FID, was, move);
        Changed("a file born", FILE_FID, NULL, was);
        Changed("a file given up", FILE_FID, was, NULL);
        Changed("a file with nothing changed but its header", FILE_FID, was, was);
    }
    /* Block 1451 is the file's 302nd: Strip 150 of Runs(), two blocks from
     * 1000 + 3 * 150, its second. */
    Strip       back  = { .start = 1451u, .count = 1, .kind = STRIP_DATA, .birth = 0 };
    Strip       other = { .start = 600100u, .count = 1, .kind = STRIP_DATA, .birth = 0 };
    FileRecord* again = over ? Spliced(over, 301u, 1, &back, 1, blocks) : NULL;
    FileRecord* moved = over ? Spliced(over, 301u, 1, &other, 1, blocks) : NULL;
    Say(again && moved, "the records spliced again are built");
    if (again && moved) {
        Moved("a block written over and its old block taken back", was, over, again, &back);
        Moved("a block written over twice", was, over, moved, NULL);
        Moved("a file born and given up", NULL, was, NULL, NULL);
    }
    FileRecordRelease(again);
    FileRecordRelease(moved);
    FileRecordRelease(over);
    FileRecordRelease(grow);
    FileRecordRelease(move);
    FileRecordRelease(was);
}

static void Mended(void)
{
    TallyPlace at = { .level = 0, .count = 3, .lo = K(970u, 0), .hi = K(1455u, 0), .has_lo = true, .has_hi = true };
    DogEarLetGo();
    DogEarMended(TALLY_DIGESTS, &at);
    at.has_hi = false;
    at.lo     = K(4, 2);
    DogEarMended(TALLY_FILES, &at);
    Say(Holds(TALLY_DIGESTS, false, K(970u, 0)) && Holds(TALLY_DIGESTS, false, K(1454u, 0)) &&
        !Holds(TALLY_DIGESTS, false, K(1455u, 0)) && !Holds(TALLY_DIGESTS, false, K(969u, 0)),
        "a leaf a copy of which did not read puts its range in");
    Say(Holds(TALLY_FILES, false, K(4, 2)) && Holds(TALLY_FILES, false, K(UINT64_MAX - 1u, 0)) &&
        !Holds(TALLY_FILES, false, K(4, 1)), "the last leaf of a tree, its range to the tree's end");
}

/* Whether the window of DIGEST_ITEM_MOST blocks block `k` lies in meets
 * [start, end): the window of the key, apart from dog_ear.c's windows of the
 * ground. */
static bool WindowMeets(uint64_t k, uint64_t start, uint64_t end)
{
    uint64_t lo   = k - k % DIGEST_ITEM_MOST;
    uint64_t last = lo > UINT64_MAX - (DIGEST_ITEM_MOST - 1u) ? UINT64_MAX : lo + (DIGEST_ITEM_MOST - 1u);
    return lo < end && start <= last;
}

/* Ground [start, start + count) let go with no record to say so: DIGESTS
 * holds the key of every block whose window meets it, from three windows
 * before it to three past it — and no other tree anything. */
static void Ground(const char* what, uint64_t start, uint64_t count)
{
    char     line[256];
    uint64_t end  = start + count;
    uint64_t from = start > 3u * DIGEST_ITEM_MOST ? start - 3u * DIGEST_ITEM_MOST : 0;
    uint64_t to   = end < UINT64_MAX - 3u * DIGEST_ITEM_MOST ? end + 3u * DIGEST_ITEM_MOST : UINT64_MAX;
    bool     held = true;
    DogEarLetGo();
    DogEarGroundChanged(start, count);
    for (uint64_t k = from;; k++) {
        held &= Holds(TALLY_DIGESTS, false, K(k, 0)) == (count && WindowMeets(k, start, end));
        if (k == to)
            break;
    }
    held &= DogEarStand(TALLY_FILES, false, s_spans, SPANS_MOST) == 0 &&
            DogEarStand(TALLY_TAGS, false, s_spans, SPANS_MOST) == 0 && !DogEarStandLost(false);
    snprintf(line, sizeof(line), "%s: DIGESTS holds every window of the ground [%llu, +%llu) and no other", what,
             (unsigned long long)start, (unsigned long long)count);
    Say(held, line);
}

/* Ground let go with no record to say so (Escheat's): a DIGESTS element is
 * blocks in a row inside one window, so ground let go out of the middle of
 * one leaves the blocks past it an element keyed by the first of them. */
static void Grounds(void)
{
    Ground("across two windows, ended inside the second", 79000u, 237u);
    Ground("from a window's first block, ended inside it", 10670u, 337u);
    Ground("over fifteen windows, ended inside the last", 1689u, 6967u);
    Ground("ended on a window's last block", 3u * DIGEST_ITEM_MOST + 10u, 2u * DIGEST_ITEM_MOST - 10u);
    Ground("one block, a window's first", 7u * DIGEST_ITEM_MOST, 1u);
    Ground("no ground", 5000u, 0);
    Ground("at the end of the keys", UINT64_MAX - 100u, 100u);
}

/* The Parcel test of §10.4: a file of 1 GiB in runs of two blocks, a block of
 * it written over 10 000 times, each on fresh ground. Each change puts into
 * DogEar the header and the Parcels it changed — the one the block lies in,
 * or the two a cut makes of it — and no other key of FILES; the windows of
 * the block let go and the block taken, and no other; the ground born and
 * gone the two blocks exactly. So a Tally laid in after it lays the leaves
 * those keys lie in and the path above them (settle/binder_sift.c), never the
 * file's ~1000 Parcels. */
#define BIG_STRIPS    131072u
#define BIG_REWRITES  10000u

static uint64_t s_seed = 0x9E3779B97F4A7C15ull;

static uint64_t Random(void)
{
    s_seed ^= s_seed << 13;
    s_seed ^= s_seed >> 7;
    s_seed ^= s_seed << 17;
    return s_seed;
}

/* How many Parcels of `rec` begin at a key DogEar's Tally took. */
static uint32_t ParcelsTaken(const FileRecord* rec)
{
    static Heads h;
    uint32_t     n = 0;
    HeadsOf(rec, &h);
    for (uint32_t i = 0; i < h.n; i++)
        n += DogEarTakenHolds(TALLY_FILES, K(FILE_FID, 1u + h.pos[i]));
    return n;
}

static void Rewrites(void)
{
    uint64_t    blocks = 2u * (uint64_t)BIG_STRIPS;
    FileRecord* rec    = Built(FILE_FID, BIG_STRIPS, 1000u);
    uint32_t    most_parcels = 0, most_files = 0, most_windows = 0, i = 0;
    bool        ground = true;
    DogEarLetGo();
    for (; rec && i < BIG_REWRITES; i++) {
        uint64_t    at  = Random() % blocks;
        Strip       one = { .start = 2000000u + i, .count = 1, .kind = STRIP_DATA, .birth = 0 };
        FileRecord* now = Spliced(rec, at, 1, &one, 1, blocks);
        uint32_t    nf = 0, nd = 0;
        uint64_t    nb = 0, ng = 0;
        if (!now)
            break;
        DogEarFileChanged(FILE_FID, rec, now);
        DogEarTake();
        (void)DogEarTakenSpans(TALLY_FILES, &nf);
        (void)DogEarTakenSpans(TALLY_DIGESTS, &nd);
        const BinderGround* born = DogEarTakenGround(false, &nb);
        (void)DogEarTakenGround(true, &ng);
        uint32_t p = ParcelsTaken(now), q = ParcelsTaken(rec);
        most_parcels = p > most_parcels ? p : most_parcels;
        most_parcels = q > most_parcels ? q : most_parcels;
        most_files   = nf > most_files ? nf : most_files;
        most_windows = nd > most_windows ? nd : most_windows;
        ground      &= nb == 1 && born[0].start == one.start && born[0].count == 1 && ng <= 1;
        DogEarTakenLetGo();
        FileRecordRelease(rec);
        rec = now;
    }
    char line[256];
    snprintf(line, sizeof(line), "a file of 1 GiB, %u rewrites of a block: each puts in the header and at most "
             "the two Parcels it changed (%u), two spans of FILES (%u), two windows of DIGESTS (%u), the block born "
             "and the block gone", i, most_parcels, most_files, most_windows);
    Say(i == BIG_REWRITES && most_parcels <= 2u && most_files <= 2u && most_windows <= 2u && ground, line);
    FileRecordRelease(rec);
}

int main(void)
{
    Spans();
    Taken();
    Lost();
    Files();
    Mended();
    Grounds();
    Rewrites();
    DogEarLetGo();
    Say(g_parcel_host_heap.allocs == g_parcel_host_heap.frees, "DogEar let go, the heap holds nothing of it");
    printf("dog_ear_host: %s\n", s_failed ? "FAIL" : "PASS");
    return s_failed ? 1 : 0;
}
