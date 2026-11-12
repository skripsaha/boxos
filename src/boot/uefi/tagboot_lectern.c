/*
 * TagBoot's Lectern — a format-v2 volume's book read the way the kernel's
 * mount stands on it, and Attest: the kernel's blocks held to the digests the
 * book keeps of them before the loader jumps (tagboot_lectern.h).
 *
 * What it holds at once is a few nodes: the slot being tried, and the path
 * down the DIGESTS tree to the leaf that names the block in hand — one rung a
 * level, each rung kept while the blocks in hand stay in its range. The
 * kernel's blocks lie in a row, so every node on their paths is read once,
 * and a copy of it that did not hold is said once.
 */

/* The loader's integer types are uefi.h's (it builds -nostdinc, with no
 * <stdint.h>); the host stand's are the host's. */
#if __STDC_HOSTED__
#include <stdint.h>
#else
#include "uefi.h"
#endif

#include "tagboot_lectern.h"

/* ------------------------------------------------------------------------- */
/* Lines, and bytes, with no library under them                              */
/* ------------------------------------------------------------------------- */

typedef struct {
    char     text[LECTERN_LINE_BYTES];
    uint32_t at;
} LecternLine;

static void LineStart(LecternLine *l)
{
    l->at      = 0;
    l->text[0] = '\0';
}

static void LinePut(LecternLine *l, const char *s)
{
    while (*s && l->at + 1u < LECTERN_LINE_BYTES) l->text[l->at++] = *s++;
    l->text[l->at] = '\0';
}

