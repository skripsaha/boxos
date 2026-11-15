#include "tests.h"
#include "../tagfs.h"
#include "../file_map/file_map.h"
#include "../file_map/tract.h"
#include "../plot/plot.h"
#include "../read/read.h"
#include "../ink/ink.h"
#include "../ink/fallow.h"
#include "../mount/ground_census.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// PARCEL — a file's runs as memory holds them: Parcels under a Tract
//
// A record's runs are cut into Parcels of at most 128 Strips, and an immutable
// Tract names them (file_record.h). A change copies the header, the Parcels it
// touches and the nodes on the way down to them — never the file — and a read
// finds a block down the Tract, not by walking every run from the first. Every
// record here is built in memory from Strips the test makes and checked
// against a flat reference the test applies by hand; nothing touches a medium
// or the volume's state.
// ============================================================================

#define PARCEL_FILE_STRIPS   1000u      /* format v1 holds ~670 at most; past it on purpose */
#define PARCEL_FIRST_BLOCK   5000u      /* where the test's ground begins */
#define PARCEL_FRESH_BLOCK   900000u    /* ground no Strip of the test's file names */
#define PARCEL_ROOM          (PARCEL_FILE_STRIPS + 512u)
#define PARCEL_HEAP_ROOM     64u        /* the nodes or Parcels one record of the test's has */
#define PARCEL_READ_SINK     (64u * 1024u)

static char s_parcel_name[]  = "parcel";
static char s_parcel_other[] = "renamed";

/* `count` Strips of a file, every `hole_every`-th a hole (0: none, and every
 * birth 0 — a file as format v1 has them): 1 to 4 blocks each by a fixed rule,
 * each DATA Strip beginning 1 to 3 blocks past where the one before it ended,
 * so no two of them meet on the medium. */
static void ParcelStrips(Strip* out, uint32_t count, uint32_t hole_every, uint64_t first)
{
    uint64_t at = first;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t n    = 1u + (i * 7u) % 4u;
        bool     hole = hole_every && i % hole_every == hole_every - 1u;
        out[i] = (Strip){ .start = hole ? 0 : at, .count = n,
                          .kind = hole ? STRIP_HOLE : STRIP_DATA,
                          .birth = hole_every ? (i & 1u) : 0 };
        if (!hole)
            at += n + 1u + i % 3u;
    }
}

static uint64_t ParcelBlocksOf(const Strip* s, uint32_t n)
{
    uint64_t blocks = 0;
    for (uint32_t i = 0; i < n; i++)
        blocks += s[i].count;
    return blocks;
}

/* A piece of Strip `s`: `len` of its blocks from its `off`-th. A DATA piece
 * begins where its blocks do; a hole owns no ground, and keeps its start. */
static Strip ParcelPiece(const Strip* s, uint64_t off, uint64_t len)
{
    Strip p = *s;
    p.start = s->kind == STRIP_DATA ? s->start + off : s->start;
    p.count = (uint32_t)len;
    return p;
}

/*
 * The flat reference: `sp` applied by hand to the n Strips of `flat`, into
 * `out` — the Strips before block `at`, cut there, then the inserted ones,
 * then the Strips from at + remove on, cut there — and the DATA ground in
 * between appended to `took`. Both have room for PARCEL_ROOM; false past it.
 */
static bool ParcelFlatSplice(const Strip* flat, uint32_t n, const BookSplice* sp, Strip* out,
                             uint32_t* nout, Strip* took, uint32_t* ntook)
{
    uint64_t pos = 0, cut = sp->at + sp->remove;
    *nout = 0;
    for (uint32_t i = 0; i < n; pos += flat[i].count, i++) {
        uint64_t end = pos + flat[i].count;
        uint64_t lo  = pos > sp->at ? pos : sp->at, hi = end < cut ? end : cut;
        if (pos < sp->at) {
            if (*nout == PARCEL_ROOM)
                return false;
            out[(*nout)++] = ParcelPiece(&flat[i], 0, (end < sp->at ? end : sp->at) - pos);
        }
        if (lo < hi && flat[i].kind == STRIP_DATA) {
            if (*ntook == PARCEL_ROOM)
                return false;
            took[(*ntook)++] = ParcelPiece(&flat[i], lo - pos, hi - lo);
        }
    }
    for (uint64_t k = 0; k < sp->ninsert; k++) {
        if (*nout == PARCEL_ROOM)
            return false;
        out[(*nout)++] = sp->insert[k];
    }
    pos = 0;
    for (uint32_t i = 0; i < n; pos += flat[i].count, i++) {
        uint64_t end = pos + flat[i].count;
        if (end > cut) {
            uint64_t off = cut > pos ? cut - pos : 0;
            if (*nout == PARCEL_ROOM)
                return false;
            out[(*nout)++] = ParcelPiece(&flat[i], off, flat[i].count - off);
        }
    }
    return true;
}

/* Whether the record's Strips are `want`, Strip for Strip, and its counts
 * theirs. */
static bool ParcelHolds(const FileRecord* rec, const Strip* want, uint32_t n)
{
    Strip*   flat  = NULL;
    uint32_t count = 0;
    bool     same  = FileRecordFlatten(rec, &flat, &count) == OK && count == n &&
                     rec->strip_count == n && rec->blocks == ParcelBlocksOf(want, n) &&
                     (n == 0 || memcmp(flat, want, sizeof(Strip) * n) == 0);
    kfree(flat);
    return same;
}

