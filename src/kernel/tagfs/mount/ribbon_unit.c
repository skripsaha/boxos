#include "ribbon.h"
#include "book_unit.h"
#include "crypto.h"

/*
 * A unit of the ribbon read off the medium (ribbon.h): its sectors judged one
 * by one as its own or not, its stream parsed whole from StitchHead to a
 * Clasp that holds — or, where sectors are lost, walked in pieces, taken up
 * again after each gap at the `lead` of the first sector past it that names
 * one (§2.1). volume-read reads a unit the same way (stitch.py Unit, Parsed,
 * Pieces); the census scenario holds the two to one answer on every landing.
 */

/* The most sectors read in one command while a unit is looked for. */
#define RIBBON_LOOK_SECTORS  64u

bool RibbonPlaceOn(const RibbonPlace* p, uint64_t k, const RibbonQuire* hop, RibbonPlace* out)
{
    uint64_t left = RibbonPlaceLeft(p);
    if (k < left) {
        out->quire = p->quire;
        out->off   = p->off + k;
        return true;
    }
    if (!hop || !hop->blocks || k - left >= hop->blocks * VOLUME_BLOCK_SECTORS)
        return false;
    out->quire = *hop;
    out->off   = k - left;
    return true;
}

bool RibbonUnitNames(const RibbonUnit* u, uint64_t* seq)
{
    if (!u->head_read || u->head.prev > u->seq)
        return false;
    *seq = u->head.prev ? u->seq - u->head.prev : 0;
    return true;
}

bool RibbonUnitWhole(const RibbonUnit* u)
{
    if (u->shape != RIBBON_SHAPE_KEPT || !u->of)
        return false;
    for (uint32_t k = 0; k < u->of; k++)
        if (!u->present[k])
            return false;
    return true;
}

void RibbonUnitLetGo(RibbonUnit* u)
{
    if (!u)
        return;
    kfree(u->valid);
    kfree(u->leads);
    kfree(u->stream);
    kfree(u->records);
    kfree(u);
}

/* One record more for the unit: false with no memory. */
static bool RibbonRecordKept(RibbonUnit* u, uint32_t* cap, uint64_t tag, uint32_t at, uint32_t body,
                             uint32_t len)
{
    if (u->nrecords == *cap) {
        uint32_t      more = *cap ? *cap * 2u : 8u;
        RibbonRecord* to   = kmalloc(sizeof(RibbonRecord) * more);
        if (!to)
            return false;
        if (u->nrecords)
            memcpy(to, u->records, sizeof(RibbonRecord) * u->nrecords);
        kfree(u->records);
        u->records = to;
        *cap       = more;
    }
    u->records[u->nrecords++] = (RibbonRecord){ tag >> 1, (tag & 1u) != 0, at, body, len };
    return true;
}

/* The StitchHead at the stream's start: whether it reads. Bytes past its known
 * fields are a newer writer's (§2.3), counted into `longer`. */
static bool RibbonHeadRead(RibbonUnit* u, const uint8_t* stream, uint32_t size, uint32_t* next)
{
    uint64_t tag = 0;
    uint32_t at = 0, body = 0, len = 0, used = 0;
    if (BookEntryNext(stream, size, &at, &tag, &body, &len) != 1 || tag >> 1 != RIBBON_STITCH_HEAD ||
        StitchHeadGet(stream + body, len, &u->head, &used) == TALLY_BODY_BROKEN)
        return false;
    u->head_read = true;
    u->longer    = len - used;
    *next        = at;
    return true;
}

/*
 * The stream read whole (stitch.py Parsed): StitchHead, records, and a Clasp
 * counting them, summing the stream before it, and followed by PAD or
 * nothing. True when it so reads; false — the unit is not sound — when it
 * does not, ERR_NO_MEMORY aside (*rc).
 */
