#include "binder_internal.h"
#include "../mount/book_mount.h"
#include "boardroom.h"

/*
 * The nodes of a Tally (binder.h): every tree's elements in key order — of a
 * Tally laid in, the elements of the leaves it lays — packed into nodes and
 * laid at A and B, among the nodes it names as they lie.
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
 * ‼ A NODE NAMED AS IT LIES IS AN ELEMENT OF THE LEVEL ABOVE IT (§16.4 п.5,
 * §16.5 п.16). A node no copy of which reads, and a node nothing under which
 * changed (TipIn), of level L is an element of level L+1 — the key its old
 * parent gave it, its old count, its old pointer — and no node of level L or
 * below holds keys on both sides of it: the node being filled is closed
 * before its first key, the leaves laid anew tipped in between. An element
 * whose key lies inside the range of one that changed nothing is its, and not
 * laid. Inside a rotten one there is none of FILES (a file born since has an
 * id past every one the Tally knew), and a DIGESTS element is cut at the range
 * and what lies in it left out — counted and said. A rotten range with no end
 * of its own ends at the next element's key.
 *
 * ‼ A ROW SHORTER THAN HALF A SHEET TAKES ITS NEIGHBOUR IN (Р-d1 (а)). The
 * leaves laid anew between two named as they lie are a row; one that comes to
 * less than half a Sheet would leave a leaf that empty for good, so the
 * measure of the Tally takes into it the leaf TipIn offered beside it — the
 * one after it as the measure meets it, or the one before it once the row has
 * ended short, the tree's measure begun again once at its end for every one
 * so taken — and the walk that writes lays the two as one. The rule of
 * packing is the one every writer keeps: a row is packed greedily, as the
 * leaves of a whole Tally are.
 *
 * Every element comes out of memory with no TagFS lock held — the records of
 * the slice are held, the registry's text is resident and taken under its
 * own leaf lock, a digest is the Gloss's or the Tally's (TallyDigestFind may
 * read the medium) — and every node goes to the medium the same way.
 */

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
    w->keyed  = true;
    if (w->level == 0)
        w->row += TallyItemBytes(w->tree, it->key, f->n > 1u ? f->last : it->key, it->len);
    uint64_t runs = 0;
    if (w->tree == TALLY_FILES && w->level == 0 && it->key.k2 && BookVarintGet(it->body, it->len, &runs)) {
        w->laid.runs += runs;   /* a Parcel's runs: each widens with the next Tally's seq (endpaper_fact.c) */
        f->items     += runs;
    }
    if (w->tree == TALLY_TAGS && w->level == 0) {
        f->items++;
        f->text += it->text;
    }
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
    BinderKid kid = { .key = f->first, .count = f->count, .sheets = sheets,
                      .block = { w->a + off, w->b + off }, .items = f->items, .text = f->text };
    if (!LevelPut(&w->lv[w->level], &kid))
        return ERR_NO_MEMORY;
    EndpaperLaidNode(&w->laid, w->tree, w->level, sheets, w->grid, f->first.k1);
    if (w->tree == TALLY_FILES && w->level == 0 && f->items)
        w->laid.run_leaves++;
    w->off = off + sheets;
    w->nodes++;
    w->sheets += sheets;
    f->n = 0;
    f->len = 0;
    f->count = 0;
    f->items = 0;
    f->text  = 0;
    return OK;
}

// ----------------------------------------------------------------------------
// The walk: a node a call
// ----------------------------------------------------------------------------

/* Half a Sheet: a row shorter than this takes a margin in. */
#define BINDER_ROW_SHORT  (TALLY_SHEET_PAYLOAD / 2u)

/* The row ended: one short of half a Sheet takes the margin before it in, on
 * the tree's first measure — it is measured when that begins again. */
static void RowEnded(BinderWork* w)
{
    if (!w->measuring || w->level != 0 || w->row_again || !w->row || w->row >= BINDER_ROW_SHORT || !w->row_left)
        return;
    BinderKept* k = &w->kept[w->row_left - 1u];
    if (k->margin && !k->taken) {
        k->taken      = true;
        w->rows_short = true;
    }
}

