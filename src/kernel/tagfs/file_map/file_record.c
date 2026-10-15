#include "tract.h"
#include "tail_slip.h"

/*
 * A file's record (file_record.h): its header — one allocation with its tags
 * and its name — and its runs, a Tract of Parcels (tract.c), shared by every
 * record that names them.
 */

/* A header of the record's own, its tags and its name copied in after it,
 * naming no runs yet: NULL for no memory. */
static FileRecord* RecordHeader(const char* name, const uint32_t* tags, uint32_t tag_count)
{
    uint32_t    name_len = name ? (uint32_t)strlen(name) : 0;
    size_t      bytes    = sizeof(FileRecord) + (size_t)tag_count * sizeof(uint32_t) + name_len + 1u;
    FileRecord* rec      = kmalloc(bytes);
    if (!rec)
        return NULL;

    uint8_t* at = (uint8_t*)(rec + 1);
    memset(rec, 0, sizeof(*rec));
    rec->refs      = 1;
    rec->bytes     = bytes;
    rec->tag_count = tag_count;
    rec->name_len  = name_len;
    rec->tags      = (const uint32_t*)at;
    if (tag_count)
        memcpy(at, tags, (size_t)tag_count * sizeof(uint32_t));
    at += (size_t)tag_count * sizeof(uint32_t);
    rec->name = (const char*)at;
    if (name_len)
        memcpy(at, name, name_len);
    at[name_len] = '\0';
    return rec;
}

/* `tract` — a reference the record takes over — as the record's runs. */
static void RecordRuns(FileRecord* rec, const Tract* tract)
{
    rec->tract       = tract;
    rec->strip_count = tract ? tract->strips : 0;
    rec->blocks      = TractBlocks(tract);
    rec->bytes      += tract ? tract->bytes : 0;
}

FileRecord* FileRecordBuild(const TagFSMetadata* meta)
{
    const Tract* tract = NULL;
    error_t      rc    = TractOfStrips(meta->extents, meta->extents ? meta->extent_count : 0, &tract);
    if (rc == ERR_INVALID_ARGUMENT) {
#if CONFIG_STRICT
        panic("[TagFS] DEFECT: file %llu's record names a run of no blocks, or of a kind memory "
              "does not know", meta->file_id);
#endif
        kprintf("[TagFS] DEFECT: file %llu's record names a run of no blocks, or of a kind memory "
                "does not know — it is not built\n", meta->file_id);
    }
    if (rc != OK)
        return NULL;

    FileRecord* rec = RecordHeader(meta->filename, meta->tag_ids,
                                   meta->tag_ids ? meta->tag_count : 0);
    if (!rec) {
        TractLetGo(tract);
        return NULL;
    }
    rec->file_id       = meta->file_id;
    rec->flags         = meta->flags;
    rec->size          = meta->size;
    rec->created_time  = meta->created_time;
    rec->modified_time = meta->modified_time;
    RecordRuns(rec, tract);
    return rec;
}

FileRecord* FileRecordRelabel(const FileRecord* old, const char* name, const uint32_t* tags,
                              uint32_t tag_count, uint32_t flags)
{
    FileRecord* rec = RecordHeader(name, tags, tag_count);
    if (!rec)
        return NULL;
    rec->file_id       = old->file_id;
    rec->flags         = flags;
    rec->size          = old->size;
    rec->created_time  = old->created_time;
    rec->modified_time = old->modified_time;
    rec->gen           = __atomic_load_n(&old->gen, __ATOMIC_RELAXED);
    rec->slip          = TailSlipHold(old->slip);
    TractHold(old->tract);
    RecordRuns(rec, old->tract);
    return rec;
}

// ----------------------------------------------------------------------------
// Spliced
// ----------------------------------------------------------------------------

/* Every splice begins at or past where the one before it ended, counted in
 * the file the one before left: OK, or ERR_INVALID_ARGUMENT, said. */
