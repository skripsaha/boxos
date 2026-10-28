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
 *     Parcel by Parcel and block by block here, apart from the kernel's walk.
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

int main(void)
{
    Spans();
    Taken();
    Lost();
    Files();
    Mended();
    DogEarLetGo();
    Say(g_parcel_host_heap.allocs == g_parcel_host_heap.frees, "DogEar let go, the heap holds nothing of it");
    printf("dog_ear_host: %s\n", s_failed ? "FAIL" : "PASS");
    return s_failed ? 1 : 0;
}