static bool RibbonParsed(RibbonUnit* u, error_t* rc)
{
    uint32_t at = 0, cap = 0, used = 0;
    if (!RibbonHeadRead(u, u->stream, u->size, &at))
        return false;
    for (;;) {
        uint64_t tag = 0;
        uint32_t start = at, body = 0, len = 0;
        if (BookEntryNext(u->stream, u->size, &at, &tag, &body, &len) != 1)
            return false;
        if (tag >> 1 != RIBBON_CLASP) {
            if (!RibbonRecordKept(u, &cap, tag, start, body, len)) {
                *rc = ERR_NO_MEMORY;
                return false;
            }
            continue;
        }
        if (ClaspGet(u->stream + body, len, &u->clasp, &used) == TALLY_BODY_BROKEN ||
            u->clasp.records != u->nrecords || u->clasp.crc != KCrc32c(u->stream, start) ||
            (at < u->size && u->stream[at] != 0))
            return false;
        u->longer    += len - used;
        u->clasp_read = true;
        u->clasp_at   = start;
        return true;
    }
}

/* Whether every sector the stream bytes [lo, hi) lie in is valid. */
static bool RibbonBytesWhole(const RibbonUnit* u, uint32_t lo, uint32_t hi)
{
    for (uint32_t k = lo / BOOK_PAYLOAD_BYTES; k <= (hi - 1u) / BOOK_PAYLOAD_BYTES; k++)
        if (!u->valid[k])
            return false;
    return true;
}

/* Where the walk is taken up after a gap: the first valid sector from `k`
 * that names a lead, at its lead — false when there is none. */
static bool RibbonResume(const RibbonUnit* u, uint32_t k, uint32_t* pos)
{
    for (; k < u->of; k++) {
        if (u->valid[k] && u->leads[k]) {
            *pos = k * BOOK_PAYLOAD_BYTES + u->leads[k] - BOOK_HEAD_BYTES;
            return true;
        }
    }
    return false;
}

/* A record the walk in pieces met whole: StitchHead at the stream's start,
 * the Clasp (whose crc is kept when it reads; the walk stops there), or a
 * record of the stitch. False with no memory; *stop at the Clasp. */
static bool RibbonPieceMet(RibbonUnit* u, uint32_t* cap, uint64_t tag, uint32_t pos, uint32_t body, uint32_t len,
                           bool* stop)
{
    uint32_t used = 0, next = 0;
    if (tag >> 1 == RIBBON_STITCH_HEAD && pos == 0) {
        RibbonHeadRead(u, u->stream, u->size, &next);
        return true;
    }
    if (tag >> 1 == RIBBON_CLASP) {
        Clasp c;
        if (ClaspGet(u->stream + body, len, &c, &used) != TALLY_BODY_BROKEN) {
            u->clasp      = c;
            u->clasp_read = true;
        }
        *stop = true;
        return true;
    }
    return RibbonRecordKept(u, cap, tag, pos, body, len);
}

/*
 * What can be read of a stream some sectors of which are lost (stitch.py
 * Pieces): the records wholly in valid sectors, the StitchHead and the
 * Clasp's crc when they so lie, and the bytes lost — from where the record a
 * gap cut begins to where the walk is taken up. ERR_NO_MEMORY, or OK.
 */
static error_t RibbonPieces(RibbonUnit* u)
{
    uint32_t pos = 0, cap = 0;
    bool     stop = false;
    u->head_read = u->clasp_read = false;
    u->nrecords  = 0;
    while (!stop && pos < u->size) {
        uint64_t tag = 0;
        uint32_t next = pos, body = 0, len = 0;
        int      got  = BookEntryNext(u->stream, u->size, &next, &tag, &body, &len);
        if (got == 0 && u->valid[pos / BOOK_PAYLOAD_BYTES])
            break;
        if (got == 1 && RibbonBytesWhole(u, pos, next)) {
            if (!RibbonPieceMet(u, &cap, tag, pos, body, len, &stop))
                return ERR_NO_MEMORY;
            pos = next;
            continue;
        }
        uint32_t k = pos / BOOK_PAYLOAD_BYTES, bad = k, to = 0;
        while (bad < u->of && u->valid[bad])
            bad++;
        if (bad == u->of)
            bad = k;
        bool     resumed = RibbonResume(u, bad + 1u, &to);
        uint32_t reach   = (bad + 1u) * BOOK_PAYLOAD_BYTES;
        u->lost += (resumed ? to : (reach < u->size ? reach : u->size)) - pos;
        if (!resumed)
            break;
        pos = to;
    }
    return OK;
}

/* Sector k of the unit, read at `raw` with `state`, judged as the unit's own
 * or not, its payload laid into the stream when it is. */
