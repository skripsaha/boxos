#include "tests.h"
#include "../tagfs.h"
#include "../file_map/file_map.h"
#include "../file_map/tract.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// TRACT — a record's runs in a Tract more than one node tall
//
// A record of a few hundred Strips is one leaf of its Tract (tract.c), and so
// is every record of the parcel suite. Here
// a file grows the way a writer grows it — a few Strips appended at its end at
// a time — until its Tract stands three levels tall, and is then cut: the
// leaf a parent begins with emptied into the last leaf of the parent before
// it, a parent emptied into its neighbour, more than a leaf's worth of
// Parcels put into one leaf, splices of every size at random, the file cut
// back to five Strips and to nothing. After every change the Tract is walked
// whole and held to its rules (tract.h) — every leaf at one depth, every node
// but the root naming at least TRACT_KIDS_LEAST kids and the root two above a
// leaf, each node's counts its kids', every Parcel within its bounds, every
// node and Parcel held once when its record alone names it — its Strips to a
// flat list the test keeps by hand, its blocks found where that list lays
// them, and the ground each splice frees to the ground the hand took out.
// Everything is in memory; nothing touches a medium or the volume's state.
// ============================================================================

#define TRACT_TEST_BATCH      64u        /* Strips a growing writer appends at a time */
#define TRACT_TEST_TALL       2u         /* the level the grown Tract's root stands at */
#define TRACT_TEST_LAST       36u        /* ... and the leaves its last kid names by then */
#define TRACT_TEST_APPENDS    4096u      /* appends the growth may take at most (it takes ~2600) */
#define TRACT_TEST_EVERY      256u       /* appends between two walks of the growing Tract */
#define TRACT_TEST_SPLICES    40u        /* splices at random on the tall Tract */
#define TRACT_TEST_INSERT     9000u      /* Strips put into one leaf: past TRACT_KIDS_MOST Parcels */
#define TRACT_TEST_GROUND     1000u      /* where the grown file's ground begins */
#define TRACT_TEST_FRESH      4000000u   /* ground no grown Strip names */
#define TRACT_TEST_SAMPLES    997u       /* Strips between two blocks looked up */

/* The flat list kept by hand: the Strips, the room for them, the blocks they
 * cover, and the DATA ground the last splice took out. */
typedef struct {
    Strip*   strip;
    uint64_t count;
    uint64_t room;
    uint64_t blocks;
    Strip*   took;
    uint64_t ntook;
} TractRef;

/* What a test holds, let go on every way out. */
typedef struct {
    TractRef    ref;
    FileRecord* rec;
    Strip*      put;
    Strip*      freed;
    uint64_t    nfreed;
    uint64_t    seed;
    uint64_t    fresh;       /* fresh Strips laid so far */
} TractHeld;

/* The k-th Strip a growing writer lays: 1 to 3 blocks, every 13th a hole,
 * every other on ground of its own, the births all different. */
static Strip TractTestStrip(uint64_t k)
{
    bool hole = k % 13u == 12u;
    return (Strip){ .start = hole ? 0 : TRACT_TEST_GROUND + 4u * k, .count = 1u + (uint32_t)(k % 3u),
                    .kind = hole ? STRIP_HOLE : STRIP_DATA, .birth = k % 5u };
}

/* A fresh Strip a splice puts in, on ground no other Strip names. */
static Strip TractFreshStrip(TractHeld* h)
{
    uint64_t k    = h->fresh++;
    bool     hole = k % 7u == 6u;
    return (Strip){ .start = hole ? 0 : TRACT_TEST_FRESH + 4u * k, .count = 1u + (uint32_t)(k % 3u),
                    .kind = hole ? STRIP_HOLE : STRIP_DATA, .birth = 7u };
}

static uint64_t TractRandom(TractHeld* h)
{
    h->seed = h->seed * 6364136223846793005ull + 1442695040888963407ull;
    return h->seed >> 17;
}

/* `len` blocks of Strip `s` from its `off`-th: a DATA piece begins where its
 * blocks do; a hole keeps its start. */
static Strip TractPiece(const Strip* s, uint64_t off, uint64_t len)
{
    Strip p = *s;
    if (s->kind == STRIP_DATA)
        p.start = s->start + off;
    p.count = (uint32_t)len;
    return p;
}

static uint64_t TractBlocksOf(const Strip* s, uint64_t n)
{
    uint64_t blocks = 0;
    for (uint64_t i = 0; i < n; i++)
        blocks += s[i].count;
    return blocks;
}