static error_t SplicesInOrder(const FileRecord* old, const BookSplice* sp, uint64_t count)
{
    uint64_t end = 0;
    for (uint64_t i = 0; i < count; i++) {
        uint64_t put = 0;
        for (uint64_t k = 0; k < sp[i].ninsert; k++)
            put += sp[i].insert[k].count;
        if (sp[i].at < end || sp[i].at > UINT64_MAX - put) {
            kprintf("[TagFS] file %llu: splice %llu of %llu begins at block %llu, before the end "
                    "of the one before it (%llu) — the change is refused\n", old->file_id, i + 1u,
                    count, (unsigned long long)sp[i].at, (unsigned long long)end);
            return ERR_INVALID_ARGUMENT;
        }
        end = sp[i].at + put;
    }
    return OK;
}

static int GroundByStart(const void* a, const void* b, void* ctx)
{
    (void)ctx;
    uint64_t x = ((const Strip*)a)->start, y = ((const Strip*)b)->start;
    return x < y ? -1 : x > y;
}

uint64_t StripsAsGround(Strip* s, uint64_t count)
{
    uint64_t n = 0;
    if (count)
        KSort(s, count, sizeof(Strip), GroundByStart, NULL);
    for (uint64_t i = 0; i < count; i++) {
        Strip* last = n ? &s[n - 1u] : NULL;
        if (last && last->start + last->count == s[i].start && last->count <= UINT32_MAX - s[i].count)
            last->count += s[i].count;
        else
            s[n++] = (Strip){ .start = s[i].start, .count = s[i].count, .kind = STRIP_DATA };
    }
    return n;
}

/* The ground `took` names that `gave` does not, as DATA Strips in a new array:
 * what the splices freed. Both lists are put in order of their first block. */
static error_t GroundLess(TractGround* took, TractGround* gave, Strip** out, uint64_t* nout)
{
    *out  = NULL;
    *nout = 0;
    if (took->count == 0)
        return OK;
    Strip* freed = kmalloc(sizeof(Strip) * (took->count + gave->count));
    if (!freed)
        return ERR_NO_MEMORY;
    KSort(took->strip, took->count, sizeof(Strip), GroundByStart, NULL);
    if (gave->count)
        KSort(gave->strip, gave->count, sizeof(Strip), GroundByStart, NULL);

    /* Each piece taken out, less every piece put in that overlaps it: the
     * gaps between them, and what is left past the last. A piece put in
     * cuts at most one gap before itself, so the freed are no more than the
     * two lists together. */
    uint64_t n = 0, first = 0;
    for (uint64_t t = 0; t < took->count; t++) {
        uint64_t at = took->strip[t].start, end = at + took->strip[t].count;
        while (first < gave->count && gave->strip[first].start + gave->strip[first].count <= at)
            first++;
        for (uint64_t g = first; g < gave->count && gave->strip[g].start < end && at < end; g++) {
            uint64_t from = gave->strip[g].start, to = from + gave->strip[g].count;
            if (from > at)
                freed[n++] = (Strip){ .start = at, .count = (uint32_t)(from - at), .kind = STRIP_DATA };
            if (to > at)
                at = to;
        }
        if (at < end)
            freed[n++] = (Strip){ .start = at, .count = (uint32_t)(end - at), .kind = STRIP_DATA };
    }
    if (n == 0)
        kfree(freed);
    else
        *out = freed;
    *nout = n;
    return OK;
}

/* The DATA Strips of a splice's insert, added to `gave`. */
static error_t GroundOfInsert(TractGround* gave, const BookSplice* sp)
{
    for (uint64_t k = 0; k < sp->ninsert; k++)
        if (sp->insert[k].kind == STRIP_DATA && TractGroundAdd(gave, &sp->insert[k]) != OK)
            return ERR_NO_MEMORY;
    return OK;
}

