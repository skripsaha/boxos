#include "binder_internal.h"
#include "../mount/book_mount.h"
#include "../tag_registry/tag_registry.h"
#include "../integrity/gloss.h"

/*
 * The elements of a whole Tally's trees, as they come (binder_nodes.c): each
 * tree's leaves in key order out of memory — a file's header and its Parcels,
 * the tags, the Use Context, the digests of the ground DIGESTS speaks for —
 * and each inner level's, a node of the level below or a rotten node of it.
 * Its own file beside binder_nodes.c, which packs them into nodes and lays
 * them.
 */

/* A run in the Tally's form with no extension, at the most: its head, its
 * start's distance and its birth_delta, each a varint at its widest. */
#define TALLY_RUN_PUT_MOST  30u

// ----------------------------------------------------------------------------
// Keys
// ----------------------------------------------------------------------------

bool KeyLess(uint64_t tree, TallyKey a, TallyKey b)
{
    if (a.k1 != b.k1)
        return a.k1 < b.k1;
    return TallyPairKeyed(tree) && a.k2 < b.k2;
}

/* The key a rotten node is named by in its parent: its lower separator — the
 * least key there is for a tree's first node, which has none of its own. */
static TallyKey RotKey(const BinderRot* r)
{
    return r->at.has_lo ? r->at.lo : (TallyKey){ 0, 0 };
}

/* Whether element key `k` lies inside the range of rotten node `r`: from its
 * lower separator (or the start), up to its upper one — or, with none, at its
 * lower separator alone, where no element may stand beside it. */
bool RotCovers(const BinderRot* r, TallyKey k)
{
    if (r->at.has_lo && KeyLess(r->tree, k, r->at.lo))
        return false;
    if (r->at.has_hi)
        return KeyLess(r->tree, k, r->at.hi);
    return r->at.has_lo && !KeyLess(r->tree, r->at.lo, k);
}

// ----------------------------------------------------------------------------
// The elements of a tree, as they come
// ----------------------------------------------------------------------------

/* Room for an element's body of `bytes`: false with no memory. */
static bool ItemRoom(BinderItems* p, uint64_t bytes)
{
    if (bytes <= p->room)
        return true;
    if (bytes > UINT32_MAX)
        return false;
    uint8_t* more = kmalloc(bytes);
    if (!more)
        return false;
    kfree(p->buf);
    p->buf  = more;
    p->room = (uint32_t)bytes;
    return true;
}

/* The sorted ids of a record's tags, in `ids` — room for its tag_count. */
static void IdsSorted(const FileRecord* rec, uint32_t* ids)
{
    for (uint32_t i = 0; i < rec->tag_count; i++) {
        uint32_t id = rec->tags[i], j = i;
        while (j > 0 && ids[j - 1u] > id) {
            ids[j] = ids[j - 1u];
            j--;
        }
        ids[j] = id;
    }
}

/* The header of file `f`, (fid, 0), counting one file: gen · flags · size ·
 * created · mtime_z · name · tags, as TallyFileHeadPut lays one. */
static error_t FileHeadItem(BinderWork* w, const BinderFile* f, BinderItem* it)
{
    BinderItems*      p    = &w->items;
    const FileRecord* rec  = f->rec;
    uint64_t          need = 70u + rec->name_len + 10u * (uint64_t)rec->tag_count;
    uint64_t          ids  = (uint64_t)rec->tag_count * sizeof(uint32_t);
    if (!ItemRoom(p, need + ids))
        return ERR_NO_MEMORY;
    uint32_t* sorted = (uint32_t*)(p->buf + need);
    IdsSorted(rec, sorted);
    uint8_t* b  = p->buf;
    uint32_t at = BookVarintPut(b, rec->gen);
    at += BookVarintPut(b + at, rec->flags);
    at += BookVarintPut(b + at, rec->size);
    at += BookVarintPut(b + at, rec->created_time);
    at += BookVarintPut(b + at, BookZigzag((int64_t)(rec->modified_time - rec->created_time)));
    at += BookVarintPut(b + at, rec->name_len);
    at += TallyBytesPut(b + at, (const uint8_t*)rec->name, rec->name_len);
    at += BookVarintPut(b + at, rec->tag_count);
    at += TallyIdsPut(b + at, sorted, rec->tag_count);
    *it = (BinderItem){ .key = { f->fid, 0 }, .count = 1, .body = b, .len = at };
    return OK;
}

