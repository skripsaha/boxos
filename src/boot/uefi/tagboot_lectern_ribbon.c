/*
 * The Lectern's ribbon (spec_v2 §16.7 п.5, §4.5 п.5): from the `at` of the
 * slot the book stands on, the stitches sewn on, one after another, by the
 * Clasp each names — rules (а)/(б)/(б′)/(в) — into b->stitches, for the
 * replay of the files followed (tagboot_lectern_replay.c).
 *
 * The rules are the kernel's pass 1 (mount/ribbon_collate.c, ribbon_unit.c)
 * and volume-read's (stitch_ribbon.py Ribbon, stitch.py Unit), held to one
 * answer by the census scenario; this is their reading for a loader, which
 * writes nothing: no direct scan past the end of the chain (§2.2) — a loader
 * gives no seq — and nothing said of the ribbon's shape, which the kernel's
 * mount says. A next_quire is taken where a Quire of the ribbon may lie — in
 * the data run, begun on an atom of the volume — and not held against the
 * ground of the book and the files, which a loader does not gather (the
 * narrowing bootbook.py names).
 *
 * ‼ THE CHAIN IS SEWN BY IDENTITY, NOT BY SEQ: a stitch is taken only when its
 * StitchHead names the last one taken by its seq AND its Clasp (H-2). The
 * walk is bounded twice: every stitch taken has a larger seq than the last,
 * and no more are taken than the volume has blocks.
 */

#include "tagboot_lectern_book.h"

/* The most sectors read in one command while a unit is read or looked for. */
#define RIBBON_READ_SECTORS 64u

/* ------------------------------------------------------------------------- */
/* Where on the ribbon                                                       */
/* ------------------------------------------------------------------------- */

static uint64_t PlaceLeft(const LecternPlace *p)
{
    return p->quire.blocks * VOLUME_BLOCK_SECTORS - p->off;
}

static uint64_t PlaceVlba(const LecternPlace *p)
{
    return p->quire.block * VOLUME_BLOCK_SECTORS + p->off;
}

/* The place `k` sectors on from `p`, into `hop` past its Quire's end: 0 when
 * it would run off a Quire with nothing named after it. */
static int PlaceOn(const LecternPlace *p, uint64_t k, const LecternQuire *hop, LecternPlace *out)
{
    uint64_t left = PlaceLeft(p);
    if (k < left) {
        out->quire = p->quire;
        out->off   = p->off + k;
        return 1;
    }
    if (!hop || !hop->blocks || k - left >= hop->blocks * VOLUME_BLOCK_SECTORS) return 0;
    out->quire = *hop;
    out->off   = k - left;
    return 1;
}

static int QuireSame(const LecternQuire *a, const LecternQuire *b)
{
    return a->block == b->block && a->blocks == b->blocks;
}

/* Whether `q` lies where a Quire of the ribbon may: in the data run, begun on
 * an atom of the volume (stitch.py Lies, ribbon_collate.c RibbonQuireLies). */
static int QuireLies(const LecternBook *b, const LecternQuire *q)
{
    uint64_t lo = b->desk->frame.data_block, hi = b->desk->block_end;
    uint64_t grid = b->desk->atom_blocks ? b->desk->atom_blocks : 1u;
    return q->blocks && q->block >= lo && q->block <= hi && q->blocks <= hi - q->block && !(q->block % grid);
}

/* ------------------------------------------------------------------------- */
/* A unit of the ribbon (ribbon_unit.c)                                      */
/* ------------------------------------------------------------------------- */

void LecternStitchGive(const LecternBook *b, LecternStitch *u)
{
    if (!u) return;
    LecternGive(b, u->valid, u->of);
    LecternGive(b, u->leads, (uint64_t)u->of * sizeof(uint16_t));
    LecternGive(b, u->stream, u->size);
    LecternGive(b, u->records, (uint64_t)u->caprecords * sizeof(LecternRecord));
    LecternGive(b, u, sizeof(*u));
}

