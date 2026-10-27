#include "binder.h"
#include "../mount/book_mount.h"
#include "../tag_registry/tag_registry.h"
#include "../integrity/gloss.h"
#include "boardroom.h"

/*
 * The nodes of a whole Tally (binder.h): every tree's elements in key order,
 * packed into nodes and laid at A and B.
 *
 * ‼ ONE RULE OF PACKING, THREE WRITERS (§16.1 п.7, §16.2 п.24, §16.3 пп.1, 5).
 * mkfs (tools/create_tagfs_bind.c) and volume-read (tools/volumeread/
 * binder.py) lay a Tally by the same steps, and volume-read binds this one
 * again where it lies and holds it byte for byte (binder.Circle):
 *   - a file is its header and its Parcels — memory's Parcels, one to one,
 *     each born where its runs were (births.c); TAGS every tag the book holds,
 *     by id (TagRoll); USE the Use Context; DIGESTS a run of digested blocks
 *     inside one window of DIGEST_ITEM_MOST an element;
 *   - leaves filled greedily in key order: an element that would not fit the
 *     Sheet of the node being filled begins the next, one longer than a Sheet
 *     is the only element of a node of as many Sheets as it takes;
 *   - the inner levels the same, bottom up, each element a node's first key,
 *     the count it carries and its BookPtr, until one node — the root — is
 *     left;
 *   - the trees in the order of their kind, every node on an atom, copy 1 in
 *     the run at A and copy 2 at the same place in the run at B.
 *
 * ‼ A NODE NO COPY OF WHICH READS IS NAMED AS IT LIES (§16.4 п.5). A rotten
 * node of level L is an element of level L+1 — the key its old parent gave
 * it, its old count, its old pointer — and no node of level L or below holds
 * keys on both sides of it: the node being filled is closed before its first
 * key. An element whose key lies inside its range is left out: there is none
 * of FILES (a file born since has an id past every one the Tally knew), and a
 * DIGESTS element is cut at the range and what lies in it left out — counted
 * and said. A range with no end of its own ends at the next element's key.
 *
 * Every element comes out of memory with no TagFS lock held — the records of
 * the slice are held, the registry's text is resident and taken under its
 * own leaf lock, a digest is the Gloss's or the Tally's (TallyDigestFind may
 * read the medium) — and every node goes to the medium the same way.
 */

/* A run in the Tally's form with no extension, at the most: its head, its
 * start's distance and its birth_delta, each a varint at its widest. */
#define TALLY_RUN_PUT_MOST  30u

// ----------------------------------------------------------------------------
// Keys
// ----------------------------------------------------------------------------

static bool KeyLess(uint64_t tree, TallyKey a, TallyKey b)
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
static bool RotCovers(const BinderRot* r, TallyKey k)
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
static error_t LeafItemNext(BinderWork* w, BinderItem* it, bool* got)
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
static error_t InnerItemNext(BinderWork* w, BinderItem* it, bool* got)
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

// ----------------------------------------------------------------------------
// A node filled, sealed and laid
// ----------------------------------------------------------------------------

#define BINDER_HEAD_ROOM  48u       /* a NodeHead at its widest, ahead of the elements */

static uint32_t FillHeadBytes(const BinderWork* w, uint64_t items, TallyKey first)
{
    NodeHead h = { .tree = w->tree, .level = w->level, .items = items, .first = first };
    return NodeHeadBytes(&h);
}

/* Room in the node being filled for `bytes` more, and a start more. */
static bool FillRoom(BinderFill* f, uint64_t bytes)
{
    if (f->n + 1u >= f->capn) {
        uint32_t  cap  = f->capn ? f->capn * 2u : 256u;
        uint32_t* more = kmalloc(sizeof(uint32_t) * cap);
        if (!more)
            return false;
        if (f->n)
            memcpy(more, f->starts, sizeof(uint32_t) * f->n);
        kfree(f->starts);
        f->starts = more;
        f->capn   = cap;
    }
    if (BINDER_HEAD_ROOM + f->len + bytes > f->cap) {
        uint64_t cap  = BINDER_HEAD_ROOM + f->len + bytes + TALLY_SHEET_PAYLOAD;
        uint8_t* more = cap <= UINT32_MAX ? kmalloc(cap) : NULL;
        if (!more)
            return false;
        if (f->len)
            memcpy(more + BINDER_HEAD_ROOM, f->bytes + BINDER_HEAD_ROOM, f->len);
        kfree(f->bytes);
        f->bytes = more;
        f->cap   = (uint32_t)cap;
    }
    return true;
}