/* Whether two lists of DATA Strips name the same ground; both are reordered. */
static bool ParcelSameGround(Strip* a, uint64_t na, Strip* b, uint64_t nb)
{
    uint64_t ga = StripsAsGround(a, na), gb = StripsAsGround(b, nb);
    return ga == gb && (ga == 0 || memcmp(a, b, sizeof(Strip) * ga) == 0);
}

/* The distinct Parcels (`nodes` false) or Tract nodes (true) the record
 * names, into `out`: how many, or PARCEL_HEAP_ROOM + 1 past the room. */
static uint32_t ParcelHeap(const FileRecord* rec, bool nodes, const void** out)
{
    StripWalk w;
    Strip     s;
    uint32_t  n = 0;
    StripWalkBegin(rec, &w);
    while (StripWalkNext(&w, &s)) {
        uint32_t have = nodes ? w.depth : 1u;
        for (uint32_t d = 0; d < have; d++) {
            const void* seen = nodes ? (const void*)w.node[d] : (const void*)w.parcel;
            bool        held = false;
            for (uint32_t j = 0; j < n && !held; j++)
                held = out[j] == seen;
            if (held)
                continue;
            if (n == PARCEL_HEAP_ROOM)
                return PARCEL_HEAP_ROOM + 1u;
            out[n++] = seen;
        }
    }
    return n;
}

typedef struct {
    const void* now[PARCEL_HEAP_ROOM];
    const void* was[PARCEL_HEAP_ROOM];
} ParcelHeapRoom;

/* What a change built that the record `was` does not name: the heap it took —
 * the new header's bytes, and every Parcel and node not shared — and in
 * *parcels how many of the Parcels it built. UINT64_MAX past the room. */
static uint64_t ParcelBuilt(const FileRecord* now, const FileRecord* was, ParcelHeapRoom* room,
                            uint32_t* parcels)
{
    uint64_t bytes = now->bytes - (now->tract ? now->tract->bytes : 0);
    *parcels = 0;
    for (uint32_t pass = 0; pass < 2; pass++) {
        uint32_t na = ParcelHeap(now, pass == 1, room->now);
        uint32_t nb = ParcelHeap(was, pass == 1, room->was);
        if (na > PARCEL_HEAP_ROOM || nb > PARCEL_HEAP_ROOM)
            return UINT64_MAX;
        for (uint32_t i = 0; i < na; i++) {
            bool shared = false;
            for (uint32_t j = 0; j < nb && !shared; j++)
                shared = room->now[i] == room->was[j];
            if (shared)
                continue;
            if (pass == 0) {
                bytes += ParcelBytes(((const Parcel*)room->now[i])->count);
                (*parcels)++;
            } else {
                bytes += sizeof(Tract) +
                         (uint64_t)((const Tract*)room->now[i])->count * sizeof(TractKid);
            }
        }
    }
    return bytes;
}

/* Every Parcel of the record within PARCEL_STRIPS_MOST, and — when there is
 * more than one — at least PARCEL_STRIPS_LEAST. */
static bool ParcelsInBounds(const FileRecord* rec, ParcelHeapRoom* room)
{
    uint32_t n = ParcelHeap(rec, false, room->now);
    if (n > PARCEL_HEAP_ROOM)
        return false;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = ((const Parcel*)room->now[i])->count;
        if (c > PARCEL_STRIPS_MOST || (n > 1 && c < PARCEL_STRIPS_LEAST))
            return false;
    }
    return true;
}

/* What every test here holds, let go whatever way out it took: the test
 * file's Strips (`nfile` of them), the flat reference's two buffers and the
 * ground it took out, the record before a change and after it. */
typedef struct {
    Strip*          file;
    uint32_t        nfile;
    Strip*          want;
    uint32_t        nwant;
    Strip*          work;
    Strip*          took;
    uint32_t        ntook;
    Strip*          freed;
    uint64_t        nfreed;
    FileRecord*     rec;
    FileRecord*     next;
    ParcelHeapRoom* heap;
} ParcelHeld;

static bool ParcelHeldTake(ParcelHeld* h)
{
    memset(h, 0, sizeof(*h));
    h->file = kmalloc(sizeof(Strip) * PARCEL_ROOM);
    h->want = kmalloc(sizeof(Strip) * PARCEL_ROOM);
    h->work = kmalloc(sizeof(Strip) * PARCEL_ROOM);
    h->took = kmalloc(sizeof(Strip) * PARCEL_ROOM);
    h->heap = kmalloc(sizeof(ParcelHeapRoom));
    return h->file && h->want && h->work && h->took && h->heap;
}

static void ParcelHeldLetGo(ParcelHeld* h)
{
    kfree(h->file);
    kfree(h->want);
    kfree(h->work);
    kfree(h->took);
    kfree(h->freed);
    kfree(h->heap);
    FileRecordRelease(h->rec);
    FileRecordRelease(h->next);
    memset(h, 0, sizeof(*h));
}

/* The record of h->file's first `count` Strips, every `hole_every`-th a hole. */
static FileRecord* ParcelFile(ParcelHeld* h, uint32_t count, uint32_t hole_every)
{
    TagFSMetadata meta;
    ParcelStrips(h->file, count, hole_every, PARCEL_FIRST_BLOCK);
    h->nfile = count;
    memset(&meta, 0, sizeof(meta));
    meta.file_id      = 9;
    meta.flags        = TAGFS_FILE_ACTIVE;
    meta.size         = 1;
    meta.filename     = s_parcel_name;
    meta.extents      = h->file;
    meta.extent_count = count;
    FileRecordRelease(h->rec);
    return h->rec = FileRecordBuild(&meta);
}