/* A run's birth: its own, or — born of this mount's writing — the seq of the
 * stitch that put it in (births.c). 0 when nobody knows it. */
static uint64_t RunBirth(const Strip* s)
{
    return s->birth ? s->birth : BirthsOf(s->start);
}

/* The Parcel `strips` of file `f`, (fid, 1 + b) — b the file block it begins
 * at — counting none: its runs in the Tally's form, born where they were. */
static error_t ParcelItem(BinderWork* w, const BinderFile* f, const Strip* strips, uint32_t n, BinderItem* it)
{
    BinderItems* p = &w->items;
    uint64_t     cursor = 0, blocks = 0;
    if (!ItemRoom(p, 10u + (uint64_t)n * TALLY_RUN_PUT_MOST))
        return ERR_NO_MEMORY;
    uint32_t at = BookVarintPut(p->buf, n);
    for (uint32_t i = 0; i < n; i++) {
        TallyRun r = { .start = strips[i].start, .count = strips[i].count, .birth = 0,
                       .hole = strips[i].kind == STRIP_HOLE, .ext_len = 0, .ext = NULL };
        if (!r.hole && (r.birth = RunBirth(&strips[i])) == 0) {
            kprintf("[TagFS] DEFECT: the run of fid %llu at block %llu (%llu blocks) has no birth this mount "
                    "knows — no Tally is written over a birth it would make up\n", (unsigned long long)f->fid,
                    (unsigned long long)r.start, (unsigned long long)r.count);
            return ERR_TAGFS_CORRUPTED;
        }
        at += TallyRunPut(p->buf + at, &r, &cursor, w->seq);
        blocks += r.count;
    }
    *it = (BinderItem){ .key = { f->fid, 1u + p->block }, .count = 0, .body = p->buf, .len = at };
    p->block += blocks;
    return OK;
}

/* The header of a file locked by name, as it read (BookKeptFile). */
static error_t KeptHeadItem(BinderWork* w, const BookKeptFile* k, BinderItem* it)
{
    BinderItems*         p   = &w->items;
    const TagFSMetadata* m   = &k->meta;
    uint64_t             len = m->filename ? strlen(m->filename) : 0;
    if (!ItemRoom(p, 70u + len + 10u * (uint64_t)m->tag_count))
        return ERR_NO_MEMORY;
    uint8_t* b  = p->buf;
    uint32_t at = BookVarintPut(b, k->gen);
    at += BookVarintPut(b + at, m->flags);
    at += BookVarintPut(b + at, m->size);
    at += BookVarintPut(b + at, m->created_time);
    at += BookVarintPut(b + at, BookZigzag((int64_t)(m->modified_time - m->created_time)));
    at += BookVarintPut(b + at, len);
    at += TallyBytesPut(b + at, (const uint8_t*)m->filename, len);
    at += BookVarintPut(b + at, m->tag_count);
    at += TallyIdsPut(b + at, m->tag_ids, m->tag_count);
    *it = (BinderItem){ .key = { k->fid, 0 }, .count = 1, .body = b, .len = at };
    return OK;
}

/* Parcel `i` of a file locked by name, its runs read by the seq of the node it
 * lay in and named again by the Tally's — each with the birth it had. */
