/*
 * tip_in_host — TipIn, what a Tally lays anew among the nodes of the one before
 * it (spec_v2 §4.1, §4.3 TALLY_NODES, §16.5 п.16), held on the host with the
 * kernel's own settle/tip_in.c, settle/dog_ear.c and file_map/ (`make
 * tip-in-host`; tools/dog_ear_host.h stands in for the rest of the kernel):
 * seconds, no machine.
 *
 *   - nothing changed: the tree's root is named as it lies, nothing else said;
 *   - a key changed in a leaf: that leaf laid anew, the leaf after it under the
 *     same parent offered to the row (a row shorter than half a Sheet takes
 *     it in, Р-d1 (а)), the inner nodes above them laid, every other inner node
 *     named as it lies whole, and the leaves under those not said at all;
 *   - the last leaf of a parent changed: the neighbour offered is the one
 *     before it — never one under another parent, whose path would be laid
 *     for nothing;
 *   - an inner node a copy of which did not hold: laid again, its children
 *     named as they lie;
 *   - a leaf no copy of which reads: never laid, whatever changed in it;
 *   - §10.4: a file of 1 GiB, its Parcels packed into leaves as a writer packs
 *     them, written over at 10 000 blocks at random, a Tally after each write:
 *     every Tally lays exactly the leaves the keys DogEar holds lie in — the
 *     header's and the Parcels' the write did not share — one neighbour a row
 *     at the most, and the root above them; the nodes of the 10 000 Tallies
 *     counted against the whole Tally's.
 */

#include "../src/kernel/tagfs/settle/tip_in.h"
#include "../src/kernel/tagfs/settle/dog_ear.h"
#include "tract.h"

ParcelHostHeap  g_parcel_host_heap;
static unsigned s_failed;

void* ParcelHostAlloc(size_t bytes)
{
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
}

static void Say(bool held, const char* what)
{
    printf("tip_in_host: %s — %s\n", held ? "PASS" : "FAIL", what);
    s_failed += !held;
}

static TallyKey K(uint64_t k1, uint64_t k2)
{
    return (TallyKey){ k1, k2 };
}

// ----------------------------------------------------------------------------
// A tree of nine leaves under three parents under a root
// ----------------------------------------------------------------------------

#define NINE 9u

static TipInNode s_leaf[NINE];
static TipInNode s_inner[4];      /* three parents, then the root */

static void NineBuilt(void)
{
    memset(s_leaf, 0, sizeof(s_leaf));
    memset(s_inner, 0, sizeof(s_inner));
    for (uint32_t i = 0; i < NINE; i++) {
        TallyPlace* p = &s_leaf[i].place;
        p->ptr    = (TallyPtr){ .seq = 5, .sheets = 1, .copies = 2, .block = { 100u + i, 200u + i } };
        p->level  = 0;
        p->count  = 10;
        p->lo     = K(10u * i, 0);
        p->hi     = K(10u * (i + 1u), 0);
        p->has_lo = i > 0;
        p->has_hi = i + 1u < NINE;
    }
    for (uint32_t j = 0; j < 3u; j++) {
        TallyPlace* p = &s_inner[j].place;
        p->ptr    = (TallyPtr){ .seq = 5, .sheets = 1, .copies = 2, .block = { 300u + j, 400u + j } };
        p->level  = 1;
        p->count  = 30;
        p->lo     = K(30u * j, 0);
        p->hi     = K(30u * (j + 1u), 0);
        p->has_lo = j > 0;
        p->has_hi = j < 2u;
    }
    s_inner[3].place = (TallyPlace){ .ptr = { .seq = 5, .sheets = 1, .copies = 2, .block = { 500u, 600u } },
                                     .level = 2, .count = 90 };
}

/* The nine as a mount's walk names them, and the reads' table once turned to
 * a Tally (settle/tip_in.c, TipInLeavesClosed): every node but the root with a
 * lower key, the first leaf's and the first parent's the tree's first key. */