/* Where Strip `i` of the test file begins among the file's blocks. */
static uint64_t ParcelAt(const ParcelHeld* h, uint32_t i)
{
    return ParcelBlocksOf(h->file, i);
}

/* `count` splices applied to h->rec into h->next and by hand to h->file into
 * h->want (h->took the ground the hand took out): whether the new record holds
 * what the hand made, Strip for Strip. False too when the splice was refused. */
static bool ParcelSpliced(ParcelHeld* h, const BookSplice* sp, uint64_t count)
{
    memcpy(h->work, h->file, sizeof(Strip) * h->nfile);
    uint32_t nwork = h->nfile;
    h->ntook = 0;
    for (uint64_t i = 0; i < count; i++) {
        if (!ParcelFlatSplice(h->work, nwork, &sp[i], h->want, &h->nwant, h->took, &h->ntook))
            return false;
        memcpy(h->work, h->want, sizeof(Strip) * h->nwant);
        nwork = h->nwant;
    }
    FileRecordRelease(h->next);
    kfree(h->freed);
    h->next  = NULL;
    h->freed = NULL;
    return FileRecordSplice(h->rec, sp, count, h->rec->size, 7, &h->next, &h->freed,
                            &h->nfreed) == OK &&
           ParcelHolds(h->next, h->want, h->nwant);
}

// ----------------------------------------------------------------------------
// Built, found, relabelled
// ----------------------------------------------------------------------------

/* A record built from Strips gives back the same Strips, one for one, cut
 * into Parcels of at most 128 — none below 64 when there are two or more —
 * for no Strips, one, a Parcel's worth, one past it, and a file of 1000. */
static TestResult TestParcelBuild(void)
{
    static const uint32_t sizes[] = { 0, 1, PARCEL_STRIPS_MOST, PARCEL_STRIPS_MOST + 1u,
                                      PARCEL_FILE_STRIPS };
    TestResult verdict = TEST_PASS;
    ParcelHeld h;

    TEST_ASSERT(ParcelHeldTake(&h), "room for the test's Strips");
    for (uint32_t s = 0; s < ARRAY_SIZE(sizes); s++) {
        TEST_ASSERT(ParcelFile(&h, sizes[s], 5u) != NULL, "a record is built");
        TEST_ASSERT(ParcelHolds(h.rec, h.file, sizes[s]), "it gives back the Strips it was built of");
        TEST_ASSERT(sizes[s] == 0 ? h.rec->tract == NULL : h.rec->tract != NULL,
                    "a record of no Strips names no Tract, any other one does");
        uint32_t parcels = ParcelHeap(h.rec, false, h.heap->now);
        TEST_ASSERT_EQ(parcels, (sizes[s] + PARCEL_STRIPS_MOST - 1u) / PARCEL_STRIPS_MOST,
                       "cut into as few Parcels as 128 Strips each allows");
        for (uint32_t i = 0; i < parcels; i++) {
            uint32_t c = ((const Parcel*)h.heap->now[i])->count;
            TEST_ASSERT(c <= PARCEL_STRIPS_MOST && (parcels == 1 || c >= PARCEL_STRIPS_MOST / 2u),
                        "each at most 128 Strips, and at least 64 beside another");
        }
    }
out:
    ParcelHeldLetGo(&h);
    return verdict;
}

/* Every block of a file of 1000 Strips, holes among them, is found where the
 * Strips say — its kind, its block, and how many follow it in its Strip — and
 * the block past the last is not. */
static TestResult TestParcelFind(void)
{
    TestResult verdict = TEST_PASS;
    ParcelHeld h;
    FileSpot   spot;
    uint64_t   k = 0;

    TEST_ASSERT(ParcelHeldTake(&h), "room for the test's Strips");
    TEST_ASSERT(ParcelFile(&h, PARCEL_FILE_STRIPS, 7u) != NULL, "the file of 1000 Strips is built");
    for (uint32_t i = 0; i < h.nfile; i++) {
        for (uint32_t b = 0; b < h.file[i].count; b++, k++) {
            TEST_ASSERT(FileRecordFind(h.rec, k, &spot), "a block inside the runs is found");
            TEST_ASSERT_EQ(spot.kind, h.file[i].kind, "of its Strip's kind");
            TEST_ASSERT_EQ(spot.block, h.file[i].kind == STRIP_DATA ? h.file[i].start + b : 0,
                           "where its Strip lays it — a hole on no block");
            TEST_ASSERT_EQ(spot.left, h.file[i].count - b - 1u, "with the rest of its Strip after it");
        }
    }
    TEST_ASSERT(!FileRecordFind(h.rec, k, &spot), "and the block past the last is not");
out:
    ParcelHeldLetGo(&h);
    return verdict;
}

/* A rename shares every run: the new record names the old one's Tract, and
 * the old record is as it was. */