static void RibbonSectorJudged(RibbonUnit* u, uint32_t k, const uint8_t* raw, uint8_t state)
{
    BookSectorHead h;
    memcpy(&h, raw, sizeof(h));
    bool mine     = h.kind == BOOK_UNIT_STITCH && h.seq == u->seq && h.of == u->of && h.at == k;
    u->holds[k]   = state == BOOK_SECTOR_HOLDS;
    u->valid[k]   = u->holds[k] && mine;
    u->present[k] = u->valid[k] || (state != BOOK_SECTOR_UNREAD && mine);
    u->leads[k]   = u->valid[k] ? h.lead : 0;
    if (u->valid[k])
        memcpy(u->stream + (uint64_t)k * BOOK_PAYLOAD_BYTES, raw + BOOK_HEAD_BYTES, BOOK_PAYLOAD_BYTES);
}

/* The unit's `of` sectors from `start` through `hop`, read a run at a time
 * and judged. */
static error_t RibbonSectorsRead(RibbonUnit* u)
{
    uint32_t k = 0;
    while (k < u->of) {
        RibbonPlace p;
        BookUnit    run;
        RibbonPlaceOn(&u->start, k, &u->hop, &p);
        uint64_t left = RibbonPlaceLeft(&p);
        uint32_t n    = u->of - k;
        if (n > left)
            n = (uint32_t)left;
        if (n > 256u)
            n = 256u;
        error_t rc = BookUnitRead(RibbonPlaceVlba(&p), n, 0, 0, &run);
        for (uint32_t i = 0; rc == OK && i < n; i++)
            RibbonSectorJudged(u, k + i, run.sectors + (uint64_t)i * BOOK_SECTOR_BYTES, run.state[i]);
        BookUnitLetGo(&run);
        if (rc != OK)
            return rc;
        k += n;
    }
    return OK;
}

/* The unit's sectors read and judged, and its stream parsed whole or walked
 * in pieces. OK, ERR_NO_MEMORY, or the medium's refusal. */
static error_t RibbonUnitFilled(RibbonUnit* u)
{
    uint32_t of = u->of;
    u->size   = (uint32_t)((uint64_t)of * BOOK_PAYLOAD_BYTES);      /* RibbonShape: no more than 2^32 - 1 */
    u->valid  = kmalloc((uint64_t)of * 3u);
    u->leads  = kmalloc((uint64_t)of * sizeof(uint16_t));
    u->stream = kmalloc(u->size);
    if (!u->valid || !u->leads || !u->stream)
        return ERR_NO_MEMORY;
    u->present = u->valid + of;
    u->holds   = u->valid + 2u * (uint64_t)of;
    memset(u->stream, 0, u->size);
    error_t rc = RibbonSectorsRead(u);
    if (rc != OK)
        return rc;
    u->has_end = RibbonPlaceOn(&u->start, of, &u->hop, &u->end);
    bool whole = true;
    for (uint32_t k = 0; k < of; k++)
        whole = whole && u->valid[k];
    if (whole && (u->sound = RibbonParsed(u, &rc)))
        return OK;
    return rc != OK ? rc : RibbonPieces(u);
}

error_t RibbonUnitRead(const RibbonPlace* start, uint32_t of, uint64_t seq, const RibbonQuire* hop,
                       RibbonUnit** out)
{
    RibbonUnit* u = kmalloc(sizeof(RibbonUnit));
    *out = NULL;
    if (!u)
        return ERR_NO_MEMORY;
    memset(u, 0, sizeof(*u));
    u->start = *start;
    u->seq   = seq;
    u->of    = of;
    if (hop)
        u->hop = *hop;
    u->shape = RibbonShape(start->off, RibbonPlaceLeft(start), u->hop.blocks * VOLUME_BLOCK_SECTORS, of);
    error_t rc = u->shape == RIBBON_SHAPE_KEPT ? RibbonUnitFilled(u) : OK;
    if (rc != OK) {
        RibbonUnitLetGo(u);
        return rc;
    }
    *out = u;
    return OK;
}

/* The next_quire a StitchHead in sector `raw` names, when one reads there, the
 * sector is a unit's first, and the Quire lies where one may (stitch.py Peek):
 * the unit's own word on where it runs on to — a word that would send the
 * reader off the volume's data run is not taken. */