static int RecordKept(LecternBook *b, LecternStitch *u, uint64_t tag, uint32_t at, uint32_t body, uint32_t len)
{
    uint64_t cap = u->caprecords;
    if (!LecternGrow(b, (void **)&u->records, u->nrecords, &cap, sizeof(LecternRecord))) return 0;
    u->caprecords                = (uint32_t)cap;
    u->records[u->nrecords++] = (LecternRecord){ tag >> 1, (int)(tag & 1u), at, body, len };
    return 1;
}

/* The StitchHead at the stream's start: whether it reads; the bytes past its
 * known fields into `longer`. */
static int HeadRead(LecternStitch *u, const uint8_t *stream, uint32_t size, uint32_t *next)
{
    uint64_t tag = 0;
    uint32_t at = 0, body = 0, len = 0, used = 0;
    if (BookEntryNext(stream, size, &at, &tag, &body, &len) != 1 || tag >> 1 != RIBBON_STITCH_HEAD ||
        StitchHeadGet(stream + body, len, &u->head, &used) == TALLY_BODY_BROKEN)
        return 0;
    u->head_read = 1;
    u->longer    = len - used;
    *next        = at;
    return 1;
}

/* The stream read whole: StitchHead, records, a Clasp counting them, summing
 * the stream before it, followed by PAD or nothing. 1 so; 0 not; -1 no room. */
static int Parsed(LecternBook *b, LecternStitch *u)
{
    uint32_t at = 0, used = 0;
    if (!HeadRead(u, u->stream, u->size, &at)) return 0;
    for (;;) {
        uint64_t tag = 0;
        uint32_t start = at, body = 0, len = 0;
        if (BookEntryNext(u->stream, u->size, &at, &tag, &body, &len) != 1) return 0;
        if (tag >> 1 != RIBBON_CLASP) {
            if (!RecordKept(b, u, tag, start, body, len)) return -1;
            continue;
        }
        if (ClaspGet(u->stream + body, len, &u->clasp, &used) == TALLY_BODY_BROKEN ||
            u->clasp.records != u->nrecords ||
            u->clasp.crc != ~BookCrc32cOn(0xFFFFFFFFu, u->stream, start) || (at < u->size && u->stream[at] != 0))
            return 0;
        u->longer    += len - used;
        u->clasp_read = 1;
        return 1;
    }
}

static int BytesWhole(const LecternStitch *u, uint32_t lo, uint32_t hi)
{
    for (uint32_t k = lo / BOOK_PAYLOAD_BYTES; k <= (hi - 1u) / BOOK_PAYLOAD_BYTES; k++)
        if (!u->valid[k]) return 0;
    return 1;
}

/* Where the walk is taken up after a gap: the first valid sector from `k`
 * that names a lead, at its lead. */
static int Resume(const LecternStitch *u, uint32_t k, uint32_t *pos)
{
    for (; k < u->of; k++) {
        if (u->valid[k] && u->leads[k]) {
            *pos = k * BOOK_PAYLOAD_BYTES + u->leads[k] - BOOK_HEAD_BYTES;
            return 1;
        }
    }
    return 0;
}

/* A record the walk in pieces met whole: StitchHead at the stream's start,
 * the Clasp (its crc kept when it reads; the walk stops), or a record. */
static int PieceMet(LecternBook *b, LecternStitch *u, uint64_t tag, uint32_t pos, uint32_t body, uint32_t len,
                    int *stop)
{
    uint32_t used = 0, next = 0;
    Clasp    c;
    if (tag >> 1 == RIBBON_STITCH_HEAD && pos == 0) {
        HeadRead(u, u->stream, u->size, &next);
        return 1;
    }
    if (tag >> 1 == RIBBON_CLASP) {
        if (ClaspGet(u->stream + body, len, &c, &used) != TALLY_BODY_BROKEN) {
            u->clasp      = c;
            u->clasp_read = 1;
        }
        *stop = 1;
        return 1;
    }
    return RecordKept(b, u, tag, pos, body, len);
}