static TestResult TestParcelRelabel(void)
{
    TestResult verdict = TEST_PASS;
    ParcelHeld h;
    uint32_t   tags[2] = { 17, 23 };

    TEST_ASSERT(ParcelHeldTake(&h), "room for the test's Strips");
    TEST_ASSERT(ParcelFile(&h, PARCEL_FILE_STRIPS, 0) != NULL, "the file of 1000 Strips is built");
    h.next = FileRecordRelabel(h.rec, s_parcel_other, tags, 2, TAGFS_FILE_ACTIVE, 0);
    TEST_ASSERT(h.next != NULL, "a new name and two tags make a new record");
    TEST_ASSERT(h.next->tract == h.rec->tract, "which names the same Tract, nothing of it copied");
    TEST_ASSERT(h.next->tag_count == 2 && h.next->tags[1] == 23 &&
                strcmp(h.next->name, s_parcel_other) == 0, "under the new name and tags");
    TEST_ASSERT(h.rec->tag_count == 0 && strcmp(h.rec->name, s_parcel_name) == 0 &&
                ParcelHolds(h.rec, h.file, h.nfile), "and the old record is as it was");
    TEST_ASSERT(ParcelHolds(h.next, h.file, h.nfile), "both naming the same runs");
out:
    ParcelHeldLetGo(&h);
    return verdict;
}

// ----------------------------------------------------------------------------
// Spliced
// ----------------------------------------------------------------------------

/* A splice across the boundary of two Parcels — the last Strips of one and
 * the first of the next taken out, three new ones put in — builds those two
 * Parcels anew and shares every other, and frees exactly the ground it took
 * out. */
static TestResult TestParcelSpliceAcross(void)
{
    TestResult verdict = TEST_PASS;
    ParcelHeld h;
    Strip      put[3];
    uint32_t   built = 0;

    TEST_ASSERT(ParcelHeldTake(&h), "room for the test's Strips");
    TEST_ASSERT(ParcelFile(&h, PARCEL_FILE_STRIPS, 0) != NULL, "the file of 1000 Strips is built");
    uint32_t edge = PARCEL_FILE_STRIPS / 8u * 3u;       /* 8 Parcels of 125: where the fourth begins */
    ParcelStrips(put, 3, 0, PARCEL_FRESH_BLOCK);
    BookSplice sp = { .at = ParcelAt(&h, edge - 2u), .ninsert = 3, .insert = put,
                      .remove = ParcelAt(&h, edge + 2u) - ParcelAt(&h, edge - 2u) };
    TEST_ASSERT(ParcelSpliced(&h, &sp, 1), "the record holds what the splice by hand made");
    TEST_ASSERT(ParcelsInBounds(h.next, h.heap), "every Parcel within its bounds");
    TEST_ASSERT(ParcelBuilt(h.next, h.rec, h.heap, &built) != UINT64_MAX, "the Parcels counted");
    TEST_ASSERT_EQ(built, 2, "the two Parcels it crossed built anew, the other six shared");
    TEST_ASSERT(ParcelSameGround(h.freed, h.nfreed, h.took, h.ntook), "it frees what it took out");
out:
    ParcelHeldLetGo(&h);
    return verdict;
}

/* 200 Strips put into the middle of a Parcel of 125: the Parcel past the most
 * is cut into Parcels within it, and the runs are the hand's. */
static TestResult TestParcelSpliceDivides(void)
{
    TestResult verdict = TEST_PASS;
    ParcelHeld h;
    Strip*     put     = kmalloc(sizeof(Strip) * 200u);

    TEST_ASSERT(put != NULL && ParcelHeldTake(&h), "room for the test's Strips");
    TEST_ASSERT(ParcelFile(&h, PARCEL_FILE_STRIPS, 0) != NULL, "the file of 1000 Strips is built");
    ParcelStrips(put, 200u, 9u, PARCEL_FRESH_BLOCK);
    BookSplice sp = { .at = ParcelAt(&h, 300u), .remove = 0, .ninsert = 200u, .insert = put };
    TEST_ASSERT(ParcelSpliced(&h, &sp, 1), "the record holds what the splice by hand made");
    TEST_ASSERT(ParcelsInBounds(h.next, h.heap), "no Parcel past 128 Strips, none below 32");
    TEST_ASSERT_EQ(h.nfreed, 0, "and a splice that took nothing out frees nothing");
out:
    kfree(put);
    ParcelHeldLetGo(&h);
    return verdict;
}

/* Three splices in one change, each inside another Parcel: the three are
 * built anew and the other five shared — a change touches k Parcels, and pays
 * for k. */
static TestResult TestParcelSpliceK(void)
{
    TestResult verdict = TEST_PASS;
    ParcelHeld h;
    Strip      put[6];
    uint32_t   built = 0;
    BookSplice sp[3];

    TEST_ASSERT(ParcelHeldTake(&h), "room for the test's Strips");
    TEST_ASSERT(ParcelFile(&h, PARCEL_FILE_STRIPS, 0) != NULL, "the file of 1000 Strips is built");
    for (uint32_t i = 0; i < 3; i++) {
        uint32_t s = 125u * (1u + 3u * i) + 60u;        /* the middle of Parcels 1, 4 and 7 */
        for (uint32_t j = 0; j < 2; j++)                /* two Strips on fresh ground, as long as the two they replace */
            put[2 * i + j] = (Strip){ .start = PARCEL_FRESH_BLOCK + 16u * (2 * i + j),
                                      .count = h.file[s + j].count, .kind = STRIP_DATA };
        sp[i] = (BookSplice){ .at = ParcelAt(&h, s), .ninsert = 2, .insert = &put[2 * i],
                              .remove = (uint64_t)h.file[s].count + h.file[s + 1].count };
    }
    TEST_ASSERT(ParcelSpliced(&h, sp, 3), "the record holds what the three splices by hand made");
    TEST_ASSERT(ParcelBuilt(h.next, h.rec, h.heap, &built) != UINT64_MAX, "the Parcels counted");
    TEST_ASSERT_EQ(built, 3, "the three Parcels touched built anew, the other five shared");
    TEST_ASSERT(ParcelSameGround(h.freed, h.nfreed, h.took, h.ntook), "it frees what it took out");
out:
    ParcelHeldLetGo(&h);
    return verdict;
}

