/*
 * Gangway's Lectern — a volume's book read the way the kernel's
 * mount stands on it, and Attest: a kernel's blocks held to the digests the
 * book keeps of them before the loader jumps (gangway_lectern.h).
 *
 * What it holds at once is a few nodes: the slot stood on, and the path down
 * the DIGESTS tree to the leaf that names the block in hand — one rung a
 * level, each rung kept while the blocks in hand stay in its range. A
 * kernel's blocks lie in rows, so every node on their paths is read once, and
 * a copy of a node that did not hold is said once, whichever kernel's block
 * reached it.
 */

#include "gangway_lectern_book.h"

/* ------------------------------------------------------------------------- */
/* Lines, memory and bytes, with no library under them                       */
/* ------------------------------------------------------------------------- */

void LineStart(LecternLine *l)
{
    l->at      = 0;
    l->text[0] = '\0';
}

void LinePut(LecternLine *l, const char *s)
{
    while (*s && l->at + 1u < LECTERN_LINE_BYTES) l->text[l->at++] = *s++;
    l->text[l->at] = '\0';
}

void LineNum(LecternLine *l, uint64_t v)
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

void LecternSay(const LecternBook *b, const LecternLine *l)
{
    b->desk->say(b->desk->ctx, l->text);
}

/* Bytes moved and cleared through volatile pointers: never turned into a call
 * to a memcpy or memset the loader has not (its image links with -z defs). */
void LecternZero(void *p, uint64_t n)
{
    volatile uint8_t *to = (volatile uint8_t *)p;
    while (n--) *to++ = 0;
}

void LecternCopy(void *to, const void *from, uint64_t n)
{
    volatile uint8_t *t = (volatile uint8_t *)to;
    const uint8_t    *f = (const uint8_t *)from;
    while (n--) *t++ = *f++;
}

void *LecternTake(LecternBook *b, uint64_t bytes)
{
    void *room = b->starved ? (void *)0 : b->desk->take(b->desk->ctx, bytes);
    if (!room) b->starved = 1;
    return room;
}

void LecternGive(const LecternBook *b, void *room, uint64_t bytes)
{
    if (room) b->desk->give(b->desk->ctx, (uint8_t *)room, bytes);
}

/* Room in the array *at of `n` elements of `size` for one more, twice the room
 * each time it runs out: 1, or 0 no room (the array as it was). */
int LecternGrow(LecternBook *b, void **at, uint64_t n, uint64_t *cap, uint64_t size)
{
    uint64_t more = *cap ? *cap * 2u : 8u;
    void    *to;
    if (n < *cap) return 1;
    if (more > ~(uint64_t)0 / size || !(to = LecternTake(b, more * size))) {
        b->starved = 1;
        return 0;
    }
    if (n) LecternCopy(to, *at, n * size);
    LecternGive(b, *at, *cap * size);
    *at  = to;
    *cap = more;
    return 1;
}

static uint64_t ListAt(const uint64_t *list, uint64_t n, uint64_t v)
{
    uint64_t lo = 0, hi = n;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2u;
        if (list[mid] < v) lo = mid + 1u;
        else hi = mid;
    }
    return lo;
}

int LecternListHas(const uint64_t *list, uint64_t n, uint64_t v)
{
    uint64_t at = ListAt(list, n, v);
    return at < n && list[at] == v;
}

/* `v` into a rising list, once: 1, or 0 no room. */
int LecternListAdd(LecternBook *b, uint64_t **list, uint64_t *n, uint64_t *cap, uint64_t v)
{
    uint64_t at = ListAt(*list, *n, v);
    if (at < *n && (*list)[at] == v) return 1;
    if (!LecternGrow(b, (void **)list, *n, cap, sizeof(uint64_t))) return 0;
    for (uint64_t i = *n; i > at; i--) (*list)[i] = (*list)[i - 1u];
    (*list)[at] = v;
    (*n)++;
    return 1;
}