static void NineTurned(void)
{
    TallyPlace p[NINE];
    for (uint32_t i = 0; i < NINE; i++)
        p[i] = s_leaf[i].place;
    TipInLeavesClosed(p, NINE);
    for (uint32_t i = 0; i < NINE; i++)
        s_leaf[i].place = p[i];
    s_inner[0].place.has_lo = true;
    s_inner[0].place.lo     = K(0, 0);
}

/* The fates TipIn gives the first `nleaf` of the nine with `spans`, against
 * `want` — a letter a leaf, then a letter a parent and the root's: U under,
 * K kept, L laid, M a margin. */
static void NineSaysOf(const char* what, uint32_t nleaf, const DogEarSpan* spans, uint32_t n, const char* want)
{
    TipInFate leaf[NINE], inner[4];
    char      got[NINE + 5u], line[240];
    static const char letter[] = { [TIP_IN_UNDER] = 'U', [TIP_IN_KEPT] = 'K', [TIP_IN_LAID] = 'L',
                                   [TIP_IN_MARGIN] = 'M' };
    error_t   rc = TipInSift(TALLY_FILES, s_leaf, nleaf, s_inner, 4u, spans, n, leaf, inner);
    for (uint32_t i = 0; i < nleaf; i++)
        got[i] = letter[leaf[i]];
    for (uint32_t j = 0; j < 4u; j++)
        got[nleaf + j] = letter[inner[j]];
    got[nleaf + 4u] = 0;
    snprintf(line, sizeof(line), "%s: %s (want %s)", what, rc == OK ? got : "refused", want);
    Say(rc == OK && strcmp(got, want) == 0, line);
}

static void NineSays(const char* what, const DogEarSpan* spans, uint32_t n, const char* want)
{
    NineSaysOf(what, NINE, spans, n, want);
}

static void Nine(void)
{
    DogEarSpan one = { K(41u, 0), K(42u, 0) };
    DogEarSpan last = { K(55u, 0), K(56u, 0) };
    DogEarSpan two[2] = { { K(12u, 0), K(13u, 0) }, { K(75u, 0), K(76u, 0) } };
    DogEarSpan wide = { K(25u, 0), K(65u, 0) };
    DogEarSpan past = { K(95u, 0), K(96u, 0) };
    DogEarSpan seven = { K(75u, 0), K(76u, 0) };
    NineBuilt();
    NineSays("nothing changed: the root named as it lies, nothing under it said", NULL, 0, "UUUUUUUUU" "UUUK");
    NineSays("a key of the middle leaf of a parent", &one, 1, "UUUKLMUUU" "KLKL");
    NineSays("a key of a parent's last leaf: the neighbour offered is the one before it", &last, 1,
             "UUUKMLUUU" "KLKL");
    NineSays("two keys under two parents", two, 2, "KLMUUUKLM" "LKLL");
    NineSays("a span across three leaves and two parents", &wide, 1, "KKLLLLLMK" "LLLL");
    s_inner[2].relay = true;
    NineSays("a parent a copy of which did not hold: laid again, its leaves named as they lie", NULL, 0,
             "UUUUUUKKK" "KKLL");
    s_inner[2].relay = false;
    s_leaf[4].rotten = true;
    NineSays("a leaf no copy of which reads that nothing changed in stays under its parent", NULL, 0,
             "UUUUUUUUU" "UUUK");
    NineSays("a leaf no copy of which reads is never laid: a change in it lays its parent anew, which names it",
             &one, 1, "UUUKKKUUU" "KLKL");
    s_leaf[4].rotten = false;
    s_leaf[8].rotten = true;
    NineSays("the last leaf rotten, a key born past it: its parent laid anew, the key laid past it", &past, 1,
             "UUUUUUKKK" "KKLL");
    s_leaf[8].rotten = false;
    s_inner[2].rotten = true;
    NineSaysOf("the last parent rotten, its leaves never read, a key born past it: the root laid anew names it",
               6u, &past, 1, "UUUUUU" "KKKL");
    s_inner[2].rotten = false;
    NineTurned();
    NineSays("the first leaf and parent with the lower keys a mount's walk and a turn of the reads give them: the "
             "first leaf under its parent named as it lies", &seven, 1, "UUUUUUKLM" "KKLL");
    NineBuilt();
    s_leaf[7].relay = true;
    NineSays("a leaf a copy of which did not hold: laid again, like a changed one", NULL, 0, "UUUUUUKLM" "KKLL");
    s_leaf[7].relay = false;
}