static error_t KeptParcelItem(BinderWork* w, const BookKeptFile* k, uint32_t i, BinderItem* it)
{
    BinderItems*   p    = &w->items;
    const uint8_t* body = k->parcel_body[i];
    uint32_t       len  = k->parcel_len[i], in = 0;
    uint64_t       n = 0, from = 0, to = 0;
    if (!ItemRoom(p, 10u + 2u * (uint64_t)len + 64u))
        return ERR_NO_MEMORY;
    TallyTake(body, len, &in, &n);
    uint32_t at = BookVarintPut(p->buf, n);
    for (uint64_t r = 0; r < n; r++) {
        TallyRun run;
        if (!TallyRunGet(body, len, &in, &from, k->parcel_seq[i], &run))
            return ERR_TAGFS_CORRUPTED;
        at += TallyRunPut(p->buf + at, &run, &to, w->seq);
    }
    *it = (BinderItem){ .key = { k->fid, 1u + k->parcel_b[i] }, .count = 0, .body = p->buf, .len = at };
    return OK;
}

/* The next item of the files locked by name, the one at p->kept: its header,
 * then its Parcels; *got false once it is done (the walk moves past it). */
static error_t KeptNext(BinderWork* w, BinderItem* it, bool* got)
{
    BinderItems*        p = &w->items;
    const BookKeptFile* k = &g_book.kept[p->kept];
    *got = true;
    if (!p->kept_head) {
        p->kept_head = true;
        if (k->head)
            return KeptHeadItem(w, k, it);
    }
    if (p->kept_parcel < k->nparcels)
        return KeptParcelItem(w, k, p->kept_parcel++, it);
    p->kept++;
    p->kept_head   = false;
    p->kept_parcel = 0;
    *got = false;
    return OK;
}

/* FILES: each file of the slice, its header and then its Parcels — and among
 * them, in the order of their ids, the files the mount locked by name, as
 * what of them read lay: a file locked stays locked across a Tally (§16.4
 * п.5), its ground held. */
static error_t FilesNext(BinderWork* w, BinderItem* it, bool* got)
{
    BinderItems* p = &w->items;
    *got = false;
    while (p->chunk || p->kept < g_book.nkept) {
        if (p->chunk && p->at == p->chunk->n) {
            p->chunk = p->chunk->next;
            p->at    = 0;
            p->head  = false;
            continue;
        }
        const BinderFile* f = p->chunk ? &p->chunk->f[p->at] : NULL;
        if (p->kept < g_book.nkept && !p->head && (!f || g_book.kept[p->kept].fid < f->fid)) {
            error_t rc = KeptNext(w, it, got);
            if (rc != OK || *got)
                return rc;
            continue;
        }
        if (!p->head) {
            p->head  = true;
            p->block = 0;
            StripWalkBegin(f->rec, &p->walk);
            *got = true;
            return FileHeadItem(w, f, it);
        }
        const Strip* strips = NULL;
        uint32_t     n      = 0;
        if (StripWalkParcel(&p->walk, &strips, &n)) {
            *got = true;
            return ParcelItem(w, f, strips, n, it);
        }
        p->at++;
        p->head = false;
    }
    return OK;
}

/* TAGS: every tag below the slice's mark, by id — the book's, Coined or not. */
static error_t TagsNext(BinderWork* w, BinderItem* it, bool* got)
{
    BinderItems* p = &w->items;
    TagSpelling  sp;
    uint32_t     id = 0;
    *got = TagRoll(tagfs_get_state()->registry, p->tag, w->tag_hi, &id, &sp);
    if (!*got)
        return OK;
    p->tag = id + 1u;
    if (!ItemRoom(p, 30u + sp.key_len + sp.value_len))
        return ERR_NO_MEMORY;
    EndpaperLaidTag(&w->laid, (uint64_t)sp.key_len + sp.value_len);
    TallyTag t = { .flags = sp.value ? TALLY_TAG_HAS_VALUE : 0u, .key = (const uint8_t*)sp.key,
                   .key_len = sp.key_len, .value = (const uint8_t*)sp.value, .value_len = sp.value_len };
    *it = (BinderItem){ .key = { id, 0 }, .count = 1, .body = p->buf, .len = TallyTagPut(p->buf, &t) };
    return OK;
}