/* The file cut short from inside a DATA Strip of its seventh Parcel to its
 * end, holes in what goes: the record ends where the cut is, and frees the
 * DATA ground cut off — never a hole, which owns none. */
static TestResult TestParcelTruncate(void)
{
    TestResult verdict = TEST_PASS;
    ParcelHeld h;
    uint32_t   s = 125u * 6u + 10u;

    TEST_ASSERT(ParcelHeldTake(&h), "room for the test's Strips");
    TEST_ASSERT(ParcelFile(&h, PARCEL_FILE_STRIPS, 7u) != NULL, "the file of 1000 Strips is built");
    while (s < h.nfile && (h.file[s].kind != STRIP_DATA || h.file[s].count < 2u))
        s++;
    TEST_ASSERT(s < h.nfile, "a DATA Strip of two blocks or more to cut inside");
    BookSplice sp = { .at = ParcelAt(&h, s) + 1u, .ninsert = 0, .insert = NULL };
    sp.remove = h.rec->blocks - sp.at;
    TEST_ASSERT(ParcelSpliced(&h, &sp, 1), "the record holds what the cut by hand made");
    TEST_ASSERT_EQ(h.next->blocks, sp.at, "and ends where the cut is");
    TEST_ASSERT(ParcelsInBounds(h.next, h.heap), "every Parcel within its bounds");
    for (uint64_t i = 0; i < h.nfreed; i++)
        TEST_ASSERT_EQ(h.freed[i].kind, STRIP_DATA, "only DATA ground is freed");
    TEST_ASSERT(ParcelSameGround(h.freed, h.nfreed, h.took, h.ntook),
                "it frees the DATA ground cut off, and nothing of the holes");
out:
    ParcelHeldLetGo(&h);
    return verdict;
}

/* 110 of a Parcel's 125 Strips taken out leave 15, below the least: they are
 * joined to a neighbour, and every Parcel stays within its bounds. */
static TestResult TestParcelJoins(void)
{
    TestResult verdict = TEST_PASS;
    ParcelHeld h;

    TEST_ASSERT(ParcelHeldTake(&h), "room for the test's Strips");
    TEST_ASSERT(ParcelFile(&h, PARCEL_FILE_STRIPS, 0) != NULL, "the file of 1000 Strips is built");
    BookSplice sp = { .at = ParcelAt(&h, 130u), .ninsert = 0, .insert = NULL,
                      .remove = ParcelAt(&h, 240u) - ParcelAt(&h, 130u) };
    TEST_ASSERT(ParcelSpliced(&h, &sp, 1), "the record holds what the splice by hand made");
    TEST_ASSERT(ParcelsInBounds(h.next, h.heap), "the 15 left are joined, no Parcel below 32");
out:
    ParcelHeldLetGo(&h);
    return verdict;
}

/* Ground a change takes out at one place and puts back at another is not
 * freed — only what no Strip names after it is — and a splice that takes out
 * only a hole frees nothing. */
static TestResult TestParcelGroundMoves(void)
{
    TestResult verdict = TEST_PASS;
    ParcelHeld h;
    Strip      fresh;
    Strip      moved;
    Strip      gone;
    BookSplice sp[2];

    TEST_ASSERT(ParcelHeldTake(&h), "room for the test's Strips");
    TEST_ASSERT(ParcelFile(&h, PARCEL_FILE_STRIPS, 7u) != NULL, "the file of 1000 Strips is built");
    TEST_ASSERT(h.file[100].kind == STRIP_DATA && h.file[600].kind == STRIP_DATA &&
                h.file[6].kind == STRIP_HOLE, "Strips 100 and 600 lie on ground, 6 is a hole");
    fresh = (Strip){ .start = PARCEL_FRESH_BLOCK, .count = h.file[100].count, .kind = STRIP_DATA };
    moved = h.file[100];
    gone  = h.file[600];
    sp[0] = (BookSplice){ .at = ParcelAt(&h, 100u), .remove = h.file[100].count,
                          .ninsert = 1, .insert = &fresh };
    sp[1] = (BookSplice){ .at = ParcelAt(&h, 600u), .remove = h.file[600].count,
                          .ninsert = 1, .insert = &moved };
    TEST_ASSERT(ParcelSpliced(&h, sp, 2), "the record holds what the two splices by hand made");
    TEST_ASSERT(ParcelSameGround(h.freed, h.nfreed, &gone, 1),
                "only Strip 600's ground is freed — Strip 100's is named again further on");

    sp[0] = (BookSplice){ .at = ParcelAt(&h, 6u), .remove = h.file[6].count, .ninsert = 0 };
    TEST_ASSERT(ParcelSpliced(&h, sp, 1), "a hole taken out");
    TEST_ASSERT_EQ(h.nfreed, 0, "frees nothing");
out:
    ParcelHeldLetGo(&h);
    return verdict;
}

/* A file of DATA and holes: 5 blocks on ground from 100, a hole of 7 whose
 * start reads as block 200, 4 blocks from 105 — where the first ground ended
 * — a hole of 3, and 2 blocks from 300. 11 blocks of ground, in three runs. */
