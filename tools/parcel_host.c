/*
 * parcel_host — what a write into a file of 1 GiB copies, measured on the host
 * with the kernel's own file_map/ (`make parcel-host`; tools/parcel_host.h).
 *
 * A random overwrite of a block of a file of 1 GiB
 * copies in memory no more than one Parcel and the record's head. The stand
 * holds it on a file of 32 MiB (stand_fresh_parcel_copied) — a stand image
 * has no room for 1 GiB — so here the file is 1 GiB of runs: 262 144 blocks
 * cut into runs of 1..64 blocks, every fiftieth a hole, built into a record
 * the way the mount builds one (FileRecordBuild), and then 10 000 blocks at
 * random written over as Plot writes one over on fresh ground: a splice of
 * one block out and one fresh block in (file_record.h, BookSplice). What each
 * splice keeps of the heap — every allocation it makes and does not let go
 * before it returns, counted by the shim — is held to the bound below; the record is held to the truth, block by block,
 * as it goes and whole at the end; every Parcel to the cut every writer
 * keeps (§16.1 п.7: at most PARCEL_STRIPS_MOST Strips, at least
 * PARCEL_STRIPS_LEAST in a file of more than one).
 *
 * ‼ THE BOUND. A splice of one block in a run makes that run three Strips at
 * the most, so the one Parcel it lies in grows by two and, past the most, is
 * cut in two; the Tract's nodes on the way down to it are built again, one a
 * level; and a new head names it all — the stand's measure. Nothing else is
 * built, every other Parcel and node shared — but a node a cut leaves past
 * TRACT_KIDS_MOST is cut in two in turn, and the root grows a level over two:
 * one node more for each, which a file of 32 MiB, a Tract of one node, never
 * meets and a file of 1 GiB does. Counted and said apart.
 */

#include "tract.h"
#include "tail_slip.h"

#define FILE_BLOCKS   (1ull << 18)      /* 1 GiB of 4 KiB blocks */
#define REWRITES      10000u
#define CHECK_EVERY   1000u
#define FRESH_FROM    (1ull << 32)      /* fresh ground lies past every block the file starts on */

ParcelHostHeap g_parcel_host_heap;

/* Each allocation carries its size ahead of it, so a free counts it out. */
#define HEAP_AHEAD 16u

void* ParcelHostAlloc(size_t bytes)
{
    uint8_t* raw = malloc(bytes + HEAP_AHEAD);
    if (!raw)
        return NULL;
    memcpy(raw, &bytes, sizeof(bytes));
    g_parcel_host_heap.allocs++;
    g_parcel_host_heap.bytes += bytes;
    g_parcel_host_heap.live  += bytes;
    return raw + HEAP_AHEAD;
}

void ParcelHostFree(const void* p)
{
    size_t bytes;
    if (!p)
        return;
    uint8_t* raw = (uint8_t*)p - HEAP_AHEAD;
    memcpy(&bytes, raw, sizeof(bytes));
    g_parcel_host_heap.frees++;
    g_parcel_host_heap.live -= bytes;
    free(raw);
}

/* No record here keeps Slips. */
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

void TailSlipLetGo(const TailSlip* s)
{
    (void)s;
}

/* The kernel's KSort is a heapsort; so is this one — in place, any count. */
static void SiftDown(char* b, size_t at, size_t n, size_t size, KSortOrder order, void* ctx, char* t)
{
    for (;;) {
        size_t big = at, l = 2 * at + 1, r = l + 1;
        if (l < n && order(b + l * size, b + big * size, ctx) > 0)
            big = l;
        if (r < n && order(b + r * size, b + big * size, ctx) > 0)
            big = r;
        if (big == at)
            return;
        memcpy(t, b + at * size, size);
        memcpy(b + at * size, b + big * size, size);
        memcpy(b + big * size, t, size);
        at = big;
    }
}

void KSort(void* base, size_t count, size_t size, KSortOrder order, void* ctx)
{
    char* b = base;
    char* t = malloc(size);
    for (size_t i = count / 2; i-- > 0;)
        SiftDown(b, i, count, size, order, ctx, t);
    for (size_t n = count; n > 1; n--) {
        memcpy(t, b, size);
        memcpy(b, b + (n - 1) * size, size);
        memcpy(b + (n - 1) * size, t, size);
        SiftDown(b, 0, n - 1, size, order, ctx, t);
    }
    free(t);
}

static uint64_t s_seed = 0x9E3779B97F4A7C15ull;

static uint64_t Next(void)
{
    s_seed ^= s_seed << 13;
    s_seed ^= s_seed >> 7;
    s_seed ^= s_seed << 17;
    return s_seed;
}

