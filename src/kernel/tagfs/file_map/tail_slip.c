#include "tail_slip.h"
#include "../settle/binder.h"   /* BinderDemanded — the heap that refuses a Slip asks a Tally */

/*
 * TailSlip (tail_slip.h): the bytes the Slips lay over a file's last block,
 * and which of the block's bytes they cover — one bit each.
 */

struct TailSlip {
    volatile uint32_t refs;
    uint8_t           covered[TAGFS_BLOCK_SIZE / 8u];
    uint8_t           bytes[TAGFS_BLOCK_SIZE];
};

static bool TailCovered(const TailSlip* s, uint32_t i)
{
    return s && (s->covered[i / 8u] & (1u << (i % 8u)));
}

TailSlip* TailSlipLaid(const TailSlip* under, uint32_t off, const uint8_t* bytes, uint32_t len)
{
    TailSlip* s = kmalloc(sizeof(TailSlip));
    if (!s)
        return NULL;
    if (under)
        memcpy(s, under, sizeof(TailSlip));
    else
        memset(s, 0, sizeof(TailSlip));
    s->refs = 1;
    for (uint32_t i = off; i < off + len && i < TAGFS_BLOCK_SIZE; i++) {
        s->bytes[i]           = bytes[i - off];
        s->covered[i / 8u] |= (uint8_t)(1u << (i % 8u));
    }
    return s;
}

const TailSlip* TailSlipHold(const TailSlip* slip)
{
    if (slip)
        __atomic_add_fetch(&((TailSlip*)slip)->refs, 1, __ATOMIC_ACQ_REL);
    return slip;
}

void TailSlipLetGo(const TailSlip* slip)
{
    if (slip && __atomic_sub_fetch(&((TailSlip*)slip)->refs, 1, __ATOMIC_ACQ_REL) == 0)
        kfree((TailSlip*)slip);
}

bool TailSlipOn(const FileRecord* rec, uint64_t k)
{
    return rec && rec->slip && rec->size && k == (rec->size - 1u) / TAGFS_BLOCK_SIZE;
}

bool TailSlipOver(const FileRecord* rec, uint64_t k, uint8_t* block)
{
    if (!TailSlipOn(rec, k))
        return false;
    const TailSlip* s = rec->slip;
    for (uint32_t i = 0; i < TAGFS_BLOCK_SIZE; i++)
        if (TailCovered(s, i))
            block[i] = s->bytes[i];
    return true;
}

// ----------------------------------------------------------------------------
// The rule a record keeps its Slips by
// ----------------------------------------------------------------------------

/* The file's last block as a record leaves it — its index, and the block it
 * lies on (UINT64_MAX for a hole, one less for none of its runs) — or false
 * for a file of no bytes. */
static bool TailLastBlock(const FileRecord* rec, uint64_t* k, uint64_t* block)
{
    FileSpot spot;
    if (!rec->size)
        return false;
    *k = (rec->size - 1u) / TAGFS_BLOCK_SIZE;
    if (!FileRecordFind(rec, *k, &spot))
        *block = UINT64_MAX - 1u;
    else
        *block = spot.kind == STRIP_HOLE ? UINT64_MAX : spot.block;
    return true;
}

/* The Slips of `old` a reader still lays on `rec`'s last block: none when a
 * splice changed which block that is (§16.3 п.7), and none of a file of no
 * bytes. */
static const TailSlip* TailUnder(const FileRecord* old, const FileRecord* rec, bool spliced, uint64_t* k,
                                 uint64_t* block)
{
    uint64_t ok = 0, ob = 0;
    if (!old || !old->slip || !TailLastBlock(rec, k, block))
        return NULL;
    if (!TailLastBlock(old, &ok, &ob) || (spliced && (ok != *k || ob != *block)))
        return NULL;
    return old->slip;
}

/* The file's bytes in its last block: 1..4096. */
static uint32_t TailCut(const FileRecord* rec, uint64_t k)
{
    return (uint32_t)(rec->size - k * TAGFS_BLOCK_SIZE);
}

error_t TailSlipKept(TagFSVolume* vol, const FileRecord* old, FileRecord* rec, bool spliced)
{
    uint64_t        k = 0, block = 0;
    const TailSlip* under = TailUnder(old, rec, spliced, &k, &block);
    uint32_t        cut = under ? TailCut(rec, k) : 0, past = 0;
    rec->slip = NULL;
    for (uint32_t i = cut; under && i < TAGFS_BLOCK_SIZE; i++)
        past += TailCovered(under, i);
    if (!under || past == 0) {
        rec->slip = TailSlipHold(under);
        return OK;
    }
    uint32_t lo = 0;
    if (TailSlipHull(under, &lo) == 0 || lo >= cut)
        return OK;                       /* the cut takes every byte the Slips laid */
    TailSlip* s = TailSlipLaid(under, 0, NULL, 0);
    if (!s) {
        /* §16.5 п.15: the event asks the Tally, which lays every tail in a
         * block and lets the memory of their Slips go. */
        kprintf("[TagFS] file %llu: the heap has no room to cut its Slips to its size of %llu — the change is "
                "refused, and a Tally is asked\n", (unsigned long long)rec->file_id, (unsigned long long)rec->size);
        BinderDemanded(vol);
        return ERR_NO_MEMORY;
    }
    for (uint32_t i = cut; i < TAGFS_BLOCK_SIZE; i++)
        s->covered[i / 8u] &= (uint8_t)~(1u << (i % 8u));
    rec->slip = s;
    return OK;
}