/* What can be read of a stream some sectors of which are lost (stitch.py
 * Pieces): the records wholly in valid sectors, the StitchHead and the
 * Clasp's crc when they so lie. 1, or 0 no room. */
static int Pieces(LecternBook *b, LecternStitch *u)
{
    uint32_t pos = 0;
    int      stop = 0;
    u->head_read = u->clasp_read = 0;
    u->nrecords  = 0;
    while (!stop && pos < u->size) {
        uint64_t tag = 0;
        uint32_t next = pos, body = 0, len = 0;
        int      got  = BookEntryNext(u->stream, u->size, &next, &tag, &body, &len);
        if (got == 0 && u->valid[pos / BOOK_PAYLOAD_BYTES]) break;
        if (got == 1 && BytesWhole(u, pos, next)) {
            if (!PieceMet(b, u, tag, pos, body, len, &stop)) return 0;
            pos = next;
            continue;
        }
        uint32_t k = pos / BOOK_PAYLOAD_BYTES, bad = k, to = 0;
        while (bad < u->of && u->valid[bad]) bad++;
        if (bad == u->of) bad = k;
        if (!Resume(u, bad + 1u, &to)) break;
        pos = to;
    }
    return 1;
}

/* The unit's `of` sectors from its start through its hop, read a run at a time
 * into `buf` and judged: each its own when it holds at its place, of a stitch
 * of the unit's seq and `of`, at its own `at`. */
static void SectorsJudged(LecternBook *b, LecternStitch *u, uint8_t *buf)
{
    uint32_t k = 0;
    while (k < u->of) {
        LecternPlace p;
        PlaceOn(&u->start, k, &u->hop, &p);
        uint64_t left = PlaceLeft(&p);
        uint32_t n    = u->of - k;
        if (n > left) n = (uint32_t)left;
        if (n > RIBBON_READ_SECTORS) n = RIBBON_READ_SECTORS;
        LecternSectors(b, PlaceVlba(&p), n, buf);
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t *raw = buf + (uint64_t)i * BOOK_SECTOR_BYTES;
            BookSectorHead h;
            LecternCopy(&h, raw, sizeof(h));
            u->valid[k + i] = BookSectorCheck(raw, b->desk->uuid, PlaceVlba(&p) + i) && h.kind == BOOK_UNIT_STITCH &&
                              h.seq == u->seq && h.of == u->of && h.at == k + i;
            u->leads[k + i] = u->valid[k + i] ? h.lead : 0;
            if (u->valid[k + i])
                LecternCopy(u->stream + (uint64_t)(k + i) * BOOK_PAYLOAD_BYTES, raw + BOOK_HEAD_BYTES,
                            BOOK_PAYLOAD_BYTES);
        }
        k += n;
    }
}

/* The unit's sectors read and judged, its stream parsed whole or walked in
 * pieces. 1, or 0 no room. */
static int UnitFilled(LecternBook *b, LecternStitch *u)
{
    uint8_t *buf = (uint8_t *)LecternTake(b, RIBBON_READ_SECTORS * BOOK_SECTOR_BYTES);
    int      whole = 1, rc;

    u->size   = u->of * BOOK_PAYLOAD_BYTES;        /* RibbonShape: no more than 2^32 - 1 */
    u->valid  = (uint8_t *)LecternTake(b, u->of);
    u->leads  = (uint16_t *)LecternTake(b, (uint64_t)u->of * sizeof(uint16_t));
    u->stream = (uint8_t *)LecternTake(b, u->size);
    if (!buf || !u->valid || !u->leads || !u->stream) {
        LecternGive(b, buf, RIBBON_READ_SECTORS * BOOK_SECTOR_BYTES);
        return 0;
    }
    LecternZero(u->stream, u->size);
    SectorsJudged(b, u, buf);
    LecternGive(b, buf, RIBBON_READ_SECTORS * BOOK_SECTOR_BYTES);
    u->has_end = PlaceOn(&u->start, u->of, &u->hop, &u->end);
    for (uint32_t k = 0; k < u->of; k++) whole = whole && u->valid[k];
    rc = whole ? Parsed(b, u) : 0;
    if (rc < 0) return 0;
    if ((u->sound = rc == 1)) return 1;
    return Pieces(b, u);
}