/* USE: the Use Context, when there is one. */
static error_t UseNext(BinderWork* w, BinderItem* it, bool* got)
{
    BinderItems* p = &w->items;
    *got = !p->use && w->use && w->use->bytes;
    p->use = true;
    if (!*got)
        return OK;
    if (!ItemRoom(p, 10u + w->use->bytes))
        return ERR_NO_MEMORY;
    *it = (BinderItem){ .key = { 0, 0 }, .count = 1, .body = p->buf,
                        .len = TallyUsePut(p->buf, (const uint8_t*)w->use->text, w->use->bytes) };
    return OK;
}

/* Where DIGESTS' rotten ranges stand to `block`: inside one (true, *past its
 * end, UINT64_MAX for none), or the next one's first block after it in
 * *until (UINT64_MAX for none). */
static bool DigestsRot(const BinderWork* w, uint64_t block, uint64_t* past, uint64_t* until)
{
    *until = UINT64_MAX;
    for (uint32_t i = 0; i < w->nrot; i++) {
        const BinderRot* r = &w->rot[i];
        if (r->tree != TALLY_DIGESTS)
            continue;
        if (RotCovers(r, (TallyKey){ block, 0 })) {
            *past = r->at.has_hi ? r->at.hi.k1 : block + 1u;
            return true;
        }
        if (r->at.has_lo && r->at.lo.k1 > block && r->at.lo.k1 < *until)
            *until = r->at.lo.k1;
    }
    return false;
}

/* The digest of `block`: the Gloss's, or the Tally's. *have false when neither
 * names one; a refusal of the Tally's leaf, the medium's or memory's. */
static error_t DigestOf(uint64_t block, uint64_t* digest, bool* have)
{
    error_t refused = OK;
    *have = GlossFind(block, digest);
    if (*have)
        return OK;
    TallyDigestSays says = TallyDigestFind(block, digest, &refused);
    *have = says == TALLY_DIGEST_NAMED;
    return says == TALLY_DIGEST_UNREAD ? refused : OK;
}

/* The next digested block from the walk's place on, its run of ground and
 * the rotten ranges passed over: false at the end. */
static error_t DigestsFirst(BinderWork* w, uint64_t* block, uint64_t* digest, bool* found)
{
    BinderItems* p = &w->items;
    *found = false;
    while (p->ground < w->nground) {
        const BinderGround* g = &w->ground[p->ground];
        if (p->next < g->start)
            p->next = g->start;
        if (p->next >= g->start + g->count) {
            p->ground++;
            continue;
        }
        uint64_t past = 0, until = 0;
        if (DigestsRot(w, p->next, &past, &until)) {
            w->dropped++;
            p->next = past;
            continue;
        }
        bool    have = false;
        error_t rc   = DigestOf(p->next, digest, &have);
        if (rc != OK || have) {
            *block = p->next++;
            *found = rc == OK;
            return rc;
        }
        p->next++;
    }
    return OK;
}

/* DIGESTS: an element of the digests of blocks in a row, in one window of
 * DIGEST_ITEM_MOST, one run of ground — cut before a rotten range. */
static error_t DigestsNext(BinderWork* w, BinderItem* it, bool* got)
{
    BinderItems* p = &w->items;
    uint64_t     first = 0, digest = 0, past = 0, until = 0;
    error_t      rc = DigestsFirst(w, &first, &digest, got);
    if (rc != OK || !*got)
        return rc;
    if (!ItemRoom(p, 10u + 8u * (uint64_t)DIGEST_ITEM_MOST))
        return ERR_NO_MEMORY;
    uint8_t* d = p->buf + 10u;
    uint32_t n = 0;
    TallyLePut(d, digest, 8);
    n = 1;
    (void)DigestsRot(w, first, &past, &until);
    const BinderGround* g = &w->ground[p->ground];
    while (n < DIGEST_ITEM_MOST && p->next < g->start + g->count && p->next < until &&
           p->next / DIGEST_ITEM_MOST == first / DIGEST_ITEM_MOST) {
        bool have = false;
        if ((rc = DigestOf(p->next, &digest, &have)) != OK)
            return rc;
        if (!have)
            break;
        TallyLePut(d + 8u * n, digest, 8);
        n++;
        p->next++;
    }
    uint32_t head = BookVarintPut(p->buf, n);
    memmove(p->buf + head, d, 8u * n);
    *it = (BinderItem){ .key = { first, 0 }, .count = 1, .body = p->buf, .len = head + 8u * n };
    return OK;
}