// ----------------------------------------------------------------------------
// §10.4: a file of 1 GiB written over at 10 000 blocks, a Tally after each
// ----------------------------------------------------------------------------

#define BIG_FID      9u
#define BIG_BLOCKS   262144u        /* 1 GiB */
#define BIG_WRITES   10000u
#define BIG_FRESH    (1ull << 32)   /* where fresh ground is taken from: past every run of the file */
#define SHEET_ROOM   3904u          /* a Sheet's payload (§2.6) */
#define LEAVES_MOST  4096u
#define SPANS_MOST   4096u

static uint64_t s_seed = 0x9E3779B97F4A7C15ull;

static uint64_t Next(void)
{
    s_seed ^= s_seed << 13;
    s_seed ^= s_seed >> 7;
    s_seed ^= s_seed << 17;
    return s_seed;
}

/* The file: runs of 1..64 blocks, every fiftieth a hole, built the way a mount
 * builds a record. */
static FileRecord* BigBuilt(void)
{
    Strip*   s = malloc(sizeof(Strip) * BIG_BLOCKS);
    uint32_t n = 0;
    uint64_t at = 0, ground = 1000u;
    while (at < BIG_BLOCKS) {
        uint64_t len = 1u + Next() % 64u;
        len = at + len > BIG_BLOCKS ? BIG_BLOCKS - at : len;
        bool hole = n % 50u == 49u;
        s[n++] = (Strip){ .start = hole ? 0 : ground, .count = (uint32_t)len, .kind = hole ? STRIP_HOLE : STRIP_DATA,
                          .birth = 0 };
        ground += hole ? 0 : len + 1u;
        at += len;
    }
    TagFSMetadata meta = { .file_id = BIG_FID, .flags = 1, .size = (uint64_t)BIG_BLOCKS * 4096u, .filename = "big",
                           .extents = s, .extent_count = n };
    FileRecord*   rec  = FileRecordBuild(&meta);
    free(s);
    return rec;
}

/* The leaves of FILES a writer packs of the file — its header, then each
 * Parcel, an element no longer than its runs at their widest — each from the
 * key of its first element; and a root over them. */
static uint32_t BigLeaves(const FileRecord* rec, TipInNode* leaf, TipInNode* root)
{
    StripWalk    w;
    const Strip* s;
    uint32_t     count = 0, n = 0, used = 80u;
    uint64_t     pos = 0;
    memset(leaf, 0, sizeof(TipInNode));
    leaf[0].place = (TallyPlace){ .ptr = { .seq = 3, .sheets = 1, .copies = 2, .block = { 10, 20 } }, .count = 1 };
    StripWalkBegin(rec, &w);
    while (StripWalkParcel(&w, &s, &count)) {
        uint32_t bytes = 10u + 9u * count;
        if (used + bytes > SHEET_ROOM && n + 1u < LEAVES_MOST) {
            leaf[n].place.hi     = K(BIG_FID, 1u + pos);
            leaf[n].place.has_hi = true;
            n++;
            leaf[n] = (TipInNode){ .place = { .ptr = { .seq = 3, .sheets = 1, .copies = 2,
                                                       .block = { 10u + 2u * n, 20u + 2u * n } },
                                              .lo = K(BIG_FID, 1u + pos), .has_lo = true } };
            used = 0;
        }
        used += bytes;
        for (uint32_t i = 0; i < count; i++)
            pos += s[i].count;
    }
    *root = (TipInNode){ .place = { .ptr = { .seq = 3, .sheets = 1, .copies = 2, .block = { 1, 2 } }, .level = 1,
                                    .count = 1 } };
    return n + 1u;
}