/* The unit of `of` sectors and `seq` begun at `start`, running on into `hop`:
 * NULL with no room (b->starved). One no stitch is shaped like is read not at
 * all (§16.3 п.8). */
static LecternStitch *UnitRead(LecternBook *b, const LecternPlace *start, uint32_t of, uint64_t seq,
                               const LecternQuire *hop)
{
    LecternStitch *u = (LecternStitch *)LecternTake(b, sizeof(LecternStitch));
    if (!u) return u;
    LecternZero(u, sizeof(*u));
    u->start = *start;
    u->seq   = seq;
    u->of    = of;
    if (hop) u->hop = *hop;
    u->shape = RibbonShape(start->off, PlaceLeft(start), u->hop.blocks * VOLUME_BLOCK_SECTORS, of);
    if (u->shape == RIBBON_SHAPE_KEPT && !UnitFilled(b, u)) {
        LecternStitchGive(b, u);
        return (LecternStitch *)0;
    }
    return u;
}

/* The next_quire a StitchHead in the first sector of a unit names, read in
 * `raw`, when it lies where one may (stitch.py Peek). */
static int Peek(const LecternBook *b, const uint8_t *raw, LecternQuire *next)
{
    LecternStitch  u;
    BookSectorHead h;
    uint32_t       at = 0;
    LecternCopy(&h, raw, sizeof(h));
    LecternZero(&u, sizeof(u));
    if (h.at != 0 || !HeadRead(&u, raw + BOOK_HEAD_BYTES, BOOK_PAYLOAD_BYTES, &at) || !u.head.next_blocks) return 0;
    next->block  = u.head.next_block;
    next->blocks = u.head.next_blocks;
    return QuireLies(b, next);
}

/* The first sector from `place` through `hop` holding a stitch of seq past
 * `after`: *found, its index in *j, its head and the sector in `first`. 1, or
 * 0 no room. */
static int Look(LecternBook *b, const LecternPlace *place, const LecternQuire *hop, uint64_t after, uint64_t *j,
                BookSectorHead *head, uint8_t *first, int *found)
{
    uint64_t limit = PlaceLeft(place) + (hop ? hop->blocks * VOLUME_BLOCK_SECTORS : 0);
    uint8_t *buf   = (uint8_t *)LecternTake(b, RIBBON_READ_SECTORS * BOOK_SECTOR_BYTES);
    *found = 0;
    if (!buf) return 0;
    for (uint64_t k = 0; k < limit && !*found;) {
        LecternPlace p;
        if (!PlaceOn(place, k, hop, &p)) break;
        uint64_t n = PlaceLeft(&p) < RIBBON_READ_SECTORS ? PlaceLeft(&p) : RIBBON_READ_SECTORS;
        LecternSectors(b, PlaceVlba(&p), (uint32_t)n, buf);
        for (uint32_t i = 0; i < n && !*found; i++) {
            const uint8_t *s = buf + (uint64_t)i * BOOK_SECTOR_BYTES;
            LecternCopy(head, s, sizeof(*head));
            if (BookSectorCheck(s, b->desk->uuid, PlaceVlba(&p) + i) && head->kind == BOOK_UNIT_STITCH &&
                head->seq > after) {
                *found = 1;
                *j     = k + i;
                LecternCopy(first, s, BOOK_SECTOR_BYTES);
            }
        }
        k += n;
    }
    LecternGive(b, buf, RIBBON_READ_SECTORS * BOOK_SECTOR_BYTES);
    return 1;
}