// ----------------------------------------------------------------------------
// The list kept by hand
// ----------------------------------------------------------------------------

/* The DATA ground of reference Strip `s`, which begins at block `pos`, lying
 * in [lo, hi): added to what the splice took. */
static void TractRefTook(TractRef* r, const Strip* s, uint64_t pos, uint64_t lo, uint64_t hi)
{
    uint64_t x = pos > lo ? pos : lo, y = pos + s->count < hi ? pos + s->count : hi;
    if (x < y && s->kind == STRIP_DATA)
        r->took[r->ntook++] = TractPiece(s, x - pos, y - x);
}

/* `sp` applied by hand: the Strips before block `at` (the one it falls in cut
 * there), the inserted ones, the Strips from at + remove on (cut likewise);
 * the DATA ground between them into r->took. An append with room is laid in
 * place, and takes nothing out. False with no memory. */
static bool TractRefSplice(TractRef* r, const BookSplice* sp)
{
    uint64_t cut = sp->at + sp->remove, pos = 0, n = 0;
    uint64_t room = r->count + sp->ninsert + 2u;
    Strip*   out  = NULL;

    r->ntook = 0;
    if (sp->at == r->blocks && sp->remove == 0 && r->count + sp->ninsert <= r->room) {
        memcpy(&r->strip[r->count], sp->insert, sizeof(Strip) * sp->ninsert);
        r->count  += sp->ninsert;
        r->blocks += TractBlocksOf(sp->insert, sp->ninsert);
        return true;
    }
    if (sp->at == r->blocks && sp->remove == 0)
        room = 2u * (r->count + sp->ninsert);
    kfree(r->took);
    r->took = kmalloc(sizeof(Strip) * (r->count + 1u));
    if (!r->took || !(out = kmalloc(sizeof(Strip) * room)))
        return false;
    for (uint64_t i = 0; i < r->count; pos += r->strip[i].count, i++) {
        uint64_t end = pos + r->strip[i].count;
        if (pos < sp->at)
            out[n++] = TractPiece(&r->strip[i], 0, (end < sp->at ? end : sp->at) - pos);
        TractRefTook(r, &r->strip[i], pos, sp->at, cut);
    }
    memcpy(&out[n], sp->insert, sizeof(Strip) * sp->ninsert);
    n += sp->ninsert;
    pos = 0;
    for (uint64_t i = 0; i < r->count; pos += r->strip[i].count, i++) {
        uint64_t end = pos + r->strip[i].count, off = cut > pos ? cut - pos : 0;
        if (end > cut)
            out[n++] = TractPiece(&r->strip[i], off, r->strip[i].count - off);
    }
    kfree(r->strip);
    r->strip  = out;
    r->count  = n;
    r->room   = room;
    r->blocks = r->blocks - sp->remove + TractBlocksOf(sp->insert, sp->ninsert);
    return true;
}

// ----------------------------------------------------------------------------
// The Tract held to its rules
// ----------------------------------------------------------------------------

typedef struct {
    uint32_t parcels;
    uint32_t nodes;
    uint32_t least;      /* the fewest Strips of a Parcel */
} TractCount;

static const char* TractParcelBroken(const Parcel* p, bool sole, TractCount* c)
{
    if (p->count == 0 || p->count > PARCEL_STRIPS_MOST)
        return "a Parcel of no Strips, or past the most";
    if (ParcelEnds(p)[p->count - 1u] != p->blocks || TractBlocksOf(p->strip, p->count) != p->blocks)
        return "a Parcel's blocks are not its Strips'";
    if (sole && p->refs != 1)
        return "a Parcel its record alone names is held more or less than once";
    c->parcels++;
    if (p->count < c->least)
        c->least = p->count;
    return NULL;
}

/* One node: its kids within the bounds, its counts its kids'. */
static const char* TractNodeBroken(const Tract* t, bool root, bool sole)
{
    uint64_t end = 0, strips = 0, bytes = sizeof(Tract) + sizeof(TractKid) * t->count;
    if (t->count == 0 || t->count > TRACT_KIDS_MOST || (!root && t->count < TRACT_KIDS_LEAST) ||
        (root && t->level > 0 && t->count < 2u))
        return "a node names fewer kids than its place allows, or more than the most";
    if (sole && t->refs != 1)
        return "a node its record alone names is held more or less than once";
    for (uint32_t i = 0; i < t->count; i++) {
        const void* kid = t->kid[i].node;
        if (t->level == 0) {
            end    += ((const Parcel*)kid)->blocks;
            strips += ((const Parcel*)kid)->count;
            bytes  += ParcelBytes(((const Parcel*)kid)->count);
        } else {
            if (((const Tract*)kid)->level + 1u != t->level)
                return "a kid is not one level below its node: the leaves are not at one depth";
            end    += TractBlocks(kid);
            strips += ((const Tract*)kid)->strips;
            bytes  += ((const Tract*)kid)->bytes;
        }
        if (t->kid[i].end != end)
            return "a kid's end is not the blocks under it and every kid before it";
    }
    return strips == t->strips && bytes == t->bytes ? NULL
                                                    : "a node's Strips or bytes are not its kids'";
}