/* The splices applied one after another, each to the Tract the one before
 * built; the ground they took out and put in gathered when `took` is asked. */
static error_t SplicesApplied(const FileRecord* old, const BookSplice* sp, uint64_t count,
                              TractGround* took, TractGround* gave, const Tract** out)
{
    const Tract* root = old->tract;
    error_t      rc   = OK;
    TractHold(root);
    for (uint64_t i = 0; i < count && rc == OK; i++) {
        const Tract* next = NULL;
        rc = TractSplice(root, &sp[i], &next, took);
        if (rc == ERR_INVALID_ARGUMENT)
            kprintf("[TagFS] file %llu: splice %llu of %llu — %llu block(s) out at %llu, %llu "
                    "Strip(s) in — does not fit its %llu block(s), or puts in a Strip of no block "
                    "— the change is refused\n", old->file_id, i + 1u, count,
                    (unsigned long long)sp[i].remove, (unsigned long long)sp[i].at,
                    (unsigned long long)sp[i].ninsert, (unsigned long long)TractBlocks(root));
        if (rc != OK)
            break;
        TractLetGo(root);
        root = next;
        if (took)
            rc = GroundOfInsert(gave, &sp[i]);
    }
    if (rc != OK) {
        TractLetGo(root);
        root = NULL;
    }
    *out = root;
    return rc;
}

error_t FileRecordSplice(const FileRecord* old, const BookSplice* sp, uint64_t count,
                         uint64_t size, uint64_t mtime, FileRecord** out,
                         Strip** freed, uint64_t* nfreed)
{
    TractGround  took = { 0 }, gave = { 0 };
    const Tract* root = NULL;
    FileRecord*  rec  = NULL;

    *out = NULL;
    if (freed) {
        *freed  = NULL;
        *nfreed = 0;
    }
    error_t rc = SplicesInOrder(old, sp, count);
    if (rc == OK)
        rc = SplicesApplied(old, sp, count, freed ? &took : NULL, &gave, &root);
    if (rc == OK && !(rec = RecordHeader(old->name, old->tags, old->tag_count)))
        rc = ERR_NO_MEMORY;
    if (rc == OK && freed)
        rc = GroundLess(&took, &gave, freed, nfreed);
    kfree(took.strip);
    kfree(gave.strip);
    if (rc != OK) {
        TractLetGo(root);
        kfree(rec);
        return rc;
    }
    rec->file_id       = old->file_id;
    rec->flags         = old->flags;
    rec->size          = size;
    rec->created_time  = old->created_time;
    rec->modified_time = mtime;
    rec->gen           = old->gen;
    rec->slip          = TailSlipHold(old->slip);
    RecordRuns(rec, root);
    *out = rec;
    return OK;
}

// ----------------------------------------------------------------------------
// Found and walked
// ----------------------------------------------------------------------------

bool FileRecordFind(const FileRecord* rec, uint64_t k, FileSpot* out)
{
    const Parcel* p     = NULL;
    uint64_t      at    = 0;
    uint64_t      steps = 0;
    bool          found = TractFind(rec->tract, k, &p, &at, &steps);
    if (found) {
        const uint64_t* end = ParcelEnds(p);
        uint32_t        lo = 0, hi = p->count;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2u;
            steps++;
            if (end[mid] > at)
                hi = mid;
            else
                lo = mid + 1u;
        }
        const Strip* s   = &p->strip[lo];
        uint64_t     off = at - (lo ? end[lo - 1u] : 0);
        out->kind  = s->kind;
        out->left  = (uint32_t)(s->count - off - 1u);
        out->block = s->kind == STRIP_DATA ? s->start + off : 0;
    }
#ifdef CONFIG_VOLUME_TESTS
    __atomic_add_fetch(&((FileRecord*)rec)->find_steps, steps, __ATOMIC_RELAXED);
#endif
    return found;
}