/* The unit of seq past `after` that begins at `place`, by the first sector
 * from there on through `hop` holding one (stitch.py FindUnit): NULL when that
 * sector is a unit's begun elsewhere, there is none, or no room. */
static LecternStitch *UnitFind(LecternBook *b, const LecternPlace *place, const LecternQuire *hop, uint64_t after)
{
    BookSectorHead head;
    LecternQuire   own;
    uint64_t       j = 0;
    int            found = 0;
    uint8_t        first[BOOK_SECTOR_BYTES];

    if (!Look(b, place, hop, after, &j, &head, first, &found) || !found || head.at != j)
        return (LecternStitch *)0;
    return UnitRead(b, place, head.of, head.seq, j == 0 && Peek(b, first, &own) ? &own : hop);
}

/* Rule (б′)'s look along the next_quire from `place`: the first sector of a
 * seq past `after` — *found, its index *j and head — and, when it is its
 * unit's first, that unit, running on into the next_quire its own StitchHead
 * names. 1, or 0 no room. */
static int UnitAhead(LecternBook *b, const LecternPlace *place, uint64_t after, uint64_t *j, BookSectorHead *head,
                     int *found, LecternStitch **out)
{
    LecternQuire own;
    uint8_t      first[BOOK_SECTOR_BYTES];
    *out = (LecternStitch *)0;
    if (!Look(b, place, (const LecternQuire *)0, after, j, head, first, found)) return 0;
    if (*found && head->at == 0) {
        LecternPlace at = { place->quire, place->off + *j };
        *out = UnitRead(b, &at, head->of, head->seq, Peek(b, first, &own) ? &own : (const LecternQuire *)0);
        if (!*out) return 0;
    }
    return 1;
}

/* The seq a unit's StitchHead names before it — 0 for none — when it reads
 * and names one below the unit's own. */
static int Names(const LecternStitch *u, uint64_t *seq)
{
    if (!u->head_read || u->head.prev > u->seq) return 0;
    *seq = u->head.prev ? u->seq - u->head.prev : 0;
    return 1;
}

/* ------------------------------------------------------------------------- */
/* The chain (ribbon_collate.c)                                              */
/* ------------------------------------------------------------------------- */

typedef struct {
    LecternBook   *b;
    uint64_t       seq;          /* the last stitch taken, and its Clasp — unknown when it did not read */
    uint32_t       clasp;
    int            clasp_known;
    LecternQuire   quire, next;  /* the Quire the chain is in, and the next it may run on into */
    int            has_end;
    LecternPlace   end;          /* where the next unit begins */
    LecternStitch *ahead;        /* the unit at the end of the stitch last taken, read to weigh it */
    int            ahead_read;
} LecternChain;

/* `q` taken as the next_quire when it may be. */
static void Named(LecternChain *c, const LecternQuire *q)
{
    if (!q->blocks || QuireSame(q, &c->quire) || QuireSame(q, &c->next) || !QuireLies(c->b, q)) return;
    c->next = *q;
}

/* `u` sewn on: the chain's last stitch from here, its Quire the chain's when it
 * ended in another, the next_quire its StitchHead names weighed. 1, or 0 no
 * room (`u` given back). */
static int Take(LecternChain *c, LecternStitch *u)
{
    LecternBook *b = c->b;
    LecternPlace last;
    if (!LecternGrow(b, (void **)&b->stitches, b->nstitches, &b->capstitches, sizeof(LecternStitch *))) {
        LecternStitchGive(b, u);
        return 0;
    }
    b->stitches[b->nstitches++] = u;
    c->seq         = u->seq;
    c->clasp       = u->clasp.crc;
    c->clasp_known = u->clasp_read;
    PlaceOn(&u->start, u->of - 1u, &u->hop, &last);
    if (!QuireSame(&last.quire, &c->quire)) {
        c->quire = last.quire;
        c->next  = (LecternQuire){ 0, 0 };
    }
    c->has_end = u->has_end;
    c->end     = u->end;
    if (u->head_read) {
        LecternQuire next = { u->head.next_block, u->head.next_blocks };
        Named(c, &next);
    }
    return 1;
}