static uint64_t* s_truth;     /* where each file block lies: 0 for a hole */
static unsigned  s_failed;

static void Say(bool held, const char* what)
{
    printf("  %s %s\n", held ? "ok  " : "FAIL", what);
    s_failed += !held;
}

/* The file of 1 GiB as runs of 1..64 blocks, every fiftieth a hole, no two
 * runs meeting on the medium: *count of them, and the truth laid. */
static Strip* FileRuns(uint32_t* count)
{
    Strip*   runs = malloc(sizeof(Strip) * FILE_BLOCKS);
    uint64_t file = 0, ground = 1000;
    uint32_t n = 0;
    while (file < FILE_BLOCKS) {
        uint32_t len = (uint32_t)(Next() % 64u) + 1u;
        if (len > FILE_BLOCKS - file)
            len = (uint32_t)(FILE_BLOCKS - file);
        bool hole = n % 50u == 49u;
        runs[n++] = (Strip){ hole ? 0 : ground, len, hole ? STRIP_HOLE : STRIP_DATA, 1 };
        for (uint32_t k = 0; k < len; k++)
            s_truth[file + k] = hole ? 0 : ground + k;
        if (!hole)
            ground += len + 1u;
        file += len;
    }
    *count = n;
    return runs;
}

/* Every Parcel of the record held to the cut: how many, the fewest and the
 * most Strips one holds. */
static void ParcelsCounted(const FileRecord* rec, uint64_t* parcels, uint32_t* least, uint32_t* most)
{
    StripWalk    w;
    const Strip* strips;
    uint32_t     n;
    *parcels = 0;
    *least   = UINT32_MAX;
    *most    = 0;
    StripWalkBegin(rec, &w);
    while (StripWalkParcel(&w, &strips, &n)) {
        (*parcels)++;
        *least = n < *least ? n : *least;
        *most  = n > *most ? n : *most;
    }
}

/* Blocks of the record held to the truth: `probes` at random, or every one
 * (probes 0) by a walk of its Strips. How many disagree. */
static uint64_t Disagree(const FileRecord* rec, uint32_t probes)
{
    uint64_t wrong = 0;
    if (probes) {
        for (uint32_t i = 0; i < probes; i++) {
            uint64_t k = Next() % FILE_BLOCKS;
            FileSpot spot;
            wrong += !FileRecordFind(rec, k, &spot) || spot.block != s_truth[k] ||
                     (spot.kind == STRIP_HOLE) != (s_truth[k] == 0);
        }
        return wrong;
    }
    StripWalk w;
    Strip     s;
    uint64_t  k = 0;
    StripWalkBegin(rec, &w);
    while (StripWalkNext(&w, &s))
        for (uint32_t j = 0; j < s.count; j++, k++)
            wrong += k >= FILE_BLOCKS || s_truth[k] != (s.kind == STRIP_HOLE ? 0 : s.start + j);
    return wrong + (k != FILE_BLOCKS);
}

/* What one splice of one block may keep (above), the stand's own measure
 * (stand_fresh_ground_tests.c StandFreshBuiltMost): the Strips of one Parcel
 * and the two a write in the middle of a run adds, in two Parcels where that
 * one is split; the record's head; and a node of the Tract a level. */
static uint64_t SpliceBound(const FileRecord* rec)
{
    uint32_t height = rec->tract ? rec->tract->level + 1u : 0u;
    return ParcelBytes(PARCEL_STRIPS_MOST + 2u) + ParcelBytes(0) + (rec->bytes - (rec->tract ? rec->tract->bytes : 0)) +
           (uint64_t)height * (sizeof(Tract) + TRACT_KIDS_MOST * sizeof(TractKid));
}

/* The nodes of a Tract: one a level on the way to a Parcel is built again by
 * a splice, and a node past the most cut in two — the one more a level the
 * stand's measure does not count, which a file of 32 MiB never meets. */
static uint64_t TractNodes(const Tract* t)
{
    uint64_t n = 1;
    if (!t)
        return 0;
    for (uint32_t i = 0; t->level && i < t->count; i++)
        n += TractNodes((const Tract*)t->kid[i].node);
    return n;
}

typedef struct {
    uint64_t most_kept, most_bound, most_scratch, over, cut, cut_over;
} Spent;