/* Whether `it` goes in the node being filled (§2.9): the first always — one
 * longer than a Sheet alone, in as many as it takes — and each next while the
 * node with it, its head grown with the count, still fits one Sheet. */
static bool FillTakes(const BinderWork* w, const BinderItem* it)
{
    const BinderFill* f = &w->fill;
    if (f->n == 0)
        return true;
    uint64_t size = FillHeadBytes(w, f->n + 1u, f->first) + f->len +
                    TallyItemBytes(w->tree, it->key, f->last, it->len);
    return size <= TALLY_SHEET_PAYLOAD;
}

/* `it` into the node being filled, its key written from the one before it —
 * the first from itself (§16.3 п.2). */
static error_t FillAdd(BinderWork* w, const BinderItem* it)
{
    BinderFill* f = &w->fill;
    if (!FillRoom(f, TallyItemBytes(w->tree, it->key, f->n ? f->last : it->key, it->len)))
        return ERR_NO_MEMORY;
    if (f->n == 0)
        f->first = it->key;
    f->starts[f->n++] = f->len;
    f->len  += TallyItemPut(f->bytes + BINDER_HEAD_ROOM + f->len, w->tree, it->key, f->n > 1u ? f->last : it->key,
                            it->body, it->len);
    f->last  = it->key;
    f->count += it->count;
    uint64_t runs = 0;
    if (w->tree == TALLY_FILES && w->level == 0 && it->key.k2 && BookVarintGet(it->body, it->len, &runs))
        w->laid.runs += runs;   /* a Parcel's runs: each widens with the next Tally's seq (endpaper_fact.c) */
    return OK;
}

/* Whether the node being filled holds more than a Sheet: one element alone,
 * in as many Sheets as it takes. */
static bool FillLong(const BinderWork* w)
{
    return w->fill.n == 1u && FillHeadBytes(w, 1u, w->fill.first) + w->fill.len > TALLY_SHEET_PAYLOAD;
}

/* One copy of the node sealed at `block` — every sector's sum seeded by its
 * address (§2.5) — and written, a window the seat takes at a time. */
static error_t LayCopy(BinderWork* w, const uint8_t* stream, uint32_t len, uint64_t sheets, uint64_t block)
{
    uint64_t sectors = sheets * TAGFS_BLOCK_SECTORS, vlba = block * TAGFS_BLOCK_SECTORS;
    uint32_t most    = BoardroomSeatRun(tagfs_get_seat());
    TallyUnitSeal(w->unit, stream, len, w->fill.starts, w->fill.n + 1u, BOOK_UNIT_SHEET, w->seq, vlba,
                  (uint32_t)sectors, tagfs_get_state()->uuid);
    for (uint64_t at = 0; at < sectors;) {
        uint32_t n  = sectors - at < most ? (uint32_t)(sectors - at) : most;
        int      rc = tagfs_volume_write(vlba + at, n, w->unit + at * BOOK_SECTOR_BYTES);
        if (rc != 0)
            return rc == (int)ERR_DEVICE_NOT_READY ? ERR_DEVICE_NOT_READY
                 : rc == (int)ERR_TAGFS_READ_ONLY  ? ERR_TAGFS_READ_ONLY : ERR_WRITE_FAILED;
        at += n;
    }
    return OK;
}

/* A kid more for the level the node was laid at. */
static bool LevelPut(BinderLevel* lv, const BinderKid* kid)
{
    if (lv->n == lv->cap) {
        uint32_t   cap  = lv->cap ? lv->cap * 2u : 64u;
        BinderKid* more = kmalloc(sizeof(BinderKid) * cap);
        if (!more)
            return false;
        if (lv->n)
            memcpy(more, lv->kid, sizeof(BinderKid) * lv->n);
        kfree(lv->kid);
        lv->kid = more;
        lv->cap = cap;
    }
    lv->kid[lv->n++] = *kid;
    return true;
}

/*
 * The node being filled closed and laid: its NodeHead written before its
 * elements, the node put on the atom at or past the runs' next block — copy 1
 * at A, copy 2 at the same place at B — and named for the level above. With
 * `measuring`, nothing is written: the blocks are counted.
 */