void LecternSectors(LecternBook *b, uint64_t vlba, uint32_t sectors, uint8_t *into)
{
    const LecternDesk *d = b->desk;
    for (uint32_t at = 0; at < sectors;) {
        uint32_t n    = sectors - at < LECTERN_READ_MOST ? sectors - at : LECTERN_READ_MOST;
        uint8_t *part = into + (uint64_t)at * BOOK_SECTOR_BYTES;
        if (!d->read(d->ctx, vlba + at, n, part))
            for (uint32_t k = 0; k < n; k++)
                if (!d->read(d->ctx, vlba + at + k, 1, part + (uint64_t)k * BOOK_SECTOR_BYTES))
                    LecternZero(part + (uint64_t)k * BOOK_SECTOR_BYTES, BOOK_SECTOR_BYTES);
        at += n;
    }
}

uint64_t LecternRunRead(const LecternBook *b, int hole, uint64_t v, uint64_t count, uint8_t *into)
{
    const LecternDesk *d    = b->desk;
    const uint64_t     most = LECTERN_READ_MOST / VOLUME_BLOCK_SECTORS;
    uint64_t           done = 0;
    if (hole) {
        LecternZero(into, count * VOLUME_BLOCK_BYTES);
        return count;
    }
    while (done < count) {
        uint64_t n = count - done < most ? count - done : most;
        if (d->read(d->ctx, (v + done) * VOLUME_BLOCK_SECTORS, (uint32_t)(n * VOLUME_BLOCK_SECTORS),
                    into + done * VOLUME_BLOCK_BYTES)) {
            done += n;
            continue;
        }
        for (uint64_t end = done + n; done < end; done++)
            if (!d->read(d->ctx, (v + done) * VOLUME_BLOCK_SECTORS, VOLUME_BLOCK_SECTORS,
                         into + done * VOLUME_BLOCK_BYTES))
                return done;
    }
    return done;
}

void LecternTailLaid(const uint8_t *tail, uint8_t *block)
{
    for (uint32_t i = 0; tail && i < VOLUME_BLOCK_BYTES; i++)
        if (tail[VOLUME_BLOCK_BYTES + i / 8u] & (1u << (i % 8u))) block[i] = tail[i];
}