static bool RibbonPeek(const uint8_t* raw, RibbonQuire* next)
{
    RibbonUnit     u;
    BookSectorHead h;
    uint32_t       at = 0;
    memcpy(&h, raw, sizeof(h));
    memset(&u, 0, sizeof(u));
    if (h.at != 0 || !RibbonHeadRead(&u, raw + BOOK_HEAD_BYTES, BOOK_PAYLOAD_BYTES, &at) || !u.head.next_blocks)
        return false;
    next->block  = u.head.next_block;
    next->blocks = u.head.next_blocks;
    return RibbonQuireLies(next);
}

/* The first sector from `place` through `hop` holding a stitch of seq past
 * `after`: true with its index `*j` and its head, read into `first` (one
 * sector) — false when the Quires hold none. OK, or the medium's or memory's
 * refusal. */
static error_t RibbonLook(const RibbonPlace* place, const RibbonQuire* hop, uint64_t after, uint64_t* j,
                          BookSectorHead* head, uint8_t* first, bool* found)
{
    uint64_t limit = RibbonPlaceLeft(place) + (hop ? hop->blocks * VOLUME_BLOCK_SECTORS : 0);
    *found = false;
    for (uint64_t k = 0; k < limit && !*found;) {
        RibbonPlace p;
        BookUnit    run;
        if (!RibbonPlaceOn(place, k, hop, &p))
            return OK;
        uint64_t n = RibbonPlaceLeft(&p);
        if (n > RIBBON_LOOK_SECTORS)
            n = RIBBON_LOOK_SECTORS;
        error_t rc = BookUnitRead(RibbonPlaceVlba(&p), (uint32_t)n, 0, 0, &run);
        for (uint32_t i = 0; rc == OK && i < n && !*found; i++) {
            const uint8_t* s = run.sectors + (uint64_t)i * BOOK_SECTOR_BYTES;
            memcpy(head, s, sizeof(*head));
            if (run.state[i] == BOOK_SECTOR_HOLDS && head->kind == BOOK_UNIT_STITCH && head->seq > after) {
                *found = true;
                *j     = k + i;
                memcpy(first, s, BOOK_SECTOR_BYTES);
            }
        }
        BookUnitLetGo(&run);
        if (rc != OK)
            return rc;
        k += n;
    }
    return OK;
}

error_t RibbonUnitFind(const RibbonPlace* place, const RibbonQuire* hop, uint64_t after, RibbonUnit** out)
{
    BookSectorHead head;
    RibbonQuire    own;
    uint64_t       j     = 0;
    bool           found = false;
    uint8_t*       first = kmalloc(BOOK_SECTOR_BYTES);
    *out = NULL;
    if (!first)
        return ERR_NO_MEMORY;
    error_t rc = RibbonLook(place, hop, after, &j, &head, first, &found);
    if (rc == OK && found && head.at == j) {
        bool peeked = j == 0 && RibbonPeek(first, &own);
        rc = RibbonUnitRead(place, head.of, head.seq, peeked ? &own : hop, out);
    }
    kfree(first);
    return rc;
}

error_t RibbonUnitHead(const RibbonPlace* place, const RibbonQuire* hop, uint64_t after, RibbonUnit* u,
                       bool* found)
{
    BookUnit       run;
    BookSectorHead h;
    RibbonQuire    own;
    uint32_t       at = 0;
    *found = false;
    memset(u, 0, sizeof(*u));
    error_t rc = BookUnitRead(RibbonPlaceVlba(place), 1, 0, 0, &run);
    if (rc == OK && run.state[0] == BOOK_SECTOR_HOLDS) {
        memcpy(&h, run.sectors, sizeof(h));
        u->start = *place;
        u->seq   = h.seq;
        u->of    = h.of;
        if (h.kind == BOOK_UNIT_STITCH && h.at == 0 && h.seq > after)
            *found = RibbonHeadRead(u, run.sectors + BOOK_HEAD_BYTES, BOOK_PAYLOAD_BYTES, &at);
        if (*found && RibbonPeek(run.sectors, &own))
            u->hop = own;
        else if (*found && hop)
            u->hop = *hop;
    }
    BookUnitLetGo(&run);
    return rc;
}