/* The record's Tract walked whole with no recursion — every node held to its
 * rules on the way down, every Parcel at the bottom — and the record's counts
 * the Tract's. NULL when every rule holds; else the first broken, said. */
static const char* TractBroken(const FileRecord* rec, bool sole, TractCount* c)
{
    struct {
        const Tract* node;
        uint32_t     next;
    } path[TRACT_HEIGHT_MOST];
    uint32_t     depth = 0;
    const Tract* root  = rec->tract;
    const char*  why   = NULL;

    memset(c, 0, sizeof(*c));
    c->least = PARCEL_STRIPS_MOST;
    if (!root)
        return rec->strip_count == 0 && rec->blocks == 0 ? NULL : "a record of no Tract counts Strips";
    if (root->level >= TRACT_HEIGHT_MOST || rec->strip_count != root->strips ||
        rec->blocks != TractBlocks(root) || (sole && rec->refs != 1))
        return "the record's counts are not its Tract's, or the Tract is past the most levels";
    if ((why = TractNodeBroken(root, true, sole)) != NULL)
        return why;
    path[depth].node = root;
    path[depth].next = 0;
    depth++;
    while (depth && !why) {
        const Tract* n = path[depth - 1u].node;
        if (path[depth - 1u].next == n->count) {
            depth--;
            continue;
        }
        const void* kid = n->kid[path[depth - 1u].next++].node;
        c->nodes += path[depth - 1u].next == 1u;
        if (n->level == 0) {
            why = TractParcelBroken(kid, sole, c);
        } else if ((why = TractNodeBroken(kid, false, sole)) == NULL) {
            path[depth].node = kid;
            path[depth].next = 0;
            depth++;
        }
    }
    if (!why && c->parcels > 1u && c->least < PARCEL_STRIPS_LEAST)
        why = "a Parcel beside another holds fewer Strips than the least";
    return why;
}

/* The record holds the hand's Strips, Strip for Strip, and finds every
 * TRACT_TEST_SAMPLES-th Strip's first block — and the last block — where the
 * hand lays it. Walked, never laid flat: the list is millions of bytes. */
static bool TractMatches(const FileRecord* rec, const TractRef* r)
{
    StripWalk w;
    Strip     s;
    FileSpot  spot;
    uint64_t  i = 0, pos = 0;
    StripWalkBegin(rec, &w);
    for (; StripWalkNext(&w, &s); pos += s.count, i++) {
        if (i >= r->count || memcmp(&s, &r->strip[i], sizeof(Strip)) != 0)
            return false;
        if (i % TRACT_TEST_SAMPLES == 0 &&
            (!FileRecordFind(rec, pos, &spot) || spot.kind != s.kind || spot.left != s.count - 1u ||
             spot.block != (s.kind == STRIP_DATA ? s.start : 0)))
            return false;
    }
    if (i != r->count || pos != r->blocks)
        return false;
    return pos == 0 || (FileRecordFind(rec, pos - 1u, &spot) && spot.left == 0 &&
                        !FileRecordFind(rec, pos, &spot));
}

/* After a splice: the Tract keeps its rules, the record the hand's Strips,
 * and the ground freed is what the hand took out. Said when not. */
static bool TractStillHolds(TractHeld* h, const char* after)
{
    TractCount  c;
    const char* why = TractBroken(h->rec, true, &c);
    if (why) {
        kprintf("[TESTS] tract: after %s, %s\n", after, why);
        return false;
    }
    if (!TractMatches(h->rec, &h->ref)) {
        kprintf("[TESTS] tract: after %s, the record's Strips are not the ones laid by hand\n", after);
        return false;
    }
    uint64_t a = StripsAsGround(h->freed, h->nfreed), b = StripsAsGround(h->ref.took, h->ref.ntook);
    if (a != b || (a && memcmp(h->freed, h->ref.took, sizeof(Strip) * a) != 0)) {
        kprintf("[TESTS] tract: after %s, the ground freed is not the ground taken out\n", after);
        return false;
    }
    return true;
}