/* Whether DogEar's spans of FILES `sp` (`n`) meet the leaf at `p`. */
static bool SpansMeet(const DogEarSpan* sp, uint32_t n, const TallyPlace* p)
{
    for (uint32_t i = 0; i < n; i++) {
        bool below = !p->has_hi || sp[i].lo.k1 < p->hi.k1 || (sp[i].lo.k1 == p->hi.k1 && sp[i].lo.k2 < p->hi.k2);
        bool above = !p->has_lo || p->lo.k1 < sp[i].hi.k1 || (p->lo.k1 == sp[i].hi.k1 && p->lo.k2 < sp[i].hi.k2);
        if (below && above)
            return true;
    }
    return false;
}

static void Big(void)
{
    static TipInNode  leaf[LEAVES_MOST];
    static TipInFate  fate[LEAVES_MOST];
    static DogEarSpan spans[SPANS_MOST];
    TipInNode         root;
    TipInFate         root_fate = TIP_IN_UNDER;
    FileRecord*       was = BigBuilt();
    uint64_t          laid_nodes = 0, whole_nodes = 0, fresh = BIG_FRESH, margins_over = 0;
    bool              exact = true, refused = false;
    char              line[240];
    uint32_t          nleaf = was ? BigLeaves(was, leaf, &root) : 0;
    Say(was && nleaf > 8u, "the file of 1 GiB is built, its Parcels packed into leaves past eight");
    for (uint32_t k = 0; was && k < BIG_WRITES && !refused; k++) {
        Strip       one = { .start = fresh++, .count = 1, .kind = STRIP_DATA, .birth = 0 };
        BookSplice  sp  = { .at = Next() % BIG_BLOCKS, .remove = 1, .ninsert = 1, .insert = &one };
        FileRecord* now = NULL;
        if (FileRecordSplice(was, &sp, 1, (uint64_t)BIG_BLOCKS * 4096u, 7, &now, NULL, NULL) != OK) {
            refused = true;
            break;
        }
        DogEarLetGo();
        DogEarFileChanged(BIG_FID, was, now);
        uint32_t n = DogEarStand(TALLY_FILES, false, spans, SPANS_MOST);
        if (n > SPANS_MOST || TipInSift(TALLY_FILES, leaf, nleaf, &root, 1u, spans, n, fate, &root_fate) != OK) {
            refused = true;
            FileRecordRelease(now);
            break;
        }
        uint32_t laid = 0, margins = 0;
        for (uint32_t i = 0; i < nleaf; i++) {
            bool meets = SpansMeet(spans, n, &leaf[i].place);
            laid    += fate[i] == TIP_IN_LAID;
            margins += fate[i] == TIP_IN_MARGIN;
            exact   &= (fate[i] == TIP_IN_LAID) == meets;
        }
        exact &= root_fate == TIP_IN_LAID;
        margins_over += margins > n;
        laid_nodes   += laid + margins + 1u;
        whole_nodes  += nleaf + 1u;
        FileRecordRelease(was);
        was = now;
    }
    Say(!refused, "10 000 blocks written over at random, a Tally after each write");
    Say(exact, "each Tally lays exactly the leaves the keys DogEar holds lie in, and the root above them");
    Say(margins_over == 0, "and offers a row no more neighbours than DogEar has spans");
    snprintf(line, sizeof(line), "the 10 000 Tallies lay %llu node(s) at the most, the whole Tallies %llu: %llu a "
             "Tally against %u", (unsigned long long)laid_nodes, (unsigned long long)whole_nodes,
             (unsigned long long)(laid_nodes / BIG_WRITES), nleaf + 1u);
    Say(laid_nodes * 2u <= whole_nodes, line);
    FileRecordRelease(was);
    DogEarLetGo();
}

int main(void)
{
    Nine();
    Big();
    Say(g_parcel_host_heap.allocs == g_parcel_host_heap.frees, "the heap holds nothing TipIn or the records took");
    printf("tip_in_host: %s\n", s_failed ? "FAIL" : "PASS");
    return s_failed ? 1 : 0;
}