/* Does `u`'s StitchHead name the last stitch taken by its seq and Clasp —
 * and, begun at the start of the next_quire, the Quire the last one ended in
 * as the one it came from? One that does not read names nothing either way. */
static int Follows(const LecternChain *c, const LecternStitch *u, int at_next)
{
    if (!u->head_read) return 1;
    if (!c->clasp_known || u->head.prev_clasp != c->clasp || u->head.prev != (c->seq ? u->seq - c->seq : 0))
        return 0;
    return !at_next || (u->head.from_block == c->quire.block && u->head.from_blocks == c->quire.blocks);
}

/* Rule (а) gives `here`, rules (б) and (б′) `there`; when both do, the one of
 * the larger seq is taken (§16.3 п.11), the other given back. */
static LecternStitch *Retried(const LecternChain *c, LecternStitch *here, LecternStitch *there)
{
    if (!here || !there || here->seq == there->seq) {
        if (here && there) LecternStitchGive(c->b, there);
        return here ? here : there;
    }
    LecternStitch *take = here->seq > there->seq ? here : there;
    LecternStitchGive(c->b, take == here ? there : here);
    return take;
}

/* Rule (в): `u` closed and damaged when its StitchHead, if it reads, names the
 * last stitch taken, and the sound stitch after it names its seq and, where
 * its Clasp reads, that Clasp — both taken. *closed whether it was; 1, or 0 no
 * room. `u` stays the caller's when it is not taken. */
static int Closed(LecternChain *c, LecternStitch *u, int at_next, int *closed)
{
    LecternBook   *b = c->b;
    LecternStitch *f;
    uint64_t       names = 0;
    *closed = 0;
    if (!Follows(c, u, at_next) || !u->has_end) return 1;
    LecternQuire own = { u->head.next_block, u->head.next_blocks };
    f = UnitFind(b, &u->end, u->head_read && QuireLies(b, &own) ? &own : &c->next, u->seq);
    if (b->starved) return 0;
    if (!f || !f->sound || !Names(f, &names) || names != u->seq ||
        (u->clasp_read && f->head.prev_clasp != u->clasp.crc)) {
        LecternStitchGive(b, f);
        return 1;
    }
    *closed = 1;
    if (!Take(c, u)) {
        LecternStitchGive(b, f);
        return 0;
    }
    return Take(c, f);
}

/* Past the stitch `take` found in the next_quire, while a whole stitch naming
 * the last one taken, with a larger seq, lies right after it — a try refused
 * that landed, and its retry (§16.3 п.11) — that one in its place. 1, or 0. */
static int FurtherOn(LecternChain *c, LecternStitch **take)
{
    while (*take && (*take)->has_end) {
        LecternQuire   own = { (*take)->head.next_block, (*take)->head.next_blocks };
        LecternStitch *f   = UnitFind(c->b, &(*take)->end, QuireLies(c->b, &own) ? &own : (LecternQuire *)0,
                                      (*take)->seq);
        if (c->b->starved) return 0;
        if (!f || !f->sound || !Follows(c, f, 1)) {
            LecternStitchGive(c->b, f);
            return 1;
        }
        LecternStitchGive(c->b, *take);
        *take = f;
    }
    return 1;
}