static const Strip s_parcel_holed[] = {
    { .start = 100, .count = 5, .kind = STRIP_DATA },
    { .start = 200, .count = 7, .kind = STRIP_HOLE },
    { .start = 105, .count = 4, .kind = STRIP_DATA },
    { .start = 0,   .count = 3, .kind = STRIP_HOLE },
    { .start = 300, .count = 2, .kind = STRIP_DATA },
};

/* The record of `count` of the Strips above, as a file of id 9 named parcel. */
static FileRecord* ParcelHoled(uint32_t from, uint32_t count)
{
    TagFSMetadata meta;
    memset(&meta, 0, sizeof(meta));
    meta.file_id      = 9;
    meta.flags        = TAGFS_FILE_ACTIVE;
    meta.filename     = s_parcel_name;
    meta.extents      = (Strip*)&s_parcel_holed[from];
    meta.extent_count = count;
    return FileRecordBuild(&meta);
}

/* Every walk that owns ground, gives it back or guards it passes over a hole,
 * which owns none: the walk of a record's ground gives its three DATA Strips
 * in order; a Fallow node of the record holds 11 blocks, and a record of holes
 * alone makes none; the kernel guard — of the record, and of runs laid flat —
 * with the Deed pointed at the hole's start finds no kernel there, and at a
 * block of ground finds it; the mount's census
 * claims the 11 blocks as the file's and nothing at the holes' starts; and
 * the count of seeks reads one gap — none across the hole between 105 and the
 * ground that ended there — over 10 steps. */
static TestResult TestParcelHoleOwnsNothing(void)
{
    TestResult  verdict = TEST_PASS;
    TagFSState* fs      = tagfs_get_state();
    VolumeBoot  boot    = fs->boot;
    FileRecord* rec     = ParcelHoled(0, ARRAY_SIZE(s_parcel_holed));
    FileRecord* holes   = ParcelHoled(1, 1);
    FallowNode* node    = NULL;
    FallowNode* none    = NULL;
    bool        census  = false;
    uint32_t    gaps    = 0;
    uint64_t    steps   = 0, n = 0;
    StripWalk   walk;
    Strip       run;

    TEST_ASSERT(rec != NULL && holes != NULL, "the records are built");
    StripWalkBegin(rec, &walk);
    for (uint32_t i = 0; i < ARRAY_SIZE(s_parcel_holed); i++) {
        if (s_parcel_holed[i].kind != STRIP_DATA)
            continue;
        TEST_ASSERT(StripWalkNextGround(&walk, &run) && run.start == s_parcel_holed[i].start &&
                    run.count == s_parcel_holed[i].count, "the walk of ground gives each DATA Strip");
        n++;
    }
    TEST_ASSERT(!StripWalkNextGround(&walk, &run) && n == 3, "three of them, and no hole");

    TEST_ASSERT_OK(FallowNodeOfRecord(rec, FALLOW_BY_RECORD, &node), "a Fallow node of the record");
    TEST_ASSERT_EQ(FallowNodeBlocks(node), 11, "holds the record's 11 blocks of ground");
    TEST_ASSERT_OK(FallowNodeOfRecord(holes, FALLOW_BY_RECORD, &none), "a record of a hole alone");
    TEST_ASSERT(none == NULL, "gives back no ground");

    fs->boot.kernel_blocks = 1;
    fs->boot.kernel_block  = 200u;
    TEST_ASSERT(!InkRecordTouchesBoot(rec), "the Deed at the hole's start finds no kernel in it");
    TEST_ASSERT(!InkTouchesBoot(&s_parcel_holed[1], 1), "nor in the hole laid flat, as a Plot's runs are");
    fs->boot.kernel_block  = 107u;
    TEST_ASSERT(InkRecordTouchesBoot(rec), "at a block of its ground, the kernel");
    TEST_ASSERT(InkTouchesBoot(&s_parcel_holed[2], 1), "and in that ground laid flat");
    fs->boot = boot;

    GroundCensusBegin();
    census = true;
    StripWalkBegin(rec, &walk);
    while (StripWalkNextGround(&walk, &run))
        GroundClaim(GROUND_FILE, run.start, run.count, 9);
    for (uint64_t b = 100; b < 109; b++)
        TEST_ASSERT_EQ(GroundClaimOf(b), GROUND_FILE, "the census claims the ground as the file's");
    TEST_ASSERT_EQ(GroundClaimOf(301), GROUND_FILE, "all of it");
    TEST_ASSERT_EQ(GroundClaimOf(200), GROUND_OWNER_KINDS, "and nothing at a hole's start");
    TEST_ASSERT_EQ(GroundClaimOf(0), GROUND_OWNER_KINDS, "at either hole's");

    TagFSFragOfRecord(rec, &gaps, &steps);
    TEST_ASSERT_EQ(gaps, 1, "one gap — to 300; the ground past the hole begins where it ended");
    TEST_ASSERT_EQ(steps, 10, "over the 10 steps between its 11 blocks");
out:
    fs->boot = boot;
    if (census)
        GroundCensusEnd();
    FallowNodeLetGo(node);
    FallowNodeLetGo(none);
    FileRecordRelease(rec);
    FileRecordRelease(holes);
    return verdict;
}

// ----------------------------------------------------------------------------
// What an Ink conveys
// ----------------------------------------------------------------------------

