#include "dedup_internal.h"

/*
 * Commonplace (dedup_internal.h): the blocks written since a Tally, one array
 * sorted by digest and then block — a digest's entries found in log n, the
 * walk's question for every block a leaf of DIGESTS names, and those of one
 * digest side by side, the pass's question of them among themselves. Never
 * written to the medium: no tree of it lies there. The service's alone; built
 * a Tally's blocks at a time, the array made anew at each take, so it holds
 * what it holds and no more.
 */

static CommonplaceEntry* s_entry;
static uint64_t          s_n;

static int EntryOrder(const void* a, const void* b, void* ctx)
{
    const CommonplaceEntry* x = a;
    const CommonplaceEntry* y = b;
    (void)ctx;
    if (x->digest != y->digest)
        return x->digest < y->digest ? -1 : 1;
    return x->block < y->block ? -1 : x->block > y->block;
}

/* Whether `block` lies in one of the `n` runs, in order. */
static bool GroundHolds(const BinderGround* g, uint64_t n, uint64_t block)
{
    uint64_t lo = 0, hi = n;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2u;
        if (g[mid].start + g[mid].count <= block)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo < n && g[lo].start <= block;
}

/* Whether `block` is one of the `n` entries, in order of their blocks. */
static bool InHolds(const CommonplaceEntry* in, uint64_t n, uint64_t block)
{
    uint64_t lo = 0, hi = n;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2u;
        if (in[mid].block < block)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo < n && in[lo].block == block;
}

/* The first entry whose digest is not below `digest`. */
static uint64_t DigestFrom(uint64_t digest)
{
    uint64_t lo = 0, hi = s_n;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2u;
        if (s_entry[mid].digest < digest)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo;
}

/*
 * ‼ A BLOCK LET GO AND TAKEN AGAIN HOLDS NEW BYTES (dog_ear.h). An entry on
 * ground the files let go since the Tally before is gone — nothing is laid on
 * a block no file names — and one on a block that comes in again is the
 * block's old digest: the new entry stands in its place.
 */
error_t CommonplaceTake(CommonplaceEntry* in, uint64_t n, const BinderGround* gone, uint64_t ngone,
                        uint64_t since)
{
    uint64_t live = 0, k = 0;
    for (uint64_t i = 0; i < s_n; i++) {
        CommonplaceEntry* e = &s_entry[i];
        if (GroundHolds(gone, ngone, e->block) || InHolds(in, n, e->block))
            e->mark |= COMMONPLACE_GONE;
        live += !(e->mark & COMMONPLACE_GONE);
    }
    if (n > UINT64_MAX / sizeof(CommonplaceEntry) - live)
        return ERR_NO_MEMORY;
    CommonplaceEntry* all = live + n ? kmalloc(sizeof(CommonplaceEntry) * (live + n)) : NULL;
    if (live + n && !all)
        return ERR_NO_MEMORY;
    for (uint64_t j = 0; j < n; j++) {
        in[j].since = since;
        in[j].fresh = 1u;
        in[j].mark  = 0;
    }
    KSort(in, n, sizeof(CommonplaceEntry), EntryOrder, NULL);
    for (uint64_t i = 0, j = 0; i < s_n || j < n;) {
        if (i < s_n && (s_entry[i].mark & COMMONPLACE_GONE)) {
            i++;
            continue;
        }
        bool mine = j == n || (i < s_n && EntryOrder(&s_entry[i], &in[j], NULL) < 0);
        all[k++]  = mine ? s_entry[i++] : in[j++];
    }
    kfree(s_entry);
    s_entry = all;
    s_n     = k;
    return OK;
}

CommonplaceEntry* CommonplaceGroup(uint64_t digest, uint64_t* count)
{
    uint64_t lo = DigestFrom(digest), hi = lo;
    while (hi < s_n && s_entry[hi].digest == digest)
        hi++;
    *count = hi - lo;
    return hi > lo ? &s_entry[lo] : NULL;
}

CommonplaceEntry* CommonplaceAt(uint64_t i)
{
    return &s_entry[i];
}

uint64_t CommonplaceCount(void)
{
    return s_n;
}

void CommonplaceStrike(uint64_t digest, uint64_t block)
{
    for (uint64_t i = DigestFrom(digest); i < s_n && s_entry[i].digest == digest; i++)
        if (s_entry[i].block == block)
            s_entry[i].mark |= COMMONPLACE_GONE;
}

void CommonplaceKeep(uint64_t digest, uint64_t block, uint64_t since)
{
    for (uint64_t i = DigestFrom(digest); i < s_n && s_entry[i].digest == digest; i++)
        if (s_entry[i].block == block)
            s_entry[i].since = since;
}

void CommonplaceRetire(uint64_t walked, uint64_t lap)
{
    uint64_t k = 0;
    for (uint64_t i = 0; i < s_n; i++) {
        CommonplaceEntry e = s_entry[i];
        if ((e.mark & COMMONPLACE_GONE) || walked - e.since >= lap)
            continue;
        e.fresh      = 0;
        e.mark       = 0;
        s_entry[k++] = e;
    }
    s_n = k;
    if (k == 0) {
        kfree(s_entry);
        s_entry = NULL;
    }
}

void CommonplaceLetGo(void)
{
    kfree(s_entry);
    s_entry = NULL;
    s_n     = 0;
}