static error_t FillLay(BinderWork* w)
{
    BinderFill* f    = &w->fill;
    NodeHead    h    = { .tree = w->tree, .level = w->level, .items = f->n, .first = f->first };
    uint32_t    head = NodeHeadBytes(&h);
    uint8_t*    s    = f->bytes + BINDER_HEAD_ROOM - head;
    uint32_t    len  = head + f->len;
    uint64_t    sheets = len > TALLY_SHEET_PAYLOAD ? (len + TALLY_SHEET_PAYLOAD - 1u) / TALLY_SHEET_PAYLOAD : 1u;
    uint64_t    off  = (w->off + w->grid - 1u) / w->grid * w->grid;
    error_t     rc   = OK;
    NodeHeadPut(s, &h);
    for (uint32_t i = f->n; i > 0; i--)
        f->starts[i] = f->starts[i - 1u] + head;
    f->starts[0] = 0;
    if (!w->measuring) {
        if (off + sheets > w->room) {
            kprintf("[TagFS] DEFECT: the Tally's node at %llu takes %llu Sheet(s) past the %llu block(s) its "
                    "measure gave it — no Tally is written\n", (unsigned long long)off,
                    (unsigned long long)sheets, (unsigned long long)w->room);
            return ERR_TAGFS_CORRUPTED;
        }
        if (sheets * TAGFS_BLOCK_SIZE > w->unit_room) {
            kfree(w->unit);
            w->unit_room = 0;
            if (!(w->unit = kmalloc(sheets * TAGFS_BLOCK_SIZE)))
                return ERR_NO_MEMORY;
            w->unit_room = sheets * TAGFS_BLOCK_SIZE;
        }
        if ((rc = LayCopy(w, s, len, sheets, w->a + off)) == OK)
            rc = LayCopy(w, s, len, sheets, w->b + off);
        if (rc != OK)
            return rc;
    }
    BinderKid kid = { .key = f->first, .count = f->count, .sheets = sheets, .off = off, .rot = NULL };
    if (!LevelPut(&w->lv[w->level], &kid))
        return ERR_NO_MEMORY;
    EndpaperLaidNode(&w->laid, w->tree, w->level, sheets, w->grid, f->first.k1);
    w->off = off + sheets;
    w->nodes++;
    f->n = 0;
    f->len = 0;
    f->count = 0;
    return OK;
}

// ----------------------------------------------------------------------------
// The walk: a node a call
// ----------------------------------------------------------------------------

/* Whether a rotten node of this level or above stands between the node being
 * filled and `key`: it is closed there. */
static bool FillBarred(const BinderWork* w, TallyKey key)
{
    if (w->fill.n == 0)
        return false;
    for (uint32_t i = 0; i < w->nrot; i++) {
        const BinderRot* r = &w->rot[i];
        if (r->tree == w->tree && r->at.level >= w->level && r->at.has_lo &&
            KeyLess(w->tree, w->fill.last, r->at.lo) && !KeyLess(w->tree, key, r->at.lo))
            return true;
    }
    return false;
}

/* Whether a leaf element's key lies inside a rotten range: a DIGESTS element
 * is never cut so (DigestsNext); a file's, never one this kernel wrote. */
static bool ItemRotten(const BinderWork* w, TallyKey key)
{
    for (uint32_t i = 0; i < w->nrot; i++)
        if (w->rot[i].tree == w->tree && RotCovers(&w->rot[i], key))
            return true;
    return false;
}

/* The node of this level filled until it is full, or its elements are spent:
 * *laid when one was laid; nothing laid and nothing held, the level is done. */
static error_t LevelStep(BinderWork* w, bool* laid)
{
    BinderItems* p = &w->items;
    *laid = false;
    for (;;) {
        bool    got = p->held;
        error_t rc  = OK;
        if (!got)
            rc = w->level == 0 ? LeafItemNext(w, &p->item, &got) : InnerItemNext(w, &p->item, &got);
        if (rc != OK)
            return rc;
        p->held = false;
        if (!got)
            break;
        if (w->level == 0 && ItemRotten(w, p->item.key)) {
            kprintf("[TagFS] DEFECT: the %s element (%llu, %llu) lies inside a rotten range the Tally names "
                    "as it lies — no Tally is written\n", TallyTreeName(w->tree),
                    (unsigned long long)p->item.key.k1, (unsigned long long)p->item.key.k2);
            return ERR_TAGFS_CORRUPTED;
        }
        if (FillBarred(w, p->item.key) || !FillTakes(w, &p->item)) {
            p->held = true;
            *laid   = true;
            return FillLay(w);
        }
        if ((rc = FillAdd(w, &p->item)) != OK)
            return rc;
        if (FillLong(w)) {
            *laid = true;
            return FillLay(w);
        }
    }
    if (w->fill.n == 0)
        return OK;
    *laid = true;
    return FillLay(w);
}