/* The Plot's runs `runs` conveyed from h->rec (PlotConveyance) and the
 * splices applied: whether the new record holds the runs Strip for Strip and
 * frees the ground `expect` names, with the number of splices in *count. */
static bool ParcelConveyed(ParcelHeld* h, Strip* runs, uint32_t nruns, Strip* expect,
                           uint64_t nexpect, uint64_t* count)
{
    Plot        p;
    BookSplice* sp = NULL;
    memset(&p, 0, sizeof(p));
    p.fid       = 9;
    p.runs      = runs;
    p.run_count = nruns;
    *count = UINT64_MAX;
    FileRecordRelease(h->next);
    kfree(h->freed);
    h->next  = NULL;
    h->freed = NULL;
    bool same = PlotConveyance(&p, h->rec, &sp, count) == OK &&
                FileRecordSplice(h->rec, sp, *count, h->rec->size, 7, &h->next, &h->freed,
                                 &h->nfreed) == OK &&
                ParcelHolds(h->next, runs, nruns) &&
                ParcelSameGround(h->freed, h->nfreed, expect, nexpect);
    kfree(sp);
    return same;
}

/* The runs as the record has them, with the Strips from `from` up to `to`
 * gathered into one on fresh ground: into h->work, their ground into h->took;
 * how many Strips. */
static uint32_t ParcelGathered(ParcelHeld* h, uint32_t from, uint32_t to)
{
    uint32_t n = 0;
    h->ntook = 0;
    for (uint32_t i = 0; i < from; i++)
        h->work[n++] = h->file[i];
    h->work[n++] = (Strip){ .start = PARCEL_FRESH_BLOCK, .kind = STRIP_DATA,
                            .count = (uint32_t)(ParcelAt(h, to) - ParcelAt(h, from)) };
    for (uint32_t i = from; i < to; i++)
        h->took[h->ntook++] = h->file[i];
    for (uint32_t i = to; i < h->nfile; i++)
        h->work[n++] = h->file[i];
    return n;
}

/* What a Plot did to the runs it loaded comes back as splices: none for runs
 * as they were; one for two Strips added at the end, for a cut inside a
 * Strip, for ten Strips gathered into one, for a Strip split in two where it
 * lay; two for a gather and an append together — and applying them gives the
 * Plot's runs Strip for Strip, freeing exactly the ground they let go. */
static TestResult TestParcelConveyance(void)
{
    TestResult verdict = TEST_PASS;
    ParcelHeld h;
    uint64_t   count   = 0;
    uint32_t   n;

    TEST_ASSERT(ParcelHeldTake(&h), "room for the test's Strips");
    TEST_ASSERT(ParcelFile(&h, PARCEL_FILE_STRIPS, 0) != NULL, "the file of 1000 Strips is built");

    memcpy(h.work, h.file, sizeof(Strip) * h.nfile);
    TEST_ASSERT(ParcelConveyed(&h, h.work, h.nfile, NULL, 0, &count) && count == 0,
                "runs as they were convey nothing");

    ParcelStrips(&h.work[h.nfile], 2, 0, PARCEL_FRESH_BLOCK);
    TEST_ASSERT(ParcelConveyed(&h, h.work, h.nfile + 2u, NULL, 0, &count) && count == 1,
                "two Strips at the end: one splice, nothing freed");

    for (n = 900u; h.file[n].count < 2u; n++)
        ;
    h.ntook = 0;
    h.took[h.ntook++] = ParcelPiece(&h.file[n], 1, h.file[n].count - 1u);
    for (uint32_t i = n + 1u; i < h.nfile; i++)
        h.took[h.ntook++] = h.file[i];
    memcpy(h.work, h.file, sizeof(Strip) * h.nfile);
    h.work[n].count = 1;
    TEST_ASSERT(ParcelConveyed(&h, h.work, n + 1u, h.took, h.ntook, &count) && count == 1,
                "a cut inside a Strip: one splice, the ground cut off freed");

    n = ParcelGathered(&h, 100u, 110u);
    TEST_ASSERT(ParcelConveyed(&h, h.work, n, h.took, h.ntook, &count) && count == 1,
                "ten Strips gathered into one: one splice, their ground freed");

    n = ParcelGathered(&h, 100u, 110u);
    ParcelStrips(&h.work[n], 2, 0, PARCEL_FRESH_BLOCK + 64u);
    TEST_ASSERT(ParcelConveyed(&h, h.work, n + 2u, h.took, h.ntook, &count) && count == 2,
                "a gather and an append: two splices");

    memcpy(h.work, h.file, sizeof(Strip) * h.nfile);
    for (n = 500u; h.file[n].count < 2u; n++)
        ;
    memmove(&h.work[n + 1u], &h.work[n], sizeof(Strip) * (h.nfile - n));
    h.work[n]      = ParcelPiece(&h.file[n], 0, 1);
    h.work[n + 1u] = ParcelPiece(&h.file[n], 1, h.file[n].count - 1u);
    TEST_ASSERT(ParcelConveyed(&h, h.work, h.nfile + 1u, NULL, 0, &count) && count == 1,
                "a Strip split in two where it lay: one splice — the record keeps the Strips as "
                "they are given — and nothing freed");
out:
    ParcelHeldLetGo(&h);
    return verdict;
}

/* A file of 1000 runs as format v1 has them, one of them written over onto
 * fresh ground: the change builds a header, the one Parcel holding that run
 * and the node over it — far less than the flat copy of every run each change
 * of the file made (24 bytes a run, 24000 here). */