/* The next element of the tree's leaves: *got false at its end. */
error_t LeafItemNext(BinderWork* w, BinderItem* it, bool* got)
{
    switch (w->tree) {
    case TALLY_FILES:
        return FilesNext(w, it, got);
    case TALLY_TAGS:
        return TagsNext(w, it, got);
    case TALLY_USE:
        return UseNext(w, it, got);
    case TALLY_DIGESTS:
        return DigestsNext(w, it, got);
    default:
        *got = false;
        return OK;
    }
}

// ----------------------------------------------------------------------------
// The inner levels: each node of the level below, and each rotten node of it
// ----------------------------------------------------------------------------

/* The copies a pointer is laid with: the two a writer writes (§2.5); a rotten
 * node's own as it named them — up to this many. */
#define BINDER_PTR_COPIES_MOST  16u

/* The level below's next node, or rotten node, in key order — the element
 * that names it: its first key, the count it carries, its BookPtr. */
error_t InnerItemNext(BinderWork* w, BinderItem* it, bool* got)
{
    BinderItems*       p   = &w->items;
    const BinderLevel* lv  = &w->lv[w->level - 1u];
    const BinderKid*   kid = w->kid_at < lv->n ? &lv->kid[w->kid_at] : NULL;
    const BinderRot*   rot = NULL;
    uint64_t           blocks[BINDER_PTR_COPIES_MOST];
    uint8_t            ptr[16u + 10u * BINDER_PTR_COPIES_MOST];
    uint32_t           plen = 0;
    while (w->rot_at < w->nrot) {
        const BinderRot* r = &w->rot[w->rot_at];
        if (r->tree == w->tree && r->at.level == w->level - 1u) {
            rot = r;
            break;
        }
        w->rot_at++;
    }
    *got = kid || rot;
    if (!*got)
        return OK;
    if (rot && (!kid || KeyLess(w->tree, RotKey(rot), kid->key))) {
        if (rot->at.ptr.copies > BINDER_PTR_COPIES_MOST)
            return ERR_TAGFS_CORRUPTED;
        for (uint32_t k = 0; k < rot->at.ptr.copies; k++)
            blocks[k] = TallyPtrBlock(&rot->at.ptr, k);
        plen = BookPtrPut(ptr, rot->at.ptr.seq, rot->at.ptr.sheets, blocks, rot->at.ptr.copies);
        if (!ItemRoom(p, 12u + plen))
            return ERR_NO_MEMORY;
        *it = (BinderItem){ .key = RotKey(rot), .count = rot->at.count, .body = p->buf,
                            .len = TallyInnerPut(p->buf, rot->at.count, ptr, plen) };
        w->rot_at++;
        return OK;
    }
    blocks[0] = w->measuring ? g_book.data_end : w->a + kid->off;
    blocks[1] = w->measuring ? g_book.data_end : w->b + kid->off;
    plen      = BookPtrPut(ptr, w->seq, kid->sheets, blocks, 2u);
    if (!ItemRoom(p, 12u + plen))
        return ERR_NO_MEMORY;
    *it = (BinderItem){ .key = kid->key, .count = kid->count, .body = p->buf,
                        .len = TallyInnerPut(p->buf, kid->count, ptr, plen) };
    w->kid_at++;
    return OK;
}