// ----------------------------------------------------------------------------
// Changes
// ----------------------------------------------------------------------------

/* One splice applied to the record — its ground freed asked for — and by hand
 * to the list; the record before it let go, so every node and Parcel of the
 * new one is held by it alone. False when either refused. */
static bool TractSpliced(TagFSVolume* vol, TractHeld* h, uint64_t at, uint64_t remove, uint64_t ninsert)
{
    BookSplice  sp   = { .at = at, .remove = remove, .ninsert = ninsert, .insert = h->put };
    FileRecord* next = NULL;
    kfree(h->freed);
    h->freed = NULL;
    if (FileRecordSplice(vol, h->rec, &sp, 1, 0, 0, &next, &h->freed, &h->nfreed) != OK)
        return false;
    FileRecordRelease(h->rec);
    h->rec = next;
    return TractRefSplice(&h->ref, &sp);
}

static bool TractTake(TractHeld* h)
{
    TagFSMetadata meta;
    memset(h, 0, sizeof(*h));
    memset(&meta, 0, sizeof(meta));
    meta.file_id  = 11;
    meta.filename = "tract";
    h->seed       = 0x7AC7u;
    h->rec        = FileRecordBuild(&meta);
    h->put        = kmalloc(sizeof(Strip) * TRACT_TEST_INSERT);
    return h->rec && h->put;
}

static void TractLetGoAll(TractHeld* h)
{
    FileRecordRelease(h->rec);
    kfree(h->put);
    kfree(h->freed);
    kfree(h->ref.strip);
    kfree(h->ref.took);
    memset(h, 0, sizeof(*h));
}

/* Whether the Tract stands TRACT_TEST_TALL levels up and its root's last kid
 * names TRACT_TEST_LAST leaves: two parents of more leaves together than one
 * node names, so a leaf joined across them leaves the root two kids. */
static bool TractGrown(const Tract* root)
{
    return root->level >= TRACT_TEST_TALL &&
           ((const Tract*)root->kid[root->count - 1u].node)->count >= TRACT_TEST_LAST;
}

/* The file grown by TRACT_TEST_BATCH Strips at a time at its end until its
 * Tract is grown (TractGrown), walked whole every TRACT_TEST_EVERY appends and
 * at the end. */
static TestResult TractGrowTall(TagFSVolume* vol, TractHeld* h)
{
    TestResult verdict = TEST_PASS;
    uint64_t   k       = 0;
    uint32_t   n       = 0;
    TractCount c;

    for (n = 0; n < TRACT_TEST_APPENDS; n++) {
        for (uint32_t i = 0; i < TRACT_TEST_BATCH; i++)
            h->put[i] = TractTestStrip(k++);
        TEST_ASSERT(TractSpliced(vol, h, h->rec->blocks, 0, TRACT_TEST_BATCH), "an append is taken");
        if (n % TRACT_TEST_EVERY == 0)
            TEST_ASSERT(TractStillHolds(h, "an append"), "the growing Tract keeps its rules");
        if (TractGrown(h->rec->tract))
            break;
    }
    TEST_ASSERT(TractStillHolds(h, "the last append"), "the grown Tract keeps its rules");
    TEST_ASSERT_EQ(h->rec->tract->level, TRACT_TEST_TALL, "and stands three levels tall");
    TEST_ASSERT(TractBroken(h->rec, true, &c) == NULL, "counted");
    TestEvidence("Parcels of the grown file", c.parcels);
out:
    return verdict;
}

/* The leaf the root's last kid begins with, emptied but for its last
 * Parcel: the Parcel left is joined with the last leaf of the kid before — a
 * neighbour under another parent — and the path above is that neighbour's;
 * the two parents name more leaves than one node does, so the root keeps two
 * kids. Then the root's last kid emptied but for its first leaf: one leaf is
 * fewer than a parent beside another names, so it is joined into the kid
 * before, and the root, left one kid, gives way to it. */