/* One block at random written over on fresh ground: OK, the record replaced. */
static error_t Rewrite(FileRecord** rec, uint32_t i, Spent* spent)
{
    uint64_t       k     = Next() % FILE_BLOCKS;
    Strip          fresh = { FRESH_FROM + 2ull * i, 1, STRIP_DATA, 2u + i };
    BookSplice     sp    = { k, 1, 1, &fresh };
    FileRecord*    now   = NULL;
    uint64_t       bound = SpliceBound(*rec);
    uint64_t       nodes = TractNodes((*rec)->tract);
    ParcelHostHeap was   = g_parcel_host_heap;
    error_t        rc    = FileRecordSplice(*rec, &sp, 1, FILE_BLOCKS * 4096ull, 2u + i, &now, NULL, NULL);
    if (rc != OK)
        return rc;
    uint64_t kept    = g_parcel_host_heap.live - was.live;
    uint64_t scratch = g_parcel_host_heap.bytes - was.bytes - kept;
    spent->most_kept    = kept > spent->most_kept ? kept : spent->most_kept;
    spent->most_bound   = bound > spent->most_bound ? bound : spent->most_bound;
    spent->most_scratch = scratch > spent->most_scratch ? scratch : spent->most_scratch;
    uint64_t grown = TractNodes(now->tract) - nodes;
    spent->cut += grown > 0;
    spent->cut_over += grown > 0 && kept > bound;
    spent->over += kept > bound + grown * (sizeof(Tract) + TRACT_KIDS_MOST * sizeof(TractKid));
    FileRecordRelease(*rec);
    *rec       = now;
    s_truth[k] = fresh.start;
    return OK;
}

static void Built(const FileRecord* rec, uint32_t runs)
{
    uint64_t parcels;
    uint32_t least, most;
    char     line[256];
    ParcelsCounted(rec, &parcels, &least, &most);
    snprintf(line, sizeof(line), "1 GiB in %u runs built into %llu Parcels, %u..%u Strips each (the cut: <= %u, >= %u)",
             runs, (unsigned long long)parcels, least, most, PARCEL_STRIPS_MOST, PARCEL_STRIPS_LEAST);
    Say(rec->strip_count == runs && rec->blocks == FILE_BLOCKS && most <= PARCEL_STRIPS_MOST &&
        least >= PARCEL_STRIPS_LEAST, line);
}

int main(void)
{
    uint32_t runs;
    Spent    spent = { 0, 0, 0, 0, 0, 0 };
    char     line[320];
    s_truth = malloc(sizeof(uint64_t) * FILE_BLOCKS);
    Strip*        strips = FileRuns(&runs);
    TagFSMetadata meta   = { .file_id = 7, .flags = 1, .size = FILE_BLOCKS * 4096ull, .filename = "big",
                             .extents = strips, .extent_count = runs };
    FileRecord*   rec    = FileRecordBuild(&meta);
    free(strips);
    if (!rec) {
        printf("parcel_host: FAIL — the record of 1 GiB would not build\n");
        return 1;
    }
    printf("parcel_host: a file of 1 GiB, %u random rewrites of a block on fresh ground\n", REWRITES);
    Built(rec, runs);
    Say(Disagree(rec, 0) == 0, "every block of it lies where its run says");
    for (uint32_t i = 0; i < REWRITES; i++) {
        if (Rewrite(&rec, i, &spent) != OK) {
            Say(false, "a splice of one block was refused");
            break;
        }
        if ((i + 1u) % CHECK_EVERY == 0 && Disagree(rec, 2000) != 0)
            Say(false, "a block found by halving lies elsewhere than its last write put it");
    }
    snprintf(line, sizeof(line), "each rewrite kept at most %llu new bytes of the heap — one Parcel grown by two "
             "Strips, the head and a node a level: at most %llu, and a node more for each one a cut adds (%llu "
             "rewrite(s) cut a node, %llu of them past the stand's measure; %llu over); its scratch, let go "
             "before it returned, at most %llu", (unsigned long long)spent.most_kept,
             (unsigned long long)spent.most_bound, (unsigned long long)spent.cut,
             (unsigned long long)spent.cut_over, (unsigned long long)spent.over,
             (unsigned long long)spent.most_scratch);
    Say(spent.over == 0, line);
    Built(rec, (uint32_t)rec->strip_count);
    snprintf(line, sizeof(line), "after them every block of the file lies where its last write put it (%llu Strips, "
             "Tract %u level(s))", (unsigned long long)rec->strip_count, rec->tract ? rec->tract->level + 1u : 0u);
    Say(Disagree(rec, 0) == 0, line);
    FileRecordRelease(rec);
    Say(g_parcel_host_heap.allocs == g_parcel_host_heap.frees, "and the record let go, the heap holds nothing of it");
    free(s_truth);
    printf("parcel_host: %s\n", s_failed ? "FAIL" : "PASS");
    return s_failed ? 1 : 0;
}