/* Whether a rotten node of `tree` stands at level `level` or above. */
static bool RotAbove(const BinderWork* w, uint64_t level)
{
    for (uint32_t i = 0; i < w->nrot; i++)
        if (w->rot[i].tree == w->tree && w->rot[i].at.level >= level)
            return true;
    return false;
}

static void LevelsLetGo(BinderWork* w)
{
    for (uint32_t l = 0; l < BINDER_LEVELS_MOST; l++) {
        kfree(w->lv[l].kid);
        w->lv[l] = (BinderLevel){ NULL, 0, 0 };
    }
}

/* The tree after `tree`, in the order of their kind, that a Tally writes:
 * FILES, TAGS, USE, DIGESTS — 0 past the last. */
static uint32_t TreeAfter(uint32_t tree)
{
    switch (tree) {
    case 0:
        return TALLY_FILES;
    case TALLY_FILES:
        return TALLY_TAGS;
    case TALLY_TAGS:
        return TALLY_USE;
    case TALLY_USE:
        return TALLY_DIGESTS;
    default:
        return 0;
    }
}

/* The tree's walk begun at its leaves. */
static void TreeBegin(BinderWork* w, uint32_t tree)
{
    BinderItems* p = &w->items;
    LevelsLetGo(w);
    w->tree   = tree;
    w->level  = 0;
    w->kid_at = 0;
    w->rot_at = 0;
    p->tree   = tree;
    p->chunk  = w->files;
    p->at     = 0;
    p->head   = false;
    p->kept   = 0;
    p->kept_parcel = 0;
    p->kept_head   = false;
    p->block  = 0;
    p->tag    = 0;
    p->use    = false;
    p->ground = 0;
    p->next   = 0;
    p->held   = false;
    w->fill.n = 0;
    w->fill.len = 0;
    w->fill.count = 0;
}

/* The DIGESTS leaves laid, kept for the reads to turn to once the slot names
 * them (binder.c) — a copy: the level above is still to be built of them. */
static void DigestLeavesKept(BinderWork* w)
{
    const BinderLevel* lv = &w->lv[0];
    kfree(w->dleaves);
    w->dleaves  = NULL;
    w->ndleaves = 0;
    if (lv->n && !(w->dleaves = kmalloc(sizeof(BinderKid) * lv->n))) {
        w->dleaves_lost = true;
        return;
    }
    if (lv->n)
        memcpy(w->dleaves, lv->kid, sizeof(BinderKid) * lv->n);
    w->ndleaves = lv->n;
}

/* The level's elements spent: the root found — one node at this level and no
 * rotten node above it — or the next level begun; a tree of no node at all
 * leaves no root. Then the next tree. */
static void LevelEnded(BinderWork* w)
{
    const BinderLevel* lv = &w->lv[w->level];
    bool               rot = RotAbove(w, w->level);
    if (w->tree == TALLY_DIGESTS && w->level == 0 && !w->measuring)
        DigestLeavesKept(w);
    if (lv->n == 1u && !rot) {
        w->root[w->tree] = (BinderRoot){ .laid = true, .level = w->level, .items = lv->kid[0].count,
                                         .sheets = lv->kid[0].sheets, .off = lv->kid[0].off };
    } else if ((lv->n || rot) && w->level + 1u < BINDER_LEVELS_MOST) {
        w->level++;
        w->kid_at = 0;
        w->rot_at = 0;
        return;
    }
    uint32_t next = TreeAfter(w->tree);
    if (next)
        TreeBegin(w, next);
    else
        w->trees_done = true;
}