static TestResult TestParcelCopyBytes(void)
{
    TestResult verdict = TEST_PASS;
    ParcelHeld h;
    uint64_t   count   = 0;
    uint64_t   bytes   = 0;
    uint32_t   built   = 0;

    TEST_ASSERT(ParcelHeldTake(&h), "room for the test's Strips");
    TEST_ASSERT(ParcelFile(&h, PARCEL_FILE_STRIPS, 0) != NULL, "the file of 1000 runs is built");
    memcpy(h.work, h.file, sizeof(Strip) * h.nfile);
    h.work[500].start = PARCEL_FRESH_BLOCK;
    h.took[0]         = h.file[500];
    TEST_ASSERT(ParcelConveyed(&h, h.work, h.nfile, h.took, 1, &count) && count == 1,
                "one run written over onto fresh ground: one splice, its old ground freed");
    bytes = ParcelBuilt(h.next, h.rec, h.heap, &built);
    TestEvidence("bytes the change built", bytes);
    TEST_ASSERT_EQ(built, 1, "one Parcel built, the other seven shared");
    TEST_ASSERT(bytes < PARCEL_FILE_STRIPS * sizeof(Strip) / 4u,
                "the header, one Parcel and the node over it: under a quarter of the flat copy");
out:
    ParcelHeldLetGo(&h);
    return verdict;
}

// ----------------------------------------------------------------------------
// Read
// ----------------------------------------------------------------------------

static uint64_t ParcelLog2(uint64_t n)
{
    return n > 1 ? 64u - (uint64_t)__builtin_clzll(n - 1u) : 1u;
}

/* A file of 1000 holes of 1 to 4 blocks read from end to end through the
 * read's own stream: zeros, with no block of the medium asked, and the blocks
 * found in at most 2 n log2 n steps down the Tract and inside its Parcels — a
 * walk from the first run at every run's end would take ~n^2/2. */
static TestResult TestParcelReadSteps(void)
{
    TestResult      verdict = TEST_PASS;
    ParcelHeld      h;
    uint8_t*        sink_at = kmalloc(PARCEL_READ_SINK);
    TagFSFileHandle hd;
    uint64_t        bound   = 2u * PARCEL_FILE_STRIPS * ParcelLog2(PARCEL_FILE_STRIPS);
    uint64_t        steps   = 0;

    memset(&hd, 0, sizeof(hd));
    TEST_ASSERT(sink_at != NULL && ParcelHeldTake(&h), "room for the test's Strips and the read");
    TEST_ASSERT(ParcelFile(&h, PARCEL_FILE_STRIPS, 1u) != NULL, "a file of 1000 holes is built");
    hd.file_id   = 9;
    hd.rec       = h.rec;
    hd.file_size = h.rec->blocks * TAGFS_BLOCK_SIZE;
    while (hd.offset < hd.file_size) {
        ReadSink sink = { .kernel = sink_at, .bytes = PARCEL_READ_SINK };
        uint64_t got  = 0;
        memset(sink_at, 0xA5, PARCEL_READ_SINK);
        TEST_ASSERT_OK(TagFSReadStream(&hd, &sink, &got), "a read of holes is answered");
        TEST_ASSERT(got > 0, "and moves");
        for (uint64_t i = 0; i < got; i++)
            TEST_ASSERT_EQ(sink_at[i], 0, "a hole reads as zeros");
    }
    steps = h.rec->find_steps;
    TestEvidence("steps down the Tract and inside Parcels for the whole file", steps);
    TEST_ASSERT(steps <= bound, "the read finds its blocks in O(n log n)");
out:
    kfree(sink_at);
    ParcelHeldLetGo(&h);
    return verdict;
}

static const TestCase g_parcel_tests[] = {
    { .name = "parcel_build",             .func = TestParcelBuild,           .reach = TEST_READS_ONLY },
    { .name = "parcel_find",              .func = TestParcelFind,            .reach = TEST_READS_ONLY },
    { .name = "parcel_relabel",           .func = TestParcelRelabel,         .reach = TEST_READS_ONLY },
    { .name = "parcel_splice_across",     .func = TestParcelSpliceAcross,    .reach = TEST_READS_ONLY },
    { .name = "parcel_splice_divides",    .func = TestParcelSpliceDivides,   .reach = TEST_READS_ONLY },
    { .name = "parcel_splice_k",          .func = TestParcelSpliceK,         .reach = TEST_READS_ONLY },
    { .name = "parcel_truncate",          .func = TestParcelTruncate,        .reach = TEST_READS_ONLY },
    { .name = "parcel_joins",             .func = TestParcelJoins,           .reach = TEST_READS_ONLY },
    { .name = "parcel_ground_moves",      .func = TestParcelGroundMoves,     .reach = TEST_READS_ONLY },
    { .name = "parcel_hole_owns_nothing", .func = TestParcelHoleOwnsNothing, .reach = TEST_READS_ONLY },
    { .name = "parcel_conveyance",        .func = TestParcelConveyance,      .reach = TEST_READS_ONLY },
    { .name = "parcel_copy_bytes",        .func = TestParcelCopyBytes,       .reach = TEST_READS_ONLY },
    { .name = "parcel_read_steps",        .func = TestParcelReadSteps,       .reach = TEST_READS_ONLY },
};

const TestSuite g_parcel_suite = {
    .tests = g_parcel_tests, .count = ARRAY_SIZE(g_parcel_tests),
};

#endif /* CONFIG_VOLUME_TESTS */