/* A node named as it lies passed at the leaves: the row before it ended, and
 * the row after it begins — its margin, the node right before it, noted. */
static void RowBegun(BinderWork* w, const BinderKept* k)
{
    if (w->row)
        RowEnded(w);
    w->row      = 0;
    w->row_left = k->margin && k->at.level == 0 ? (uint32_t)(k - w->kept) + 1u : 0;
}

/*
 * A leaf element against the nodes named as they lie: *skip when one holds
 * it — its key's, not laid. A margin TipIn offered is taken into the row by
 * the measure when the row before it comes to less than half a Sheet, and is
 * laid. An element inside a rotten range is a DEFECT: a DIGESTS element is
 * never cut so (binder_items.c, DigestsNext), and a file's never one this
 * kernel wrote.
 */
static error_t LeafKept(BinderWork* w, TallyKey key, bool* skip)
{
    BinderKept* k = BinderKeptAt(w, key);
    *skip = false;
    if (!k)
        return OK;
    if (k->rotten) {
        kprintf("[TagFS] DEFECT: the %s element (%llu, %llu) lies inside a rotten range the Tally names as it "
                "lies — no Tally is written\n", TallyTreeName(w->tree), (unsigned long long)key.k1,
                (unsigned long long)key.k2);
        return ERR_TAGFS_CORRUPTED;
    }
    if (k->margin && w->measuring && w->row && w->row < BINDER_ROW_SHORT) {
        k->taken = true;
        return OK;
    }
    *skip = true;
    RowBegun(w, k);
    return OK;
}

/* The node of this level filled until it is full, or its elements are spent:
 * *spent when the turn is — a node laid, or the self-check owed a leaf of the
 * Tally before the next turn reads, the element held for it; nothing laid and
 * nothing held, the level is done. */