void BinderNodesBegin(BinderWork* w)
{
    kfree(w->dleaves);
    w->dleaves      = NULL;
    w->ndleaves     = 0;
    w->dleaves_lost = false;
    w->off        = 0;
    w->nodes      = 0;
    w->dropped    = 0;
    w->trees_done = false;
    memset(w->root, 0, sizeof(w->root));
    memset(&w->laid, 0, sizeof(w->laid));
    for (uint32_t i = 0; i < w->nrot; i++)
        EndpaperLaidNode(&w->laid, w->rot[i].tree, w->rot[i].at.level, 0, w->grid,
                         w->rot[i].at.has_lo ? w->rot[i].at.lo.k1 : 0);
    TreeBegin(w, TreeAfter(0));
}

error_t BinderNodeNext(BinderWork* w, bool* done)
{
    *done = false;
    while (!w->trees_done) {
        bool    laid = false;
        error_t rc   = LevelStep(w, &laid);
        if (rc != OK || laid)
            return rc;
        LevelEnded(w);
    }
    *done = true;
    return OK;
}

void BinderNodesLetGo(BinderWork* w)
{
    LevelsLetGo(w);
    kfree(w->items.buf);
    kfree(w->fill.bytes);
    kfree(w->fill.starts);
    kfree(w->unit);
    kfree(w->dleaves);
    w->dleaves     = NULL;
    w->ndleaves    = 0;
    w->items.buf   = NULL;
    w->items.room  = 0;
    w->fill        = (BinderFill){ 0 };
    w->unit        = NULL;
    w->unit_room   = 0;
}

// ----------------------------------------------------------------------------
// The reads of a digest turned to the Tally's DIGESTS
// ----------------------------------------------------------------------------

/* The place a DIGESTS leaf of the new Tally has, as a mount of it would
 * name it: the leaf's own pointer — or a rotten node's, named as it lies —
 * its keys from its own to the next one's. */
static void DigestPlace(const BinderWork* w, const BinderKid* kid, const BinderRot* rot, TallyPlace* p)
{
    if (rot) {
        *p = rot->at;
        p->lo = rot->at.has_lo ? rot->at.lo : (TallyKey){ 0, 0 };
        return;
    }
    memset(p, 0, sizeof(*p));
    p->ptr      = (TallyPtr){ .seq = w->seq, .sheets = kid->sheets, .copies = 2u, .more = 0,
                              .block = { w->a + kid->off, w->b + kid->off } };
    p->level    = 0;
    p->count    = kid->count;
    p->lo       = kid->key;
}

error_t BinderDigestsTurn(const BinderWork* w)
{
    uint32_t    n = w->ndleaves, k = 0, r = 0;
    for (uint32_t i = 0; i < w->nrot; i++)
        n += w->rot[i].tree == TALLY_DIGESTS;
    TallyPlace* places = kmalloc(sizeof(TallyPlace) * (n ? n : 1u));
    bool*       rotten = kmalloc(sizeof(bool) * (n ? n : 1u));
    error_t     rc     = places && rotten && !w->dleaves_lost ? OK : ERR_NO_MEMORY;
    for (uint32_t i = 0; rc == OK && i < n; i++) {
        while (r < w->nrot && w->rot[r].tree != TALLY_DIGESTS)
            r++;
        const BinderRot* rot = r < w->nrot ? &w->rot[r] : NULL;
        bool             take_rot = rot && (k == w->ndleaves ||
                                            (rot->at.has_lo ? rot->at.lo.k1 : 0) < w->dleaves[k].key.k1);
        DigestPlace(w, take_rot ? NULL : &w->dleaves[k], take_rot ? rot : NULL, &places[i]);
        rotten[i] = take_rot;
        r += take_rot;
        k += !take_rot;
    }
    for (uint32_t i = 0; rc == OK && i < n; i++) {
        places[i].has_lo = i > 0;
        places[i].has_hi = i + 1u < n;
        places[i].hi     = i + 1u < n ? places[i + 1u].lo : (TallyKey){ 0, 0 };
    }
    if (rc == OK)
        rc = TallyDigestsTurn(places, rotten, n);
    kfree(places);
    kfree(rotten);
    if (rc == ERR_NO_MEMORY)
        kprintf("[TagFS] no memory to turn the reads of a digest to Tally %llu's DIGESTS — they read the Tally "
                "before it and the Gloss, which keeps every digest it holds\n", (unsigned long long)w->seq);
    return rc;
}