static TestResult TractCutAcrossParents(TagFSVolume* vol, TractHeld* h)
{
    TestResult   verdict = TEST_PASS;
    const Tract* root    = h->rec->tract;
    const Tract* last    = root->kid[root->count - 1u].node;
    const Tract* leaf    = last->kid[0].node;
    uint64_t     start   = root->kid[root->count - 2u].end;

    TEST_ASSERT(leaf->count >= 2u, "the leaf names more than one Parcel");
    TEST_ASSERT(TractSpliced(vol, h, start, leaf->kid[leaf->count - 2u].end, 0),
                "every Parcel of the leaf but its last is taken out");
    TEST_ASSERT(TractStillHolds(h, "a leaf joined across parents"), "the Tract keeps its rules");
    root = h->rec->tract;
    TEST_ASSERT(root->level == TRACT_TEST_TALL && root->count == 2u,
                "the root still stands two levels up, naming two kids");

    last  = root->kid[1].node;
    start = root->kid[0].end + last->kid[0].end;
    TEST_ASSERT(TractSpliced(vol, h, start, TractBlocks(last) - last->kid[0].end, 0),
                "every leaf of the root's last kid but its first is taken out");
    TEST_ASSERT(TractStillHolds(h, "a parent joined into the one before"), "the Tract keeps its rules");
    TEST_ASSERT_EQ(h->rec->tract->level, TRACT_TEST_TALL - 1u, "and the root gave way to its one kid");
out:
    return verdict;
}

/* TRACT_TEST_INSERT fresh Strips put inside one leaf — more Parcels than a
 * node names — then splices at random: small and large, taking out and
 * putting in, each walked whole after it. */
static TestResult TractCutAtRandom(TagFSVolume* vol, TractHeld* h)
{
    TestResult verdict = TEST_PASS;
    for (uint32_t i = 0; i < TRACT_TEST_INSERT; i++)
        h->put[i] = TractFreshStrip(h);
    TEST_ASSERT(TractSpliced(vol, h, h->rec->blocks / 3u, 0, TRACT_TEST_INSERT),
                "more than a leaf's worth of Parcels put into one leaf");
    TEST_ASSERT(TractStillHolds(h, "the leaf divided"), "the Tract keeps its rules");

    for (uint32_t s = 0; s < TRACT_TEST_SPLICES; s++) {
        uint64_t roll   = TractRandom(h);
        uint64_t at     = TractRandom(h) % (h->rec->blocks + 1u);
        uint64_t most   = roll % 8u == 0 ? 40000u : 3000u;
        uint64_t remove = TractRandom(h) % most;
        uint64_t put    = roll % 16u == 1 ? TRACT_TEST_INSERT : TractRandom(h) % 400u;
        if (remove > h->rec->blocks - at)
            remove = h->rec->blocks - at;
        for (uint64_t i = 0; i < put; i++)
            h->put[i] = TractFreshStrip(h);
        TEST_ASSERT(TractSpliced(vol, h, at, remove, put), "a splice at random is taken");
        TEST_ASSERT(TractStillHolds(h, "a splice at random"), "the Tract keeps its rules");
    }
out:
    return verdict;
}

/* The file cut back to its first five Strips — the root gives way level by
 * level down to the one leaf — and then to nothing: no Tract at all. */
static TestResult TractCutAway(TagFSVolume* vol, TractHeld* h)
{
    TestResult verdict = TEST_PASS;
    uint64_t   keep    = TractBlocksOf(h->ref.strip, 5u);
    TEST_ASSERT(TractSpliced(vol, h, keep, h->rec->blocks - keep, 0), "the file is cut to five Strips");
    TEST_ASSERT(TractStillHolds(h, "the cut to five Strips"), "the Tract keeps its rules");
    TEST_ASSERT(h->rec->tract->level == 0 && h->rec->tract->count == 1u && h->rec->strip_count == 5u,
                "one leaf naming one Parcel of five Strips is all there is");
    TEST_ASSERT(TractSpliced(vol, h, 0, h->rec->blocks, 0), "and cut to nothing");
    TEST_ASSERT(TractStillHolds(h, "the cut to nothing"), "a record of no Strips");
    TEST_ASSERT(h->rec->tract == NULL, "names no Tract");
out:
    return verdict;
}

static TestResult TestTractTall(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult verdict = TEST_PASS;
    TractHeld  h;

    TEST_ASSERT(TractTake(&h), "a record of no Strips, and room for what goes into it");
    if ((verdict = TractGrowTall(vol, &h)) == TEST_PASS &&
        (verdict = TractCutAcrossParents(vol, &h)) == TEST_PASS &&
        (verdict = TractCutAtRandom(vol, &h)) == TEST_PASS)
        verdict = TractCutAway(vol, &h);
out:
    TractLetGoAll(&h);
    return verdict;
}

static const TestCase g_tract_tests[] = {
    { .name = "tract_tall", .func = TestTractTall, .reach = TEST_READS_ONLY },
};

const TestSuite g_tract_suite = {
    .tests = g_tract_tests, .count = ARRAY_SIZE(g_tract_tests),
};

#endif /* CONFIG_VOLUME_TESTS */