// ----------------------------------------------------------------------------
// The shape a writer keeps, and a write's own Slip
// ----------------------------------------------------------------------------

uint32_t TailSlipHull(const TailSlip* slip, uint32_t* lo)
{
    int64_t first = -1, last = -1;
    *lo = 0;
    for (uint32_t i = 0; slip && i < TAGFS_BLOCK_SIZE; i++)
        if (TailCovered(slip, i)) {
            first = first < 0 ? (int64_t)i : first;
            last  = i;
        }
    if (first < 0)
        return 0;
    *lo = (uint32_t)first;
    return (uint32_t)(last - first + 1);
}

bool TailSlipJoins(const TailSlip* slip, uint32_t off, uint32_t len)
{
    uint32_t lo = 0, n = TailSlipHull(slip, &lo);
    if (n == 0)
        return true;
    for (uint32_t i = lo; i < lo + n; i++)
        if (!TailCovered(slip, i))
            return false;                /* the Slips themselves lie in two spans */
    return off <= lo + n && off + len >= lo;
}

TailSlip* TailSlipBlank(void)
{
    return TailSlipLaid(NULL, 0, NULL, 0);
}

uint8_t* TailSlipBytes(TailSlip* slip)
{
    return slip->bytes;
}

void TailSlipCovers(TailSlip* slip, uint32_t off, uint32_t len)
{
    for (uint32_t i = off; i < off + len && i < TAGFS_BLOCK_SIZE; i++)
        slip->covered[i / 8u] |= (uint8_t)(1u << (i % 8u));
}

void TailSlipFold(TailSlip* slip, uint32_t off, uint32_t len, const TailSlip* under)
{
    for (uint32_t i = 0; i < TAGFS_BLOCK_SIZE; i++) {
        if (i >= off && i < off + len)
            continue;
        bool c = TailCovered(under, i);
        slip->bytes[i] = c ? under->bytes[i] : 0;
        if (c)
            slip->covered[i / 8u] |= (uint8_t)(1u << (i % 8u));
        else
            slip->covered[i / 8u] &= (uint8_t)~(1u << (i % 8u));
    }
}

// ----------------------------------------------------------------------------
// The change a stitch lays
// ----------------------------------------------------------------------------

/* Whether byte i of the tail reads otherwise under `now`'s Slips than under
 * `was`'s: covered by one alone, or by both with two bytes. */
static bool TailDiffers(const TailSlip* now, const TailSlip* was, uint32_t i)
{
    bool a = TailCovered(now, i), b = TailCovered(was, i);
    return a != b || (a && now->bytes[i] != was->bytes[i]);
}

/* A byte inside the span the Slips leave bare over a DATA block: the writer's
 * shape broken (TailSlipJoins) — the stitch would lay a zero the block does not
 * hold. */
static void TailDeltaBare(const FileRecord* now, uint32_t i)
{
#if CONFIG_STRICT
    panic("[TagFS] DEFECT: file %llu: byte %u of its tail lies inside the span its stitch lays and no Slip covers "
          "it, over a DATA block", (unsigned long long)now->file_id, i);
#endif
    kprintf("[TagFS] DEFECT: file %llu: byte %u of its tail lies inside the span its stitch lays and no Slip "
            "covers it, over a DATA block — the stitch lays a zero there\n", (unsigned long long)now->file_id, i);
}

uint32_t TailSlipDelta(const FileRecord* was, const FileRecord* now, bool spliced, uint32_t* off, uint8_t* out)
{
    uint64_t        k = 0, block = 0;
    int64_t         lo = -1, hi = -1;
    const TailSlip* under = NULL;
    *off = 0;
    if (!now || !TailLastBlock(now, &k, &block))
        return 0;
    under        = TailUnder(was, now, spliced, &k, &block);
    uint32_t cut = TailCut(now, k);
    for (uint32_t i = 0; i < cut; i++)
        if (TailDiffers(now->slip, under, i)) {
            lo = lo < 0 ? (int64_t)i : lo;
            hi = (int64_t)i + 1;
        }
    if (lo < 0)
        return 0;
    for (uint32_t i = (uint32_t)lo; out && i < (uint32_t)hi; i++) {
        bool c = TailCovered(now->slip, i);
        out[i - lo] = c ? now->slip->bytes[i] : 0;
        if (!c && block != UINT64_MAX)
            TailDeltaBare(now, i);
    }
    *off = (uint32_t)lo;
    return (uint32_t)(hi - lo);
}

uint32_t TailSlipClaim(const FileRecord* cur, const FileRecord* rec, bool rides)
{
    uint64_t k = 0, block = 0;
    uint32_t lo = 0, hi = 0, slo = 0;
    if (!rec || !TailLastBlock(rec, &k, &block))
        return 0;
    uint64_t from = k * TAGFS_BLOCK_SIZE;
    uint64_t had  = cur && cur->size > from ? cur->size : from;
    if (block == UINT64_MAX && rec->size > had) {
        lo = (uint32_t)(had - from);
        hi = (uint32_t)(rec->size - from);
    }
    uint32_t span = rides ? TailSlipHull(rec->slip, &slo) : 0u;
    if (span) {
        lo = hi > lo && lo < slo ? lo : slo;
        hi = hi > slo + span ? hi : slo + span;
    }
    return hi - lo;
}