static void LineNum(LecternLine *l, uint64_t v)
{
    char     digits[20];
    uint32_t n = 0;
    do {
        digits[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    while (n && l->at + 1u < LECTERN_LINE_BYTES) l->text[l->at++] = digits[--n];
    l->text[l->at] = '\0';
}

static void LineSay(const LecternDesk *d, const LecternLine *l)
{
    d->say(d->ctx, l->text);
}

static void LecternZero(void *p, uint64_t n)
{
    volatile uint8_t *b = (volatile uint8_t *)p;     /* never turned into a call to a memset the loader has not */
    while (n--) *b++ = 0;
}

static int LecternZeros(const uint8_t *p, uint64_t n)
{
    for (uint64_t i = 0; i < n; i++)
        if (p[i]) return 0;
    return 1;
}

/* ------------------------------------------------------------------------- */
/* What is held while the book is read                                       */
/* ------------------------------------------------------------------------- */

/* Keys [lo, hi) of a tree — no lo from its start, no hi to its end. */
typedef struct {
    TallyKey lo, hi;
    int      has_lo, has_hi;
} LecternRange;

/* A node read whole: its unit in `room` (`bytes` the desk gave), its stream
 * laid over the room's front (`stream` bytes), its head, where its items
 * begin. */
typedef struct {
    uint8_t *room;
    uint64_t bytes;
    uint32_t stream;
    NodeHead head;
    uint32_t items_at;
} LecternNode;

/* A rung of the path down DIGESTS: the node held at its level, or the range at
 * its level no copy of whose node holds. */
typedef struct {
    LecternNode  node;      /* node.room NULL: no node held */
    LecternRange range;
    int          rotten;
} LecternRung;

typedef struct {
    const LecternDesk *desk;
    LecternRung       *rungs;          /* rungs[level], 0 the leaves, `top` the root; NULL: no DIGESTS */
    uint64_t           top;
    uint64_t           lane;           /* the digests' seed: lane 0 of the volume's uuid */
    BookPtr            root_ptr;       /* the DIGESTS root's pointer, while the slot it is in is held */
    uint32_t           root_failed;    /* its copies that did not hold */
} LecternBook;

static const char *const g_tree_names[] = {
    "", "FILES", "TAGS", "USE", "BOOKMARKS", "LIEN", "DIGESTS",
};

static int KeyBefore(TallyKey a, TallyKey b)
{
    return a.k1 < b.k1 || (a.k1 == b.k1 && a.k2 < b.k2);
}

static int RangeHolds(const LecternRange *r, TallyKey key)
{
    return !(r->has_lo && KeyBefore(key, r->lo)) && !(r->has_hi && !KeyBefore(key, r->hi));
}

static void NodeGive(const LecternDesk *d, LecternNode *n)
{
    if (n->room) d->give(d->ctx, n->room, n->bytes);
    n->room = (uint8_t *)0;
}

/* ------------------------------------------------------------------------- */
/* Units and nodes                                                           */
/* ------------------------------------------------------------------------- */

typedef enum { UNIT_HOLDS, UNIT_BLANK, UNIT_TORN, UNIT_NO_ROOM } LecternUnitSays;

/*
 * `blocks` blocks of the book from volume block `block`, read into room the
 * desk gives and judged one whole unit of `kind`, sealed at `seq` (0: at any)
 * — volume_book.h BookUnitCheck. HOLDS leaves the room in *room with the
 * unit's stream laid over its front, in place: sector k's payload moves down
 * to k * 488, below every byte not yet moved. BLANK is sectors all zeros —
 * what a slot never written is; TORN any other, the medium refusing them too.
 * Neither keeps the room. NO_ROOM when the desk gives none.
 */
static LecternUnitSays LecternUnit(const LecternDesk *d, uint64_t block, uint64_t blocks, uint32_t kind,
                                   uint64_t seq, uint8_t **room, BookSectorHead *head)
{
    uint64_t        bytes   = blocks * VOLUME_BLOCK_BYTES;
    uint32_t        sectors = (uint32_t)(blocks * VOLUME_BLOCK_SECTORS);
    uint8_t        *r       = d->take(d->ctx, bytes);
    LecternUnitSays says    = UNIT_TORN;

    *room = (uint8_t *)0;
    if (!r) return UNIT_NO_ROOM;
    if (d->read(d->ctx, block * VOLUME_BLOCK_SECTORS, sectors, r)) {
        if (LecternZeros(r, bytes))
            says = UNIT_BLANK;
        else if (BookUnitCheck(r, sectors, d->uuid, block * VOLUME_BLOCK_SECTORS, head) && head->kind == kind &&
                 (!seq || head->seq == seq))
            says = UNIT_HOLDS;
    }
    if (says != UNIT_HOLDS) {
        d->give(d->ctx, r, bytes);
        return says;
    }
    BookUnitStream(r, sectors, r);
    *room = r;
    return UNIT_HOLDS;
}

/*
 * Does a copy's stream hold as a node of the range `r` — a NodeHead; its items
 * each whole, their keys rising from its first key; nothing but PAD past the
 * last; every key inside the range its parent gives it? The kernel's rule
 * (mount/tally_walk.c, TallyNodeParse) and volume-read's (tally.py,
 * NodeParse): a loader that held a node another kernel refuses would stand on
 * another slot than the kernel's mount, and hold the kernel's blocks to the
 * digests of another Tally.
 */
static int LecternNodeHolds(LecternNode *n, const LecternRange *r)
{
    const uint8_t *s   = n->room;
    uint32_t       end = n->stream, at = 0, body, len;
    TallyKey       prev, key;

    if (NodeHeadGet(s, end, &at, &n->head) == TALLY_BODY_BROKEN || n->head.items > end) return 0;
    n->items_at = at;
    prev        = n->head.first;
    for (uint64_t i = 0; i < n->head.items; i++) {
        if (!TallyItemGet(s, end, &at, n->head.tree, prev, &key, &body, &len)) return 0;
        if ((i && !KeyBefore(prev, key)) || KeyBefore(key, n->head.first) || !RangeHolds(r, key)) return 0;
        prev = key;
    }
    return at >= end || s[at] == 0;
}

typedef enum { NODE_READ, NODE_ROTTEN, NODE_NO_ROOM } LecternNodeSays;

/*
 * The node `ptr` names into *n, from the first of its copies that holds as a
 * node of the range `r` (§2.5): a copy below the far copy of the Deed, its
 * blocks one SHEET unit sealed at the pointer's seq — a write that never
 * landed leaves the old node there with a sum that still holds — and its
 * stream a node of the range. READ, the copies before it that did not hold in
 * *failed; ROTTEN when none holds; NO_ROOM when the desk gives none, or the
 * node's stream is longer than this reader counts (u32).
 */
static LecternNodeSays LecternNodeRead(const LecternBook *b, const BookPtr *ptr, const LecternRange *r,
                                       LecternNode *n, uint32_t *failed)
{
    const LecternDesk *d  = b->desk;
    uint32_t           at = 0;
    BookSectorHead     head;

    *failed = 0;
    if (ptr->sheets > 0xFFFFFFFFu / TALLY_SHEET_PAYLOAD) return NODE_NO_ROOM;
    for (uint64_t k = 0; k < ptr->copies; k++, (*failed)++) {
        uint64_t block = 0;
        at += BookVarintGet(ptr->blocks + at, ptr->blocks_bytes - at, &block);
        if (block >= d->block_end || ptr->sheets > d->block_end - block) continue;
        LecternUnitSays u = LecternUnit(d, block, ptr->sheets, BOOK_UNIT_SHEET, ptr->seq, &n->room, &head);
        if (u == UNIT_NO_ROOM) return NODE_NO_ROOM;
        if (u != UNIT_HOLDS) continue;
        n->bytes  = ptr->sheets * VOLUME_BLOCK_BYTES;
        n->stream = (uint32_t)(ptr->sheets * TALLY_SHEET_PAYLOAD);
        if (LecternNodeHolds(n, r)) return NODE_READ;
        NodeGive(d, n);
    }
    return NODE_ROTTEN;
}

/* "the DIGESTS node of level L at block(s) a/b: N of its copies do not hold —
 * copy N+1 is read": what a node read past a copy that did not hold says —
 * the next Tally the kernel writes lays it again (§2.5). */
static void LecternCopiesSay(const LecternBook *b, uint64_t level, const BookPtr *ptr, uint32_t failed)
{
    LecternLine l;
    uint32_t    at = 0;

    LineStart(&l);
    LinePut(&l, "the DIGESTS node of level ");
    LineNum(&l, level);
    LinePut(&l, " at block(s) ");
    for (uint64_t k = 0; k < ptr->copies; k++) {
        uint64_t block = 0;
        at += BookVarintGet(ptr->blocks + at, ptr->blocks_bytes - at, &block);
        if (k) LinePut(&l, "/");
        LineNum(&l, block);
    }
    LinePut(&l, ": ");
    LineNum(&l, failed);
    LinePut(&l, " of its copies do not hold — copy ");
    LineNum(&l, failed + 1u);
    LinePut(&l, " is read");
    LineSay(b->desk, &l);
}

/* ------------------------------------------------------------------------- */
/* The slot stood on (§1.4)                                                  */
/* ------------------------------------------------------------------------- */

/* A slot's stream walked to its end by the one rule (volume_spine.h): whether
 * it is whole. */
static int LecternSlotWhole(const uint8_t *stream)
{
    SpineWalk w;
    SpineSays says;

    LecternZero(&w, sizeof(w));
    do says = SpineSlotStep(stream, TALLY_SHEET_PAYLOAD, &w);
    while (!SpineSaysTorn(says) && says != SPINE_SAYS_WHOLE);
    return says == SPINE_SAYS_WHOLE;
}

/* Slot `index` read and judged: HOLDS with its stream in *room and its seq in
 * *seq when its sectors are one SLOT unit and its stream is whole; BLANK;
 * TORN, said in the kernel's words; NO_ROOM. */
static LecternUnitSays LecternSlot(const LecternBook *b, uint32_t index, uint8_t **room, uint64_t *seq)
{
    const LecternDesk *d     = b->desk;
    uint64_t           block = d->frame.spine_block + (uint64_t)index * d->frame.spine_stride;
    BookSectorHead     head;
    LecternUnitSays    u = LecternUnit(d, block, 1, BOOK_UNIT_SLOT, 0, room, &head);
    LecternLine        l;

    if (u == UNIT_HOLDS && LecternSlotWhole(*room)) {
        *seq = head.seq;
        return UNIT_HOLDS;
    }
    if (u == UNIT_HOLDS) {
        d->give(d->ctx, *room, VOLUME_BLOCK_BYTES);
        *room = (uint8_t *)0;
        u     = UNIT_TORN;
    }
    if (u == UNIT_TORN) {
        LineStart(&l);
        LinePut(&l, "Spine slot ");
        LineNum(&l, index);
        LinePut(&l, " at block ");
        LineNum(&l, block);
        LinePut(&l, " does not hold — the book is read off another");
        LineSay(d, &l);
    }
    return u;
}

/* The DIGESTS root, read, made the top of the path: rungs for its level and
 * every one below. 0 when the desk gives no room for them, the root given
 * back. */
static int LecternTop(LecternBook *b, const SpineRoot *root, LecternNode *node, uint32_t failed)
{
    const LecternDesk *d = b->desk;
    uint64_t           n = root->level + 1u;

    if (!n || n > ~(uint64_t)0 / sizeof(LecternRung) ||
        !(b->rungs = (LecternRung *)(void *)d->take(d->ctx, n * sizeof(LecternRung)))) {
        NodeGive(d, node);
        return 0;
    }
    LecternZero(b->rungs, n * sizeof(LecternRung));
    b->top                = root->level;
    b->rungs[b->top].node = *node;
    b->root_ptr           = root->ptr;
    b->root_failed        = failed;
    return 1;
}

/* Every rung given back. */
static void LecternPathGive(LecternBook *b)
{
    if (!b->rungs) return;
    for (uint64_t l = 0; l <= b->top; l++) NodeGive(b->desk, &b->rungs[l].node);
    b->desk->give(b->desk->ctx, (uint8_t *)(void *)b->rungs, (b->top + 1u) * sizeof(LecternRung));
    b->rungs = (LecternRung *)0;
}

/* "slot i (seq S) names a Tally that does not read: its T root will not read
 * — the next slot is read" (book_mount.c, BookStand). */
static void LecternFallSay(const LecternDesk *d, uint32_t index, uint64_t seq, uint64_t tree)
{
    LecternLine l;
    LineStart(&l);
    LinePut(&l, "slot ");
    LineNum(&l, index);
    LinePut(&l, " (seq ");
    LineNum(&l, seq);
    LinePut(&l, ") names a Tally that does not read: its ");
    LinePut(&l, g_tree_names[tree]);
    LinePut(&l, " root will not read — the next slot is read");
    LineSay(d, &l);
}

/*
 * Whether the Tally a whole slot's `stream` names reads — every authoritative
 * root (SpineTreeAuthoritative) in some copy, the node there of the tree and
 * level the root says (book_mount.c BookSlotReads, tally_walk.c
 * TallyRootReads) — the DIGESTS root kept as the path's top when it does: 1;
 * 0 said; -1 no room.
 */
static int LecternTallyReads(LecternBook *b, uint32_t index, uint64_t seq, const uint8_t *stream)
{
    const LecternDesk *d    = b->desk;
    LecternRange       none = { { 0, 0 }, { 0, 0 }, 0, 0 };
    LecternNode        node;
    SpineWalk          w;
    SpineSays          says;
    uint32_t           failed;

    LecternZero(&w, sizeof(w));
    LecternZero(&node, sizeof(node));
    do {
        says = SpineSlotStep(stream, TALLY_SHEET_PAYLOAD, &w);
        if ((says != SPINE_SAYS_RECORD && says != SPINE_SAYS_LONGER) || w.kind != SPINE_ROOT ||
            !SpineTreeAuthoritative(&w.root))
            continue;
        LecternNodeSays n = LecternNodeRead(b, &w.root.ptr, &none, &node, &failed);
        if (n == NODE_READ && (node.head.tree != w.root.tree || node.head.level != w.root.level)) {
            NodeGive(d, &node);
            n = NODE_ROTTEN;
        }
        if (n != NODE_READ) {
            LecternPathGive(b);
            if (n == NODE_NO_ROOM) return -1;
            LecternFallSay(d, index, seq, w.root.tree);
            return 0;
        }
        if (w.root.tree != TALLY_DIGESTS)
            NodeGive(d, &node);
        else if (!LecternTop(b, &w.root, &node, failed))
            return -1;
    } while (!SpineSaysTorn(says) && says != SPINE_SAYS_WHOLE);
    return 1;
}

/* Slot `index` (seq `seq`) tried: read again, and its Tally's roots: 1 stood
 * on, said with the DIGESTS root's copies that did not hold; 0 passed over,
 * said; -1 no room. */
static int LecternTry(LecternBook *b, uint32_t index, uint64_t seq)
{
    const LecternDesk *d    = b->desk;
    uint8_t           *room = (uint8_t *)0;
    uint64_t           again = 0;
    LecternUnitSays    u    = LecternSlot(b, index, &room, &again);
    LecternLine        l;
    int                rc;

    if (u == UNIT_NO_ROOM) return -1;
    if (u != UNIT_HOLDS) return 0;
    rc = again == seq ? LecternTallyReads(b, index, seq, room) : 0;
    if (rc == 1) {
        LineStart(&l);
        LinePut(&l, "the book stands on slot ");
        LineNum(&l, index);
        LinePut(&l, " (seq ");
        LineNum(&l, seq);
        LinePut(&l, ")");
        LineSay(d, &l);
        if (b->rungs && b->root_failed) LecternCopiesSay(b, b->top, &b->root_ptr, b->root_failed);
    }
    d->give(d->ctx, room, VOLUME_BLOCK_BYTES);
    return rc;
}

/* The slots judged in the ring's order, those that hold tried newest first
 * (SpineSlotNewer), the first whose Tally reads stood on: 1; 0 with why none
 * is in *why; -1 no room. */
static int LecternStand(LecternBook *b, LecternLine *why)
{
    const LecternDesk *d     = b->desk;
    uint32_t           slots = d->frame.spine_slots, held = 0;
    uint64_t          *seqs  = (uint64_t *)(void *)d->take(d->ctx, (uint64_t)slots * sizeof(uint64_t));
    int                rc    = 0;

    if (!seqs) return -1;
    for (uint32_t i = 0; i < slots && rc == 0; i++) {
        uint8_t        *room = (uint8_t *)0;
        LecternUnitSays u    = LecternSlot(b, i, &room, &seqs[i]);
        if (u != UNIT_HOLDS) seqs[i] = 0;
        if (u == UNIT_NO_ROOM) rc = -1;
        if (u == UNIT_HOLDS) {
            held++;
            d->give(d->ctx, room, VOLUME_BLOCK_BYTES);
        }
    }
    if (rc == 0 && held == 0) {
        LinePut(why, "none of the ");
        LineNum(why, slots);
        LinePut(why, " slots of its Spine holds");
    }
    while (rc == 0 && held > 0) {
        uint32_t best = slots;
        for (uint32_t j = 0; j < slots; j++)
            if (seqs[j] && (best == slots || SpineSlotNewer(seqs[j], j, seqs[best], best))) best = j;
        if (best == slots) {
            LinePut(why, "no slot that holds names a Tally that reads");
            break;
        }
        rc         = LecternTry(b, best, seqs[best]);
        seqs[best] = 0;
    }
    d->give(d->ctx, (uint8_t *)(void *)seqs, (uint64_t)slots * sizeof(uint64_t));
    return rc;
}

/* ------------------------------------------------------------------------- */
/* A block's digest, down the DIGESTS tree (§2.9)                            */
/* ------------------------------------------------------------------------- */

typedef enum { DIGEST_NAMED, DIGEST_UNNAMED, DIGEST_ROTTEN, DIGEST_NO_ROOM } LecternDigestSays;

static int RungCovers(const LecternRung *r, uint64_t v)
{
    TallyKey key = { v, 0 };
    return (r->node.room || r->rotten) && RangeHolds(&r->range, key);
}

/*
 * From the inner node at rung `level` down one, to the child whose range holds
 * block `v` — the last item whose key is not past it, its range reaching to
 * the next item's key or to the node's own end (tally_walk.c TallyInnerKids)
 * — every rung below given back first. A child whose pointer does not read,
 * or that reads in no copy, or is another tree's or level's, is its range
 * rotten (§4.5 п.3). UNNAMED for a block before every child; NO_ROOM; NAMED
 * otherwise, the child at rung level - 1.
 */
static LecternDigestSays LecternDescend(LecternBook *b, uint64_t level, uint64_t v)
{
    const LecternDesk *d   = b->desk;
    LecternNode       *n   = &b->rungs[level].node;
    LecternRung       *kid = &b->rungs[level - 1u];
    LecternRange       r   = b->rungs[level].range;
    uint32_t           at = n->items_at, body, len, kbody = 0, klen = 0, failed;
    TallyKey           prev = n->head.first, key;
    int                chosen = 0;
    BookPtr            ptr;
    uint64_t           count;

    for (uint64_t i = 0; i < n->head.items; i++) {
        TallyItemGet(n->room, n->stream, &at, TALLY_DIGESTS, prev, &key, &body, &len);
        prev = key;
        if (key.k1 > v) {
            r.hi     = key;
            r.has_hi = 1;
            break;
        }
        chosen   = 1;
        r.lo     = key;
        r.has_lo = 1;
        kbody    = body;
        klen     = len;
    }
    if (!chosen) return DIGEST_UNNAMED;
    for (uint64_t l = 0; l < level; l++) {
        NodeGive(d, &b->rungs[l].node);
        b->rungs[l].rotten = 0;
    }
    kid->range  = r;
    kid->rotten = 1;
    if (TallyInnerGet(n->room + kbody, klen, &count, &ptr) == TALLY_BODY_BROKEN) return DIGEST_NAMED;
    LecternNodeSays s = LecternNodeRead(b, &ptr, &r, &kid->node, &failed);
    if (s == NODE_NO_ROOM) return DIGEST_NO_ROOM;
    if (s == NODE_READ && (kid->node.head.tree != TALLY_DIGESTS || kid->node.head.level != level - 1u))
        NodeGive(d, &kid->node);
    else if (s == NODE_READ)
        kid->rotten = 0;
    if (!kid->rotten && failed) LecternCopiesSay(b, level - 1u, &ptr, failed);
    return DIGEST_NAMED;
}

/* The digest of volume block `v` in *digest: the path held reused from the
 * lowest rung whose range holds `v`, walked down from there to the leaf, the
 * leaf's elements walked in key order (volume_tally.h, TallyDigestIn).
 * ROTTEN with the range that does not read in *rot. */
static LecternDigestSays LecternDigestOf(LecternBook *b, uint64_t v, uint64_t *digest, LecternRange *rot)
{
    uint64_t l = 0;

    if (!b->rungs) return DIGEST_UNNAMED;
    while (l < b->top && !RungCovers(&b->rungs[l], v)) l++;
    for (;; l--) {
        if (b->rungs[l].rotten) {
            *rot = b->rungs[l].range;
            return DIGEST_ROTTEN;
        }
        if (l == 0) break;
        LecternDigestSays s = LecternDescend(b, l, v);
        if (s != DIGEST_NAMED) return s;
    }
    int found = TallyDigestIn(b->rungs[0].node.room, b->rungs[0].node.stream, v, digest);
    if (found < 0) *rot = b->rungs[0].range;
    return found < 0 ? DIGEST_ROTTEN : found ? DIGEST_NAMED : DIGEST_UNNAMED;
}

/* ------------------------------------------------------------------------- */
/* Attest                                                                    */
/* ------------------------------------------------------------------------- */

/* Why the kernel's block `k` (volume block `v`) is held to nothing. */
static void LecternWhy(LecternLine *why, LecternDigestSays s, uint64_t k, uint64_t v, const LecternRange *rot)
{
    if (s == DIGEST_NO_ROOM) {
        LinePut(why, "this loader was given no room to read its book");
        return;
    }
    LinePut(why, s == DIGEST_UNNAMED ? "its Tally names no digest of its block " : "its block ");
    LineNum(why, k);
    LinePut(why, " (volume block ");
    LineNum(why, v);
    LinePut(why, ")");
    if (s == DIGEST_UNNAMED) return;
    LinePut(why, " lies in the DIGESTS range of blocks [");
    LineNum(why, rot->has_lo ? rot->lo.k1 : 0);
    LinePut(why, ", ");
    if (rot->has_hi)
        LineNum(why, rot->hi.k1);
    else
        LinePut(why, "the end");
    LinePut(why, "), which does not read");
}

/* The kernel's blocks in order, each held to its digest: BROKEN at the first
 * that does not match, *v->block it; else UNCHECKED with the first block held
 * to nothing, why in *why; else HELD. */
static AttestSays LecternBlocks(LecternBook *b, uint64_t first, uint64_t blocks, const uint8_t *image,
                                LecternLine *why, AttestVerdict *v)
{
    AttestSays says = ATTEST_HELD;

    for (uint64_t k = 0; k < blocks; k++) {
        uint64_t          digest = 0;
        LecternRange      rot;
        LecternZero(&rot, sizeof(rot));
        LecternDigestSays s = LecternDigestOf(b, first + k, &digest, &rot);
        if (s == DIGEST_NAMED && WyHash64(image + k * VOLUME_BLOCK_BYTES, VOLUME_BLOCK_BYTES, b->lane) != digest) {
            v->block = k;
            return ATTEST_BROKEN;
        }
        if (s == DIGEST_NAMED || says == ATTEST_UNCHECKED) continue;
        says     = ATTEST_UNCHECKED;
        v->block = k;
        LecternWhy(why, s, k, first + k, &rot);
        if (s == DIGEST_NO_ROOM) break;
    }
    return says;
}

void LecternAttest(const LecternDesk *desk, const char *who, uint64_t first, uint64_t blocks,
                   const uint8_t *image, AttestVerdict *verdict)
{
    LecternBook b;
    LecternLine why, l;
    uint64_t    lanes[4];

    LecternZero(&b, sizeof(b));
    b.desk = desk;
    WySeedLanes(desk->uuid, 16u, lanes);
    b.lane = lanes[0];
    LineStart(&why);
    verdict->block = 0;
    int stood = LecternStand(&b, &why);
    if (stood < 0) LinePut(&why, "this loader was given no room to read its book");
    verdict->says = stood == 1 ? LecternBlocks(&b, first, blocks, image, &why, verdict) : ATTEST_UNCHECKED;
    LecternPathGive(&b);

    LineStart(&l);
    LinePut(&l, who);
    if (verdict->says == ATTEST_HELD) {
        LinePut(&l, " is attested by the book: ");
        LineNum(&l, blocks);
        LinePut(&l, " block(s)");
    } else if (verdict->says == ATTEST_BROKEN) {
        LinePut(&l, " does not match the book: block ");
        LineNum(&l, verdict->block);
        LinePut(&l, " (volume block ");
        LineNum(&l, first + verdict->block);
        LinePut(&l, ")");
    } else {
        LinePut(&l, " is not attested: ");
        LinePut(&l, why.text);
    }
    for (uint32_t i = 0; i <= l.at; i++) verdict->line[i] = l.text[i];
    LineSay(desk, &l);
}
