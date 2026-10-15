#include "gloss.h"

/*
 * Gloss (gloss.h): (block, digest, order) noted in the replay's order, then
 * sorted by block and order, and the latest of each block kept — a binary
 * search answers a read.
 */

typedef struct {
    uint64_t block;
    uint64_t digest;
    uint64_t order;
} GlossEntry;

static GlossEntry* s_gloss;
static uint64_t    s_count;
static uint64_t    s_cap;

error_t GlossNote(uint64_t block, uint64_t digest)
{
    if (s_count == s_cap) {
        uint64_t    cap  = s_cap ? s_cap * 2u : 64u;
        GlossEntry* more = kmalloc(sizeof(GlossEntry) * cap);
        if (!more)
            return ERR_NO_MEMORY;
        if (s_count)
            memcpy(more, s_gloss, sizeof(GlossEntry) * s_count);
        kfree(s_gloss);
        s_gloss = more;
        s_cap   = cap;
    }
    s_gloss[s_count] = (GlossEntry){ block, digest, s_count };
    s_count++;
    return OK;
}

static int GlossOrder(const void* a, const void* b, void* ctx)
{
    const GlossEntry* x = a;
    const GlossEntry* y = b;
    (void)ctx;
    if (x->block != y->block)
        return x->block < y->block ? -1 : 1;
    return x->order < y->order ? -1 : x->order > y->order;
}

void GlossSealed(void)
{
    uint64_t kept = 0;
    if (s_count)
        KSort(s_gloss, s_count, sizeof(GlossEntry), GlossOrder, NULL);
    for (uint64_t i = 0; i < s_count; i++) {
        if (kept && s_gloss[kept - 1u].block == s_gloss[i].block)
            s_gloss[kept - 1u] = s_gloss[i];
        else
            s_gloss[kept++] = s_gloss[i];
    }
    s_count = kept;
}

bool GlossFind(uint64_t block, uint64_t* digest)
{
    uint64_t lo = 0, hi = s_count;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2u;
        if (s_gloss[mid].block == block) {
            *digest = s_gloss[mid].digest;
            return true;
        }
        if (s_gloss[mid].block < block)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return false;
}

void GlossLetGo(void)
{
    kfree(s_gloss);
    s_gloss = NULL;
    s_count = s_cap = 0;
}