/* Rules (б) and (б′) (§16.4 п.4): the stitch the ribbon goes on with in the
 * next_quire — the whole one at its start naming the last stitch taken and the
 * Quire it ended in, or the first such further in, a unit there stepped over
 * to its end, a sector of no seq past the last stepped over alone; to the
 * next_quire's end. *out NULL when nothing goes on; *blank when no sector there
 * holds a stitch of a seq past the last one. 1, or 0 no room. */
static int Further(LecternChain *c, LecternStitch **out, int *blank)
{
    LecternQuire n     = c->next;
    uint64_t     total = n.blocks * VOLUME_BLOCK_SECTORS, pos = 0;
    *out   = (LecternStitch *)0;
    *blank = 1;
    while (pos < total && !*out) {
        LecternPlace   p = { n, pos };
        BookSectorHead h;
        LecternStitch *u = (LecternStitch *)0;
        uint64_t       j = 0;
        int            found = 0;
        if (!UnitAhead(c->b, &p, c->seq, &j, &h, &found, &u)) return 0;
        if (!found) break;
        *blank = 0;
        if (u && u->sound && Follows(c, u, 1)) {
            *out = u;
            break;
        }
        LecternStitchGive(c->b, u);
        pos += j + (h.at < h.of ? (uint64_t)h.of - h.at : 1u);
    }
    if (!FurtherOn(c, out)) {
        LecternStitchGive(c->b, *out);
        *out = (LecternStitch *)0;
        return 0;
    }
    return 1;
}

/* Whether `u`'s StitchHead, read, names stitch `s` by its seq and its Clasp. */
static int NamesStitch(const LecternStitch *u, const LecternStitch *s)
{
    uint64_t names = 0;
    return u && Names(u, &names) && names == s->seq && s->clasp_read && u->head.prev_clasp == s->clasp.crc;
}

/* ‼ RULES (б) AND (б′) ARE WEIGHED ONLY WHERE NOTHING SAYS THE STITCH RULE (а)
 * GIVES WAS KEPT (§16.4 п.4, ribbon_collate.c RibbonKept): a unit past `s`
 * naming it, at its end or at the start of the next_quire it names. *kept so;
 * the unit at its end into *ahead, read through *hop. 1, or 0 no room. */
static int Kept(LecternChain *c, const LecternStitch *s, int *kept, LecternStitch **ahead, LecternQuire *hop)
{
    LecternBook *b    = c->b;
    LecternQuire own  = { s->head.next_block, s->head.next_blocks };
    LecternQuire none = { 0, 0 };
    LecternPlace last;
    *kept  = 0;
    *ahead = (LecternStitch *)0;
    *hop   = none;
    if (!s->has_end) return 1;
    PlaceOn(&s->start, s->of - 1u, &s->hop, &last);
    *hop   = QuireLies(b, &own) ? own : QuireSame(&last.quire, &c->quire) ? c->next : none;
    *ahead = UnitFind(b, &s->end, hop, s->seq);
    if (b->starved) return 0;
    *kept = NamesStitch(*ahead, s);
    if (!*kept && hop->blocks && !QuireSame(&s->end.quire, hop)) {
        LecternPlace   head = { *hop, 0 };
        LecternStitch *v    = UnitFind(b, &head, (LecternQuire *)0, s->seq);
        if (b->starved) return 0;
        *kept = NamesStitch(v, s);
        LecternStitchGive(b, v);
    }
    return 1;
}

/* The unit rule (а) weighs: the one read at the end of the stitch last taken
 * while it was weighed, else looked for there now. */
static LecternStitch *At(LecternChain *c)
{
    LecternStitch *u = c->ahead;
    if (!c->ahead_read) return UnitFind(c->b, &c->end, &c->next, c->seq);
    c->ahead      = (LecternStitch *)0;
    c->ahead_read = 0;
    return u;
}

/* `take` sewn on, and what was read past it kept for the next step when the
 * chain goes on through the Quire it was read through. 1, or 0 no room. */