static error_t LevelStep(BinderWork* w, bool* spent)
{
    BinderItems* p = &w->items;
    *spent = false;
    for (;;) {
#if BINDER_CHECKS
        bool    fresh = !p->held || BinderCheckOwed();  /* not one held back from the node before, checked */
#endif
        bool    got   = p->held;
        error_t rc    = OK;
        if (!got)
            rc = w->level == 0 ? BinderLeafItemNext(w, &p->item, &got) : BinderInnerItemNext(w, &p->item, &got);
        if (rc != OK)
            return rc;
        p->held = false;
        if (!got)
            break;
        bool skip = false;
        if (w->level == 0 && (rc = LeafKept(w, p->item.key, &skip)) != OK)
            return rc;
#if BINDER_CHECKS
        if (w->level == 0 && fresh && !w->measuring && (rc = BinderCheckItem(w, &p->item)) != OK)
            return rc;
        if (BinderCheckOwed()) {
            p->held = true;
            *spent  = true;
            return OK;
        }
#endif
        if (skip)
            continue;
        const BinderKept* bar = BinderKeptBars(w, p->item.key);
        if (bar && w->level == 0)
            RowBegun(w, bar);
        if ((bar && w->fill.n) || !FillTakes(w, &p->item)) {
            p->held = true;
            *spent  = true;
            return FillLay(w);
        }
        if ((rc = FillAdd(w, &p->item)) != OK)
            return rc;
        if (FillLong(w)) {
            *spent = true;
            return FillLay(w);
        }
    }
    if (w->fill.n == 0)
        return OK;
    *spent = true;
    return FillLay(w);
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
    w->row        = 0;
    w->row_left   = 0;
    w->rows_short = false;
    w->row_again  = false;
    w->tree_off  = w->off;
    BinderKeptTreeBegin(w);
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
    p->digest    = (BinderDigestAt){ 0, 0, w->tree_kept };
    p->tip_at    = (BinderDigestAt){ 0, 0, w->tree_kept };
    p->tip_range = 0;
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

/* The tree's root named as it lies, nothing under it changed: no node of the
 * tree laid — the draft of the Foliation takes it and everything under it as
 * they were. */
static void TreeKeptWhole(BinderWork* w, const BinderKept* k)
{
    w->root[w->tree] = (BinderRoot){ .laid = true, .level = k->at.level, .items = k->at.count, .ptr = k->at.ptr };
    if (w->measuring)
        return;
    FoliationDraftNode(w->tree, k->at.level, k->at.has_lo ? k->at.lo : (TallyKey){ 0, 0 }, true, k->at.count,
                       &k->at.ptr, k->items, k->text);
    FoliationDraftUnder(w->tree, &k->at);
    w->named++;
}

/* The trees from `tree` on whose roots are named whole as they lie passed
 * over: the walk begun at the next tree that lays a node, or ended. The
 * self-check walks such a tree's leaves all the same, laying none, to hold
 * every element of it to the Tally before (binder_check.c). */
static void TreesOn(BinderWork* w, uint32_t tree)
{
    for (; tree; tree = TreeAfter(tree)) {
        TreeBegin(w, tree);
        const BinderKept* whole = BinderKeptRoot(w);
        if (!whole || (BINDER_CHECKS && !w->measuring))
            return;
        TreeKeptWhole(w, whole);
    }
    w->trees_done = true;
}

/* The measure's leaves spent: the last row ended, and the tree's measure
 * begun again if a short row took the margin before it in — once a tree. */
static bool RowAgain(BinderWork* w)
{
    RowEnded(w);
    if (!w->measuring || w->level != 0 || w->row_again || !w->rows_short)
        return false;
    uint64_t off = w->tree_off;
    TreeBegin(w, w->tree);
    w->off       = off;
    w->tree_off  = off;
    w->row_again = true;
    return true;
}

/* The level's elements spent: the root found — one node at this level and no
 * node named as it lies above it — or the next level begun; a tree of no node
 * at all leaves no root. Then the next tree. */
static void LevelEnded(BinderWork* w)
{
    const BinderLevel* lv = &w->lv[w->level];
    const BinderKept*  whole = w->level == 0 ? BinderKeptRoot(w) : NULL;
    bool               kept = BinderKeptAbove(w, w->level);
    if (whole) {
        TreeKeptWhole(w, whole);
        TreesOn(w, TreeAfter(w->tree));
        return;
    }
    if (RowAgain(w))
        return;
    if (w->tree == TALLY_DIGESTS && w->level == 0 && !w->measuring)
        DigestLeavesKept(w);
    if (lv->n == 1u && !kept) {
        const BinderKid* kid = &lv->kid[0];
        TallyPtr ptr = { .seq = w->seq, .sheets = kid->sheets, .copies = 2u, .more = 0,
                         .block = { kid->block[0], kid->block[1] } };
        w->root[w->tree] = (BinderRoot){ .laid = true, .level = w->level, .items = kid->count, .ptr = ptr };
        if (!w->measuring)
            FoliationDraftNode(w->tree, w->level, kid->key, true, kid->count, &ptr, kid->items, kid->text);
    } else if ((lv->n || kept) && w->level + 1u < BINDER_LEVELS_MOST) {
        w->level++;
        w->kid_at = 0;
        BinderKeptLevelBegin(w);
        return;
    }
    TreesOn(w, TreeAfter(w->tree));
}

void BinderNodesBegin(BinderWork* w)
{
    kfree(w->dleaves);
    w->dleaves      = NULL;
    w->ndleaves     = 0;
    w->dleaves_lost = false;
    w->off        = 0;
    w->nodes      = 0;
    w->sheets     = 0;
    w->named      = 0;
    w->dsheets    = 0;
    w->dropped    = 0;
    w->trees_done = false;
    memset(w->root, 0, sizeof(w->root));
    memset(&w->laid, 0, sizeof(w->laid));
    BinderKeptLaid(w);
    if (!w->measuring) {
        FoliationDraftBegin();
#if BINDER_CHECKS
        BinderCheckBegin();
#endif
    }
    TreesOn(w, TreeAfter(0));
}

/* A tree's leaves spent: what is left of the Tally before's under them held
 * to DogEar, a leaf of it a turn (binder_check.c) — *more while some is. */
static error_t LeavesSpent(BinderWork* w, bool* more)
{
    *more = false;
#if BINDER_CHECKS
    if (w->level == 0 && !w->measuring)
        return BinderCheckTreeEnd(w, more);
#else
    (void)w;
#endif
    return OK;
}

error_t BinderNodeNext(BinderWork* w, bool* done)
{
    *done = false;
#if BINDER_CHECKS
    BinderCheckTurn();
#endif
    while (!w->trees_done) {
        bool    spent = false, more = false;
        error_t rc    = OK;
#if BINDER_CHECKS
        if (!(w->level == 0 && !w->measuring && BinderCheckDraining()))
#endif
            rc = LevelStep(w, &spent);
        if (rc != OK || spent)
            return rc;
        if ((rc = LeavesSpent(w, &more)) != OK || more)
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
    kfree(w->items.tip_buf);
    w->items.tip_buf = NULL;
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
static void DigestPlace(const BinderWork* w, const BinderKid* kid, const BinderKept* rot, TallyPlace* p)
{
    if (rot) {
        *p = rot->at;
        p->lo = rot->at.has_lo ? rot->at.lo : (TallyKey){ 0, 0 };
        return;
    }
    memset(p, 0, sizeof(*p));
    p->ptr      = (TallyPtr){ .seq = w->seq, .sheets = kid->sheets, .copies = 2u, .more = 0,
                              .block = { kid->block[0], kid->block[1] } };
    p->level    = 0;
    p->count    = kid->count;
    p->lo       = kid->key;
}

error_t BinderDigestsTurn(const BinderWork* w)
{
    if (w->tip) {
        error_t tip = BinderDigestsTipTurn(w);
        if (tip == ERR_NO_MEMORY)
            kprintf("[TagFS] no memory to turn the reads of a digest to Tally %llu's DIGESTS — they read the Tally "
                    "before it and the Gloss, which keeps every digest it holds\n", (unsigned long long)w->seq);
        return tip;
    }
    uint32_t    n = w->ndleaves, k = 0, r = 0;
    for (uint32_t i = 0; i < w->nkept; i++)
        n += w->kept[i].tree == TALLY_DIGESTS && !w->kept[i].taken;
    TallyPlace* places = kmalloc(sizeof(TallyPlace) * (n ? n : 1u));
    bool*       rotten = kmalloc(sizeof(bool) * (n ? n : 1u));
    error_t     rc     = places && rotten && !w->dleaves_lost ? OK : ERR_NO_MEMORY;
    for (uint32_t i = 0; rc == OK && i < n; i++) {
        while (r < w->nkept && (w->kept[r].tree != TALLY_DIGESTS || w->kept[r].taken))
            r++;
        const BinderKept* rot = r < w->nkept ? &w->kept[r] : NULL;
        bool             take_rot = rot && (k == w->ndleaves ||
                                            (rot->at.has_lo ? rot->at.lo.k1 : 0) < w->dleaves[k].key.k1);
        DigestPlace(w, take_rot ? NULL : &w->dleaves[k], take_rot ? rot : NULL, &places[i]);
        rotten[i] = take_rot;
        r += take_rot;
        k += !take_rot;
    }
    if (rc == OK) {
        TipInLeavesClosed(places, n);
        rc = TallyDigestsTurn(places, rotten, n);
    }
    kfree(places);
    kfree(rotten);
    if (rc == ERR_NO_MEMORY)
        kprintf("[TagFS] no memory to turn the reads of a digest to Tally %llu's DIGESTS — they read the Tally "
                "before it and the Gloss, which keeps every digest it holds\n", (unsigned long long)w->seq);
    return rc;
}