static int LecternZeros(const uint8_t *p, uint64_t n)
{
    for (uint64_t i = 0; i < n; i++)
        if (p[i]) return 0;
    return 1;
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
static LecternUnitSays LecternUnit(LecternBook *b, uint64_t block, uint64_t blocks, uint32_t kind, uint64_t seq,
                                   uint8_t **room, BookSectorHead *head)
{
    const LecternDesk *d       = b->desk;
    uint64_t           bytes   = blocks * VOLUME_BLOCK_BYTES;
    uint32_t           sectors = (uint32_t)(blocks * VOLUME_BLOCK_SECTORS);
    uint8_t           *r       = (uint8_t *)LecternTake(b, bytes);
    LecternUnitSays    says    = UNIT_TORN;

    *room = (uint8_t *)0;
    if (!r) return UNIT_NO_ROOM;
    if (LecternRunRead(b, 0, block, blocks, r) == blocks) {
        if (LecternZeros(r, bytes))
            says = UNIT_BLANK;
        else if (BookUnitCheck(r, sectors, d->uuid, block * VOLUME_BLOCK_SECTORS, head) && head->kind == kind &&
                 (!seq || head->seq == seq))
            says = UNIT_HOLDS;
    }
    if (says != UNIT_HOLDS) {
        LecternGive(b, r, bytes);
        return says;
    }
    BookUnitStream(r, sectors, r);
    *room = r;
    return UNIT_HOLDS;
}

void LecternNodeGive(const LecternBook *b, LecternNode *n)
{
    LecternGive(b, n->room, n->bytes);
    n->room = (uint8_t *)0;
}

static const char *TreeName(uint64_t tree)
{
    static const char *const names[] = { "", "FILES", "TAGS", "USE", "BOOKMARKS", "LIEN", "DIGESTS" };
    return tree <= TALLY_DIGESTS ? names[tree] : "";
}

/* "the T node of level L at block(s) a/b: N of its copies do not hold — copy
 * N+1 is read": what a node read past a copy that did not hold says — the
 * next Tally the kernel writes lays it again (§2.5) — once a node. */
void LecternCopiesSay(LecternBook *b, uint64_t tree, uint64_t level, const BookPtr *ptr, uint32_t failed)
{
    LecternLine l;
    uint32_t    at    = 0;
    uint64_t    first = 0;

    BookVarintGet(ptr->blocks, ptr->blocks_bytes, &first);
    if (!failed || LecternListHas(b->said, b->nsaid, first) ||
        !LecternListAdd(b, &b->said, &b->nsaid, &b->capsaid, first))
        return;
    LineStart(&l);
    LinePut(&l, "the ");
    LinePut(&l, TreeName(tree));
    LinePut(&l, " node of level ");
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
    LecternSay(b, &l);
}

/*
 * A copy is the node when it lies below the far copy of the Deed, its blocks
 * are one SHEET unit sealed at the pointer's seq — a write that never landed
 * leaves the old node there with a sum that still holds — and its stream holds
 * as a node of its range by the one rule (volume_tally.h, TallyNodeJudge). The
 * first that is, of the tree and level asked, is read; one of another tree or
 * level is the range rotten, as the kernel's walk takes it (tally_walk.c) and
 * volume-read's (tally.py TreeRead). The copies before it that did not hold
 * in *failed. The node too long for this reader to count (u32) is no room.
 */
LecternNodeSays LecternNodeRead(LecternBook *b, uint64_t tree, uint64_t level, const BookPtr *ptr,
                                const TallyRange *r, LecternNode *n, uint32_t *failed)
{
    uint64_t        end = b->desk->block_end;
    uint32_t        at  = 0;
    BookSectorHead  head;
    TallyNodeJudged j;

    *failed = 0;
    if (ptr->sheets > 0xFFFFFFFFu / TALLY_SHEET_PAYLOAD) return NODE_NO_ROOM;
    for (uint64_t k = 0; k < ptr->copies; k++, (*failed)++) {
        uint64_t block = 0;
        at += BookVarintGet(ptr->blocks + at, ptr->blocks_bytes - at, &block);
        if (block >= end || ptr->sheets > end - block) continue;
        LecternUnitSays u = LecternUnit(b, block, ptr->sheets, BOOK_UNIT_SHEET, ptr->seq, &n->room, &head);
        if (u == UNIT_NO_ROOM) return NODE_NO_ROOM;
        if (u != UNIT_HOLDS) continue;
        n->bytes  = ptr->sheets * VOLUME_BLOCK_BYTES;
        n->stream = (uint32_t)(ptr->sheets * TALLY_SHEET_PAYLOAD);
        n->seq    = ptr->seq;
        if (TallyNodeJudge(n->room, n->stream, r, &j) != TALLY_NODE_HOLDS) {
            LecternNodeGive(b, n);
            continue;
        }
        n->head     = j.head;
        n->items_at = j.items_at;
        if (n->head.tree != tree || n->head.level != level) {
            LecternNodeGive(b, n);
            return NODE_ROTTEN;
        }
        return NODE_READ;
    }
    return NODE_ROTTEN;
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
static LecternUnitSays LecternSlot(LecternBook *b, uint32_t index, uint8_t **room, uint64_t *seq)
{
    const LecternDesk *d     = b->desk;
    uint64_t           block = d->frame.spine_block + (uint64_t)index * d->frame.spine_stride;
    BookSectorHead     head;
    LecternUnitSays    u = LecternUnit(b, block, 1, BOOK_UNIT_SLOT, 0, room, &head);
    LecternLine        l;

    if (u == UNIT_HOLDS && LecternSlotWhole(*room)) {
        *seq = head.seq;
        return UNIT_HOLDS;
    }
    if (u == UNIT_HOLDS) {
        LecternGive(b, *room, VOLUME_BLOCK_BYTES);
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
        LecternSay(b, &l);
    }
    return u;
}

/* The DIGESTS root, read, made the top of the path: rungs for its level and
 * every one below. 0 when the desk gives no room for them, the root given
 * back. */
static int LecternTop(LecternBook *b, const SpineRoot *root, LecternNode *node)
{
    uint64_t n = root->level + 1u;

    if (!n || n > ~(uint64_t)0 / sizeof(LecternRung) ||
        !(b->rungs = (LecternRung *)LecternTake(b, n * sizeof(LecternRung)))) {
        LecternNodeGive(b, node);
        return 0;
    }
    LecternZero(b->rungs, n * sizeof(LecternRung));
    b->top                = root->level;
    b->rungs[b->top].node = *node;
    return 1;
}

/* Every rung given back. */
static void LecternPathGive(LecternBook *b)
{
    if (!b->rungs) return;
    for (uint64_t l = 0; l <= b->top; l++) LecternNodeGive(b, &b->rungs[l].node);
    LecternGive(b, b->rungs, (b->top + 1u) * sizeof(LecternRung));
    b->rungs = (LecternRung *)0;
}

/* "slot i (seq S) names a Tally that does not read: its T root will not read
 * — the next slot is read" (book_mount.c, BookStand). */
static void LecternFallSay(const LecternBook *b, uint32_t index, uint64_t seq, uint64_t tree)
{
    LecternLine l;
    LineStart(&l);
    LinePut(&l, "slot ");
    LineNum(&l, index);
    LinePut(&l, " (seq ");
    LineNum(&l, seq);
    LinePut(&l, ") names a Tally that does not read: its ");
    LinePut(&l, TreeName(tree));
    LinePut(&l, " root will not read — the next slot is read");
    LecternSay(b, &l);
}

/*
 * Whether the Tally a whole slot's `stream` names reads — every authoritative
 * root (SpineTreeAuthoritative) in some copy, the node there of the tree and
 * level the root says (book_mount.c BookSlotReads, tally_walk.c
 * TallyRootReads) — its roots, head and ribbon kept, the DIGESTS root as the
 * path's top and its copies that did not hold in *digests, when it does: 1;
 * 0 said; -1 no room. Nothing is said of a copy of a root here: a slot passed
 * over is no book read, and FILES and TAGS are said as they are walked.
 */
static int LecternTallyReads(LecternBook *b, uint32_t index, uint64_t seq, const uint8_t *stream,
                             uint32_t *digests)
{
    TallyRange  none = { { 0, 0 }, { 0, 0 }, 0, 0 };
    LecternNode node;
    SpineWalk   w;
    SpineSays   says;
    uint32_t    failed;

    LecternZero(&w, sizeof(w));
    LecternZero(&node, sizeof(node));
    b->rooted = 0;
    do {
        says = SpineSlotStep(stream, TALLY_SHEET_PAYLOAD, &w);
        if ((says != SPINE_SAYS_RECORD && says != SPINE_SAYS_LONGER) || w.kind != SPINE_ROOT ||
            !SpineTreeAuthoritative(&w.root))
            continue;
        LecternNodeSays n = LecternNodeRead(b, w.root.tree, w.root.level, &w.root.ptr, &none, &node, &failed);
        if (n != NODE_READ) {
            LecternPathGive(b);
            if (n == NODE_NO_ROOM) return -1;
            LecternFallSay(b, index, seq, w.root.tree);
            return 0;
        }
        b->roots[w.root.tree] = w.root;
        b->rooted |= 1u << w.root.tree;
        if (w.root.tree != TALLY_DIGESTS)
            LecternNodeGive(b, &node);
        else if (!LecternTop(b, &w.root, &node))
            return -1;
        else
            *digests = failed;
    } while (!SpineSaysTorn(says) && says != SPINE_SAYS_WHOLE);
    b->head   = w.head;
    b->ribbon = w.ribbon;
    return 1;
}

/* Slot `index` (seq `seq`) tried: read again, and its Tally's roots: 1 stood
 * on, said with the DIGESTS root's copies that did not hold, its stream kept;
 * 0 passed over, said; -1 no room. */
static int LecternTry(LecternBook *b, uint32_t index, uint64_t seq)
{
    uint8_t        *room  = (uint8_t *)0;
    uint64_t        again = 0;
    uint32_t        digests = 0;
    LecternUnitSays u     = LecternSlot(b, index, &room, &again);
    LecternLine     l;
    int             rc;

    if (u == UNIT_NO_ROOM) return -1;
    if (u != UNIT_HOLDS) return 0;
    rc = again == seq ? LecternTallyReads(b, index, seq, room, &digests) : 0;
    if (rc != 1) {
        LecternGive(b, room, VOLUME_BLOCK_BYTES);
        return rc;
    }
    LineStart(&l);
    LinePut(&l, "the book stands on slot ");
    LineNum(&l, index);
    LinePut(&l, " (seq ");
    LineNum(&l, seq);
    LinePut(&l, ")");
    b->stood      = 1;
    b->slot_room  = room;
    b->slot_index = index;
    b->slot_seq   = seq;
    LecternSay(b, &l);
    if (b->rungs) LecternCopiesSay(b, TALLY_DIGESTS, b->top, &b->roots[TALLY_DIGESTS].ptr, digests);
    return 1;
}

/* The slots judged in the ring's order, those that hold tried newest first
 * (SpineSlotNewer), the first whose Tally reads stood on. */
int LecternStand(LecternBook *b)
{
    uint32_t  slots = b->desk->frame.spine_slots, held = 0;
    uint64_t *seqs  = (uint64_t *)LecternTake(b, (uint64_t)slots * sizeof(uint64_t));
    int       rc    = 0;

    if (!seqs) return -1;
    for (uint32_t i = 0; i < slots && rc == 0; i++) {
        uint8_t        *room = (uint8_t *)0;
        LecternUnitSays u    = LecternSlot(b, i, &room, &seqs[i]);
        if (u != UNIT_HOLDS) seqs[i] = 0;
        if (u == UNIT_NO_ROOM) rc = -1;
        if (u == UNIT_HOLDS) {
            held++;
            LecternGive(b, room, VOLUME_BLOCK_BYTES);
        }
    }
    LineStart(&b->why);
    if (rc == 0 && held == 0) {
        LinePut(&b->why, "none of the ");
        LineNum(&b->why, slots);
        LinePut(&b->why, " slots of its Spine holds");
    }
    while (rc == 0 && held > 0) {
        uint32_t best = slots;
        for (uint32_t j = 0; j < slots; j++)
            if (seqs[j] && (best == slots || SpineSlotNewer(seqs[j], j, seqs[best], best))) best = j;
        if (best == slots) {
            LinePut(&b->why, "no slot that holds names a Tally that reads");
            break;
        }
        rc         = LecternTry(b, best, seqs[best]);
        seqs[best] = 0;
    }
    if (rc < 0) LinePut(&b->why, "this loader was given no room to read its book");
    LecternGive(b, seqs, (uint64_t)slots * sizeof(uint64_t));
    return rc;
}

/* ------------------------------------------------------------------------- */
/* A block's digest, down the DIGESTS tree (§2.9)                            */
/* ------------------------------------------------------------------------- */

typedef enum { DIGEST_NAMED, DIGEST_UNNAMED, DIGEST_ROTTEN, DIGEST_NO_ROOM } LecternDigestSays;

static int RungCovers(const LecternRung *r, uint64_t v)
{
    TallyKey key = { v, 0 };
    return (r->node.room || r->rotten) && TallyRangeHolds(&r->range, key);
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
    LecternNode *n   = &b->rungs[level].node;
    LecternRung *kid = &b->rungs[level - 1u];
    TallyRange   r   = b->rungs[level].range;
    uint32_t     at = n->items_at, body, len, kbody = 0, klen = 0;
    TallyKey     prev = n->head.first, key;
    int          chosen = 0;
    BookPtr      ptr;
    uint64_t     count;

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
        LecternNodeGive(b, &b->rungs[l].node);
        b->rungs[l].rotten = 0;
    }
    kid->range  = r;
    kid->rotten = 1;
    if (TallyInnerGet(n->room + kbody, klen, &count, &ptr) == TALLY_BODY_BROKEN) return DIGEST_NAMED;
    uint32_t        failed;
    LecternNodeSays s = LecternNodeRead(b, TALLY_DIGESTS, level - 1u, &ptr, &r, &kid->node, &failed);
    if (s == NODE_NO_ROOM) return DIGEST_NO_ROOM;
    kid->rotten = s != NODE_READ;
    if (!kid->rotten) LecternCopiesSay(b, TALLY_DIGESTS, level - 1u, &ptr, failed);
    return DIGEST_NAMED;
}

/* The digest of volume block `v` in *digest: the path held reused from the
 * lowest rung whose range holds `v`, walked down from there to the leaf, the
 * leaf's elements walked in key order (volume_tally.h, TallyDigestIn).
 * ROTTEN with the range that does not read in *rot. */
static LecternDigestSays LecternDigestOf(LecternBook *b, uint64_t v, uint64_t *digest, TallyRange *rot)
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
static void LecternWhy(LecternLine *why, LecternDigestSays s, uint64_t k, uint64_t v, const TallyRange *rot)
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

/* One block's verdict: 1 matches, 0 held to nothing (why said into *why the
 * first time), -1 does not match, -2 no room. */
static int LecternBlockHeld(LecternBook *b, uint64_t k, uint64_t v, const uint8_t *data, int overlay,
                            LecternLine *why)
{
    uint64_t          digest = 0;
    TallyRange        rot;
    LecternDigestSays s = DIGEST_NAMED;

    LecternZero(&rot, sizeof(rot));
    if (!overlay || !LecternOverlaid(b, v, &digest)) s = LecternDigestOf(b, v, &digest, &rot);
    if (s == DIGEST_NAMED) return WyHash64(data, VOLUME_BLOCK_BYTES, b->lane) == digest ? 1 : -1;
    if (!why->at) LecternWhy(why, s, k, v, &rot);
    return s == DIGEST_NO_ROOM ? -2 : 0;
}

/* The kernel's blocks in file order, each held to its digest — a hole holds
 * the zeros its run names: BROKEN at the first that does not match, the block
 * in *k and *v; else UNCHECKED with the first held to nothing, why in *why;
 * else HELD. */
static AttestSays LecternBlocks(LecternBook *b, const LecternRun *runs, uint64_t nruns, uint64_t blocks,
                                const uint8_t *image, int overlay, LecternLine *why, uint64_t *k, uint64_t *v)
{
    uint64_t at = 0;

    for (uint64_t r = 0; r < nruns && at < blocks; r++) {
        for (uint64_t i = 0; i < runs[r].count && at < blocks; i++, at++) {
            if (runs[r].hole) continue;
            int held = LecternBlockHeld(b, at, runs[r].start + i, image + at * VOLUME_BLOCK_BYTES, overlay, why);
            if (held == -1) {
                *k = at;
                *v = runs[r].start + i;
                return ATTEST_BROKEN;
            }
            if (held == -2) return ATTEST_UNCHECKED;
        }
    }
    return why->at ? ATTEST_UNCHECKED : ATTEST_HELD;
}

AttestSays LecternAttest(LecternBook *b, const char *who, const LecternRun *runs, uint64_t nruns,
                         uint64_t blocks, const uint8_t *image, int overlay, LecternLine *verdict)
{
    LecternLine why;
    uint64_t    k = 0, v = 0;
    AttestSays  says;

    LineStart(&why);
    if (b->stood)
        says = LecternBlocks(b, runs, nruns, blocks, image, overlay, &why, &k, &v);
    else {
        says = ATTEST_UNCHECKED;
        LinePut(&why, b->why.text);
    }
    LineStart(verdict);
    LinePut(verdict, who);
    if (says == ATTEST_HELD) {
        LinePut(verdict, " is attested by the book: ");
        LineNum(verdict, blocks);
        LinePut(verdict, " block(s)");
    } else if (says == ATTEST_BROKEN) {
        LinePut(verdict, " does not match the book: block ");
        LineNum(verdict, k);
        LinePut(verdict, " (volume block ");
        LineNum(verdict, v);
        LinePut(verdict, ")");
    } else {
        LinePut(verdict, " is not attested: ");
        LinePut(verdict, why.text);
    }
    LecternSay(b, verdict);
    return says;
}

/* ------------------------------------------------------------------------- */
/* Everything given back                                                     */
/* ------------------------------------------------------------------------- */

void LecternClose(LecternBook *b)
{
    LecternPathGive(b);
    for (uint64_t i = 0; i < b->nstitches; i++) LecternStitchGive(b, b->stitches[i]);
    for (uint64_t i = 0; i < b->nfiles; i++) LecternFileGive(b, &b->files[i]);
    LecternGive(b, b->stitches, b->capstitches * sizeof(LecternStitch *));
    LecternGive(b, b->files, b->capfiles * sizeof(LecternFile));
    LecternGive(b, b->said, b->capsaid * sizeof(uint64_t));
    LecternGive(b, b->skip, b->capskip * sizeof(LecternSkip));
    LecternGive(b, b->refused, b->caprefused * sizeof(uint64_t));
    LecternGive(b, b->interest, b->capinterest * sizeof(uint64_t));
    LecternGive(b, b->rotten, b->caprotten * sizeof(TallyRange));
    LecternGive(b, b->overlay, b->capoverlay * sizeof(LecternDigest));
    LecternGive(b, b->slot_room, VOLUME_BLOCK_BYTES);
    LecternZero(b, sizeof(*b));
}