static int TakeOn(LecternChain *c, LecternStitch *take, LecternStitch *ahead, const LecternQuire *hop, int read)
{
    if (!Take(c, take)) {
        LecternStitchGive(c->b, ahead);
        return 0;
    }
    if (read && QuireSame(hop, &c->next)) {
        c->ahead      = ahead;
        c->ahead_read = 1;
    } else {
        LecternStitchGive(c->b, ahead);
    }
    return 1;
}

/* Rule (в) tried on `u` (at the end) and `v` (at the start of the next_quire):
 * each that is neither sound nor misshapen, in turn, until one closes. Both
 * given back but the one taken. 1, or 0 no room. */
static int ClosedEither(LecternChain *c, LecternStitch *u, LecternStitch *v, int *took)
{
    LecternStitch *tried[2] = { u, v };
    int            rc       = 1;
    for (uint32_t i = 0; rc && i < 2u && !*took; i++) {
        if (!tried[i] || tried[i]->sound || tried[i]->shape != RIBBON_SHAPE_KEPT) continue;
        rc = Closed(c, tried[i], tried[i] == v, took);
        if (*took) tried[i] = (LecternStitch *)0;
    }
    LecternStitchGive(c->b, tried[0]);
    LecternStitchGive(c->b, tried[1]);
    return rc;
}

/* One step of rules (а)–(в): *took whether a stitch was sewn on. 1, or 0. */
static int SewOne(LecternChain *c, int *took)
{
    LecternBook   *b = c->b;
    LecternStitch *u = At(c), *v = (LecternStitch *)0, *here, *there = (LecternStitch *)0, *ahead = (LecternStitch *)0;
    LecternQuire   hop  = { 0, 0 };
    int            kept = 0, read = 0, blank = 1, rc = 1;
    *took = 0;
    if (b->starved) return 0;
    here = u && u->sound && Follows(c, u, 0) ? u : (LecternStitch *)0;
    if (here && c->next.blocks) {
        rc   = Kept(c, here, &kept, &ahead, &hop);
        read = rc && here->has_end;
    }
    if (rc && c->next.blocks && !kept) rc = Further(c, &there, &blank);
    if (rc && !here && !there && !blank && !QuireSame(&c->end.quire, &c->next)) {
        LecternPlace head = { c->next, 0 };
        v  = UnitFind(b, &head, (LecternQuire *)0, c->seq);
        rc = !b->starved;
    }
    if (rc && (here || there)) {
        LecternStitch *take = Retried(c, here, there);
        if (u != here) LecternStitchGive(b, u);
        *took = 1;
        return TakeOn(c, take, ahead, &hop, read && take == here);
    }
    LecternStitchGive(b, ahead);
    if (!rc) {
        LecternStitchGive(b, u);
        LecternStitchGive(b, there);
        LecternStitchGive(b, v);
        return 0;
    }
    return ClosedEither(c, u, v, took);
}

int LecternSew(LecternBook *b)
{
    const SpineRibbon *r = &b->ribbon;
    LecternChain       c;
    uint64_t           hops = 0;
    int                took = 1, rc = 1;

    LecternZero(&c, sizeof(c));
    c.b           = b;
    c.seq         = b->head.stitch_seq;
    c.clasp       = b->head.stitch_clasp;
    c.clasp_known = 1;
    c.quire       = (LecternQuire){ r->quire_block, r->quire_blocks };
    if (!QuireLies(b, &c.quire) || r->at < r->quire_block || r->at - r->quire_block >= r->quire_blocks) return 1;
    c.end     = (LecternPlace){ c.quire, (r->at - r->quire_block) * VOLUME_BLOCK_SECTORS };
    c.has_end = 1;
    LecternQuire next = { r->next_block, r->next_blocks };
    Named(&c, &next);
    while (rc && took && c.has_end && hops <= b->desk->block_end) {
        hops++;
        rc = SewOne(&c, &took);
    }
    LecternStitchGive(b, c.ahead);
    return rc;
}