/* The walk down from node `t`, by first kids, to a leaf. */
static void WalkDown(StripWalk* w, const Tract* t)
{
    for (;;) {
        w->node[w->depth] = t;
        w->slot[w->depth] = 0;
        w->depth++;
        if (t->level == 0)
            return;
        t = t->kid[0].node;
    }
}

void StripWalkBegin(const FileRecord* rec, StripWalk* w)
{
    w->depth  = 0;
    w->parcel = NULL;
    w->strip  = 0;
    if (rec->tract)
        WalkDown(w, rec->tract);
}

bool StripWalkAt(const FileRecord* rec, uint64_t k, StripWalk* w, uint64_t* begins)
{
    return TractWalkAt(rec->tract, k, w, begins);
}

bool StripWalkNext(StripWalk* w, Strip* out)
{
    while (w->depth) {
        const Tract* leaf = w->node[w->depth - 1u];
        uint32_t*    slot = &w->slot[w->depth - 1u];
        if (*slot < leaf->count) {
            const Parcel* p = leaf->kid[*slot].node;
            if (w->strip < p->count) {
                w->parcel = p;
                *out = p->strip[w->strip++];
                return true;
            }
            (*slot)++;
            w->strip = 0;
            continue;
        }
        /* The leaf is walked: up to the first node with a kid left, and down
         * that kid's first kids. */
        w->depth--;
        while (w->depth && ++w->slot[w->depth - 1u] >= w->node[w->depth - 1u]->count)
            w->depth--;
        if (w->depth)
            WalkDown(w, w->node[w->depth - 1u]->kid[w->slot[w->depth - 1u]].node);
    }
    return false;
}

bool StripWalkNextGround(StripWalk* w, Strip* out)
{
    while (StripWalkNext(w, out))
        if (out->kind == STRIP_DATA)
            return true;
    return false;
}

error_t FileRecordFlatten(const FileRecord* rec, Strip** out, uint32_t* count)
{
    StripWalk w;
    uint32_t  n = 0;
    *out   = NULL;
    *count = 0;
    if (rec->strip_count == 0)
        return OK;
    if (rec->strip_count > UINT32_MAX) {
        kprintf("[TagFS] file %llu: its %llu runs are more than one flat list counts — it is not "
                "laid flat\n", rec->file_id, (unsigned long long)rec->strip_count);
        return ERR_NO_MEMORY;
    }
    Strip* flat = kmalloc(sizeof(Strip) * rec->strip_count);
    if (!flat)
        return ERR_NO_MEMORY;
    StripWalkBegin(rec, &w);
    while (n < rec->strip_count && StripWalkNext(&w, &flat[n]))
        n++;
    *out   = flat;
    *count = n;
    return OK;
}

void FileRecordRelease(FileRecord* rec)
{
    if (rec && __atomic_sub_fetch(&rec->refs, 1, __ATOMIC_ACQ_REL) == 0) {
        TractLetGo(rec->tract);
        TailSlipLetGo(rec->slip);
        kfree(rec);
    }
}

int FileRecordCopyOut(const FileRecord* rec, TagFSMetadata* out)
{
    memset(out, 0, sizeof(*out));
    out->file_id       = rec->file_id;
    out->flags         = rec->flags;
    out->size          = rec->size;
    out->created_time  = rec->created_time;
    out->modified_time = rec->modified_time;

    if (rec->tag_count) {
        out->tag_ids = kmalloc(sizeof(uint32_t) * rec->tag_count);
        if (!out->tag_ids)
            goto refused;
        memcpy(out->tag_ids, rec->tags, sizeof(uint32_t) * rec->tag_count);
        out->tag_count = rec->tag_count;
    }
    out->filename = kmalloc(rec->name_len + 1u);
    if (!out->filename)
        goto refused;
    memcpy(out->filename, rec->name, rec->name_len + 1u);
    return 0;

refused:
    kfree(out->tag_ids);
    memset(out, 0, sizeof(*out));
    return -1;
}
