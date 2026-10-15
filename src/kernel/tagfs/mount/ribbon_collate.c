#include "ribbon.h"
#include "book_mount.h"
#include "book_unit.h"
#include "ground_census.h"
#include "../file_map/file_map.h"

/*
 * RibbonCollate — pass 1 of the ribbon (spec_v2 §4.5 п.5): from the `at` of a
 * slot, the stitches sewn on, one after another, by the Clasp each names
 * (H-2) — rules (а)–(г) — and the Quires the chain runs through; then, for the
 * slot the mount stands on, the direct scan of what lies past the end of the
 * chain (§2.2) and what each branch there is (§3.9, §16.3 пп.10–11). Nothing
 * is applied here: pass 2 replays what pass 1 sewed on (ribbon_transcribe.c).
 *
 * ‼ THE CHAIN IS SEWN BY IDENTITY, NOT BY SEQ. A stitch is taken only when its
 * StitchHead names the last one taken by its seq AND its Clasp: a dead branch
 * of another session, whatever seq it carries, does not fit (ZIL's GUID+SEQ
 * in a block's sum, zil.h:109-115). The walk is bounded twice: every stitch
 * taken has a larger seq than the last, and no more are taken than the data
 * run has blocks (`hops >= data_blocks`, meta_pool_mount.c:478-480).
 *
 * ‼ THE NEXT UNIT IS WHERE THE LAST ONE ENDS BY ITS WRITER'S ATOM (§16.2
 * п.11): `start + of`, never this mount's atom — a volume moved onto a medium
 * of another physical block is read the same.
 *
 * The rules and the lines are volume-read's (tools/volumeread/stitch.py
 * Ribbon), word for word where the kernel can say them so: the census
 * scenario plants every rule and holds the two readers to one answer.
 */

/* A line of the ribbon, made here before it is said (the door is shut and
 * one core attends the mount). */
static char s_line[640];

static const char* const g_ribbon_codes[RIBBON_CODES] = {
    [RIBBON_CODE_RIBBON_TORN]     = "RIBBON_TORN",
    [RIBBON_CODE_STITCH_TORN]     = "STITCH_TORN",
    [RIBBON_CODE_CLASP_STRANGER]  = "CLASP_STRANGER",
    [RIBBON_CODE_DEAD_BRANCH]     = "DEAD_BRANCH",
    [RIBBON_CODE_RECORD_SHAPE]    = "RECORD_SHAPE",
    [RIBBON_CODE_UNDONE]          = "UNDONE",
    [RIBBON_CODE_NOT_LANDED]      = "NOT_LANDED",
    [RIBBON_CODE_BASE_MISMATCH]   = "BASE_MISMATCH",
    [RIBBON_CODE_EXTENT_OUTSIDE]  = "EXTENT_OUTSIDE",
};

void RibbonSay(RibbonWalk* w, RibbonCode code, const char* fmt, ...)
{
    va_list ap;
    w->said[code]++;
    if (!w->stood)
        return;
    g_book.said.ribbon[code]++;
    va_start(ap, fmt);
    kvsnprintf(s_line, sizeof(s_line), fmt, ap);
    va_end(ap);
    kprintf("[TagFS] ribbon %s: %s\n", g_ribbon_codes[code], s_line);
}

void RibbonSeqsSaid(RibbonUnit* const* units, uint32_t n, char* out, size_t room)
{
    size_t at = 0;
    out[0] = '\0';
    for (uint32_t i = 0; i < n && at + 24u < room; i++)
        at += (size_t)ksnprintf(out + at, room - at, i ? ", %llu" : "%llu", (unsigned long long)units[i]->seq);
}

void RibbonWalkLetGo(RibbonWalk* w)
{
    if (!w)
        return;
    for (uint32_t i = 0; i < w->nstitch; i++)
        RibbonUnitLetGo(w->stitch[i]);
    for (uint32_t i = 0; i < w->npast; i++)
        RibbonUnitLetGo(w->past[i]);
    RibbonUnitLetGo(w->torn);
    kfree(w->ground);
    kfree(w->stitch);
    kfree(w->past);
    kfree(w->quires);
    kfree(w);
}

// ----------------------------------------------------------------------------
// The Quires, and the next one a stitch names
// ----------------------------------------------------------------------------

bool RibbonQuireAdd(RibbonWalk* w, const RibbonQuire* q)
{
    for (uint32_t i = 0; i < w->nquires; i++)
        if (RibbonQuireSame(&w->quires[i], q))
            return true;
    if (w->nquires == w->capquires) {
        uint32_t     cap  = w->capquires ? w->capquires * 2u : 4u;
        RibbonQuire* more = kmalloc(sizeof(RibbonQuire) * cap);
        if (!more)
            return false;
        if (w->nquires)
            memcpy(more, w->quires, sizeof(RibbonQuire) * w->nquires);
        kfree(w->quires);
        w->quires    = more;
        w->capquires = cap;
    }
    w->quires[w->nquires++] = *q;
    return true;
}

bool RibbonQuireLies(const RibbonQuire* q)
{
    uint64_t lo = g_book.frame.data_block, hi = g_book.data_end;
    uint64_t grid = g_book.atom > TAGFS_BLOCK_SIZE ? g_book.atom / TAGFS_BLOCK_SIZE : 1u;
    return q->blocks && q->block >= lo && q->block <= hi && q->blocks <= hi - q->block && !(q->block % grid);
}

static int RibbonGroundOrder(const void* a, const void* b, void* ctx)
{
    const RibbonGroundRun* x = a;
    const RibbonGroundRun* y = b;
    (void)ctx;
    return x->start < y->start ? -1 : x->start > y->start;
}

/*
 * The ground a next_quire may not name for a file (§3.7's table): every DATA
 * run of every live file the Tally holds — the map as it stands before a
 * record of the ribbon is replayed — gathered once, the first time a Quire is
 * weighed, sorted by its start, each with the furthest end of any run up to
 * it. A Quire is then weighed by a search, not by a walk of every file: a
 * writer names one next_quire in every stitch of a Quire (M-5), and a ribbon
 * between two Tallies may carry many. False with no memory.
 */
static bool RibbonGroundBuilt(RibbonWalk* w)
{
    TagFSState* fs   = tagfs_get_state();
    uint64_t    from = 0, cap = 0, ids[16];
    uint32_t    n;
    if (w->ground_built)
        return true;
    while ((n = FileMapRows(fs->files, from, ids, 16)) > 0) {
        for (uint32_t i = 0; i < n; i++) {
            FileRecord* rec  = FileMapRecordHold(fs->files, ids[i], NULL);
            bool        room = true;
            StripWalk   walk;
            Strip       run;
            if (!rec)
                continue;
            StripWalkBegin(rec, &walk);
            while (room && StripWalkNextGround(&walk, &run)) {
                room = RibbonGrow((void**)&w->ground, w->nground, &cap, sizeof(RibbonGroundRun));
                if (room)
                    w->ground[w->nground++] = (RibbonGroundRun){ run.start, run.start + run.count };
            }
            FileRecordRelease(rec);
            if (!room)
                return false;
        }
        from = ids[n - 1] + 1;
    }
    KSort(w->ground, w->nground, sizeof(RibbonGroundRun), RibbonGroundOrder, NULL);
    for (uint64_t i = 1; i < w->nground; i++)
        if (w->ground[i].reach < w->ground[i - 1].reach)
            w->ground[i].reach = w->ground[i - 1].reach;
    w->ground_built = true;
    return true;
}

/* Whether a run of the files' ground meets [start, start + count): the last
 * run begun before its end reaches past its start, or one before it does. */
static bool RibbonOnGround(const RibbonWalk* w, uint64_t start, uint64_t count)
{
    uint64_t lo = 0, hi = w->nground;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2u;
        if (w->ground[mid].start < start + count)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo > 0 && w->ground[lo - 1u].reach > start;
}

/* `q`, named by `who`, taken as the next_quire when it may be — in the data
 * run, on an atom, and past the Tally the mount stands on clear of the book
 * and of the files (§3.7's table) — said when it may not. The next_quire
 * already taken is not weighed again: a writer names it in every stitch. */
bool RibbonNamed(RibbonWalk* w, const RibbonQuire* q, const char* who)
{
    if (!q->blocks || RibbonQuireSame(q, &w->quire) || RibbonQuireSame(q, &w->next))
        return true;
    if (w->stood && RibbonQuireLies(q) && !RibbonGroundBuilt(w))
        return false;
    if (!RibbonQuireLies(q) ||
        (w->stood && (GroundClaimedBy(GROUND_TALLY, q->block, q->blocks) || RibbonOnGround(w, q->block, q->blocks)))) {
        RibbonSay(w, RIBBON_CODE_RIBBON_TORN, "%s names Quire %llu+%llu as the ribbon's next, which lies outside "
                  "the data run, off an atom, or on the book or a file — the ribbon is not taken past it there",
                  who, (unsigned long long)q->block, (unsigned long long)q->blocks);
        return true;
    }
    w->next = *q;
    return RibbonQuireAdd(w, q);
}

// ----------------------------------------------------------------------------
// A stitch taken
// ----------------------------------------------------------------------------

/* The leads a sound stitch's sectors name that its records do not give them
 * (§2.1, §16.3 п.4): said as RECORD_SHAPE — were a sector before them to rot,
 * the walk in pieces would be taken up in the wrong place. */
static void RibbonLeadsSaid(RibbonWalk* w, const RibbonUnit* u)
{
    char     sectors[96], named[96], given[96];
    size_t   a = 0, b = 0, c = 0;
    uint32_t r = 0, wrong = 0;
    for (uint32_t k = 0; k < u->of; k++) {
        uint32_t lo = k * BOOK_PAYLOAD_BYTES, want = 0;
        while (r <= u->nrecords + 1u) {
            uint32_t start = r == 0 ? 0 : r <= u->nrecords ? u->records[r - 1].at : u->clasp_at;
            if (start >= lo) {
                want = start < lo + BOOK_PAYLOAD_BYTES ? BOOK_HEAD_BYTES + start - lo : 0;
                break;
            }
            r++;
        }
        if (u->leads[k] == want || a + 16u >= sizeof(sectors))
            continue;
        a += (size_t)ksnprintf(sectors + a, sizeof(sectors) - a, wrong ? ", %u" : "%u", k);
        b += (size_t)ksnprintf(named + b, sizeof(named) - b, wrong ? ", %u" : "%u", u->leads[k]);
        c += (size_t)ksnprintf(given + c, sizeof(given) - c, wrong ? ", %u" : "%u", want);
        wrong++;
    }
    if (wrong)
        RibbonSay(w, RIBBON_CODE_RECORD_SHAPE, "stitch %llu's sector(s) %s name lead(s) %s where its records "
                  "begin at %s (§2.1) — were a sector before them to rot, its walk would be taken up in the "
                  "wrong place", (unsigned long long)u->seq, sectors, named, given);
}

/* `u` sewn on: the walk's last stitch from here, its Quire the walk's when it
 * ended in another, the next_quire its StitchHead names weighed. False with no
 * memory (the unit is the walk's either way). */
static bool RibbonTake(RibbonWalk* w, RibbonUnit* u)
{
    RibbonPlace last;
    char        who[48];
    if (w->nstitch == w->capstitch) {
        uint32_t     cap  = w->capstitch ? w->capstitch * 2u : 8u;
        RibbonUnit** more = kmalloc(sizeof(RibbonUnit*) * cap);
        if (!more) {
            RibbonUnitLetGo(u);
            return false;
        }
        if (w->nstitch)
            memcpy(more, w->stitch, sizeof(RibbonUnit*) * w->nstitch);
        kfree(w->stitch);
        w->stitch    = more;
        w->capstitch = cap;
    }
    w->stitch[w->nstitch++] = u;
    w->seq         = u->seq;
    w->clasp       = u->clasp.crc;
    w->clasp_known = u->clasp_read;
    RibbonPlaceOn(&u->start, u->of - 1u, &u->hop, &last);
    if (!RibbonQuireSame(&last.quire, &w->quire)) {
        w->quire = last.quire;
        w->next  = (RibbonQuire){ 0, 0 };
        if (!RibbonQuireAdd(w, &w->quire))
            return false;
    }
    w->has_end = u->has_end;
    w->end     = u->end;
    if (u->head_read) {
        RibbonQuire next = { u->head.next_block, u->head.next_blocks };
        ksnprintf(who, sizeof(who), "stitch seq %llu", (unsigned long long)u->seq);
        if (!RibbonNamed(w, &next, who))
            return false;
        w->lied |= (u->head.flags & STITCH_MEDIUM_LIED) != 0;
    }
    if (u->sound)
        RibbonLeadsSaid(w, u);
    return true;
}

/* Does `u`'s StitchHead name the last stitch taken — by its seq and by its
 * Clasp (§2.7, H-2) — and, begun at the start of the next_quire, the Quire
 * the last one ended in as the one it came from (§4.5 п.5 (б))? A StitchHead
 * that does not read names nothing either way: rule (в) takes such a unit on
 * the word of the stitch after it. */
static bool RibbonFollows(const RibbonWalk* w, const RibbonUnit* u, bool at_next)
{
    if (!u->head_read)
        return true;
    if (!w->clasp_known || u->head.prev_clasp != w->clasp || u->head.prev != (w->seq ? u->seq - w->seq : 0))
        return false;
    return !at_next || (u->head.from_block == w->quire.block && u->head.from_blocks == w->quire.blocks);
}

// ----------------------------------------------------------------------------
// Rules (а)–(г)
// ----------------------------------------------------------------------------

/* Rule (а) gives `here`, rule (б) `there`; when both do, the one of the larger
 * seq is taken — a retry always takes a new, larger seq and carries the same
 * records of its cycle (§16.3 п.11, M-6) — and the other said as the pieces of
 * a retry: no lie, the medium kept a write it refused. The one not taken is
 * let go. */
static RibbonUnit* RibbonRetried(RibbonWalk* w, RibbonUnit* here, RibbonUnit* there)
{
    if (!here || !there || here->seq == there->seq) {
        if (here && there)
            RibbonUnitLetGo(there);
        return here ? here : there;
    }
    RibbonUnit* take = here->seq > there->seq ? here : there;
    RibbonUnit* left = take == here ? there : here;
    RibbonSay(w, RIBBON_CODE_DEAD_BRANCH, "stitch seq %llu at the end of the ribbon and seq %llu at the start of its "
              "next_quire are both whole and both name stitch %llu by its Clasp — a write the medium refused and "
              "kept, and its retry (M-6): seq %llu, the larger, is taken; seq %llu is the pieces of a retry and is "
              "not applied (§16.3, amendment 11)", (unsigned long long)here->seq, (unsigned long long)there->seq,
              (unsigned long long)w->seq, (unsigned long long)take->seq, (unsigned long long)left->seq);
    RibbonUnitLetGo(left);
    return take;
}

/* The lost sectors of a closed, damaged stitch that hold as another unit's —
 * a write out of order, or a lie — into `out`; how many. */
static uint32_t RibbonForeign(const RibbonUnit* u, char* out, size_t room)
{
    uint32_t n  = 0;
    size_t   at = 0;
    out[0] = '\0';
    for (uint32_t k = 0; k < u->of; k++)
        if (!u->valid[k] && u->holds[k] && at + 16u < room)
            at += (size_t)ksnprintf(out + at, room - at, n++ ? ", %u" : "%u", k);
    return n;
}

/* The sectors of a unit that do not hold as its own, into `out`. */
static void RibbonLostSaid(const RibbonUnit* u, char* out, size_t room)
{
    uint32_t n  = 0;
    size_t   at = 0;
    out[0] = '\0';
    for (uint32_t k = 0; k < u->of; k++)
        if (!u->valid[k] && at + 16u < room)
            at += (size_t)ksnprintf(out + at, room - at, n++ ? ", %u" : "%u", k);
}

/*
 * Rule (в): `u` closed and damaged when its StitchHead, if it reads, names the
 * last stitch taken (an identity that rotted lends no stitch of another
 * history, §16.3 п.9), and the sound stitch after it names its seq and, where
 * its Clasp reads, that Clasp. Then both are taken: u said, its lost bytes
 * counted, its foreign sectors a lie when a later stitch landed. *closed
 * whether it was; OK, or the medium's or memory's refusal. `u` stays the
 * caller's when it is not taken.
 */
static error_t RibbonClosed(RibbonWalk* w, RibbonUnit* u, bool at_next, bool* closed)
{
    RibbonUnit* f = NULL;
    uint64_t    names = 0;
    char        lost[96], foreign[96];
    *closed = false;
    if (!RibbonFollows(w, u, at_next) || !u->has_end)
        return OK;
    RibbonQuire own = { u->head.next_block, u->head.next_blocks };
    RibbonQuire hop = u->head_read && RibbonQuireLies(&own) ? own : w->next;
    error_t rc = RibbonUnitFind(&u->end, &hop, u->seq, &f);
    if (rc != OK || !f || !f->sound || !RibbonUnitNames(f, &names) || names != u->seq ||
        (u->clasp_read && f->head.prev_clasp != u->clasp.crc)) {
        RibbonUnitLetGo(f);
        return rc;
    }
    *closed = true;
    RibbonLostSaid(u, lost, sizeof(lost));
    uint32_t nforeign = RibbonForeign(u, foreign, sizeof(foreign));
    uint64_t seq = u->seq, bytes = u->lost;
    uint32_t of  = u->of;
    if (!RibbonTake(w, u)) {
        RibbonUnitLetGo(f);
        return ERR_NO_MEMORY;
    }
    w->lied |= nforeign != 0;
    RibbonSay(w, RIBBON_CODE_STITCH_TORN, "stitch %llu is closed and damaged: sector(s) %s of its %u will not read, "
              "and %llu byte(s) of its records with them; what reads by `lead` is replayed (§3.9)%s%s%s",
              (unsigned long long)seq, lost, of, (unsigned long long)bytes,
              nforeign ? "; sector(s) " : "", nforeign ? foreign : "",
              nforeign ? " hold another unit's — a write out of order or a lie, and a later stitch landed: "
                         "MEDIUM_LIED" : "");
    return RibbonTake(w, f) ? OK : ERR_NO_MEMORY;
}

/* One step of rules (а)–(в): *took whether a stitch was sewn on. */
static error_t RibbonSewOne(RibbonWalk* w, bool* took)
{
    RibbonUnit *u = NULL, *v = NULL, *here = NULL, *there = NULL;
    RibbonPlace head = { w->next, 0 };
    error_t     rc   = RibbonUnitFind(&w->end, &w->next, w->seq, &u);
    if (rc == OK && w->next.blocks && !RibbonQuireSame(&w->end.quire, &w->next))
        rc = RibbonUnitFind(&head, NULL, w->seq, &v);
    *took = false;
    if (rc == OK) {
        here  = u && u->sound && RibbonFollows(w, u, false) ? u : NULL;
        there = v && v->sound && RibbonFollows(w, v, true) ? v : NULL;
    }
    if (here || there) {
        RibbonUnit* take = RibbonRetried(w, here, there);
        if (u != here && u != there)
            RibbonUnitLetGo(u);
        if (v != here && v != there)
            RibbonUnitLetGo(v);
        *took = true;
        return RibbonTake(w, take) ? OK : ERR_NO_MEMORY;
    }
    RibbonUnit* tried[2] = { u, v };
    for (uint32_t i = 0; rc == OK && i < 2 && !*took; i++) {
        RibbonUnit* c = tried[i];
        if (!c || c->sound || c->shape != RIBBON_SHAPE_KEPT)
            continue;
        rc = RibbonClosed(w, c, c == v, took);
        if (*took)
            tried[i] = NULL;
    }
    RibbonUnitLetGo(tried[0]);
    RibbonUnitLetGo(tried[1]);
    return rc;
}

/* Rules (а)–(г) until none takes the next unit, bounded by the seq every
 * stitch raises and by the data run's length. */
static error_t RibbonSew(RibbonWalk* w)
{
    uint64_t hops = 0;
    bool     took = true;
    error_t  rc   = OK;
    while (rc == OK && took && w->has_end && hops <= g_book.data_end) {
        hops++;
        rc = RibbonSewOne(w, &took);
    }
    return rc;
}

// ----------------------------------------------------------------------------
// The direct scan past the end, and what each branch there is (§2.2, §3.9)
// ----------------------------------------------------------------------------

/* The unit of seq `seq` past the end, or NULL. */
static RibbonUnit* RibbonPastOf(const RibbonWalk* w, uint64_t seq)
{
    uint32_t lo = 0, hi = w->npast;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (w->past[mid]->seq == seq)
            return w->past[mid];
        if (w->past[mid]->seq < seq)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return NULL;
}

/* A unit past the end into the walk's list, kept in the order of its seq.
 * False with no memory (the unit let go). */
static bool RibbonPastAdd(RibbonWalk* w, RibbonUnit* u, uint32_t* cap)
{
    if (w->npast == *cap) {
        uint32_t     more = *cap ? *cap * 2u : 8u;
        RibbonUnit** to   = kmalloc(sizeof(RibbonUnit*) * more);
        if (!to) {
            RibbonUnitLetGo(u);
            return false;
        }
        if (w->npast)
            memcpy(to, w->past, sizeof(RibbonUnit*) * w->npast);
        kfree(w->past);
        w->past = to;
        *cap    = more;
    }
    uint32_t at = w->npast;
    while (at > 0 && w->past[at - 1u]->seq > u->seq) {
        w->past[at] = w->past[at - 1u];
        at--;
    }
    w->past[at] = u;
    w->npast++;
    return true;
}

/* Why a unit is no stitch the format allows (§16.3 п.8), in stitch.py's
 * words (Misshapen). */
static void RibbonShapeSaid(const RibbonUnit* u, char* out, size_t room)
{
    uint64_t space = RibbonPlaceLeft(&u->start) + u->hop.blocks * VOLUME_BLOCK_SECTORS;
    if (u->shape == RIBBON_SHAPE_ATOMS)
        ksnprintf(out, room, "%u sector(s) long — no whole number of atoms of %u", u->of, VOLUME_BLOCK_SECTORS);
    else if (u->shape == RIBBON_SHAPE_OFF)
        ksnprintf(out, room, "begun at sector %llu of its Quire, off a block's boundary",
                  (unsigned long long)u->start.off);
    else if (u->shape == RIBBON_SHAPE_LONG)
        ksnprintf(out, room, "%u sectors long, more than its Quire and the next hold (%llu)", u->of,
                  (unsigned long long)space);
    else
        ksnprintf(out, room, "%u sectors long — a stream of more than %u bytes, past what a reader holds a "
                  "stitch in", u->of, RIBBON_STREAM_MOST);
}

/* Every sector of the rest of the Quire the chain ends in and all of its
 * next_quire: each unit of a seq past the chain's that begins there, read
 * whole — the first sector of a seq that lies at or past its own `at` from
 * the end names where it begins. */
static error_t RibbonScanUnits(RibbonWalk* w, uint32_t* cap)
{
    uint64_t room = RibbonPlaceLeft(&w->end) + w->next.blocks * VOLUME_BLOCK_SECTORS;
    for (uint64_t i = 0; i < room;) {
        RibbonPlace p;
        BookUnit    run;
        if (!RibbonPlaceOn(&w->end, i, &w->next, &p))
            break;
        uint64_t n  = RibbonPlaceLeft(&p) < 256u ? RibbonPlaceLeft(&p) : 256u;
        error_t  rc = BookUnitRead(RibbonPlaceVlba(&p), (uint32_t)n, 0, 0, &run);
        for (uint32_t k = 0; rc == OK && k < n; k++) {
            BookSectorHead h;
            RibbonPlace    at;
            RibbonUnit*    u = NULL;
            memcpy(&h, run.sectors + (uint64_t)k * BOOK_SECTOR_BYTES, sizeof(h));
            if (run.state[k] != BOOK_SECTOR_HOLDS || h.kind != BOOK_UNIT_STITCH || h.seq <= w->seq ||
                i + k < h.at || RibbonPastOf(w, h.seq))
                continue;
            RibbonPlaceOn(&w->end, i + k - h.at, &w->next, &at);
            rc = RibbonUnitRead(&at, h.of, h.seq, &w->next, &u);
            if (rc == OK && !RibbonPastAdd(w, u, cap))
                rc = ERR_NO_MEMORY;
        }
        BookUnitLetGo(&run);
        if (rc != OK)
            return rc;
        i += n;
    }
    return OK;
}

/* The units past the end no stitch is shaped like: said, and out of the
 * list — the ribbon is not taken past one, and its records are not read. */
static void RibbonScanShapes(RibbonWalk* w)
{
    uint32_t kept = 0;
    char     why[96];
    for (uint32_t i = 0; i < w->npast; i++) {
        RibbonUnit* u = w->past[i];
        if (u->shape == RIBBON_SHAPE_KEPT) {
            w->past[kept++] = u;
            continue;
        }
        RibbonShapeSaid(u, why, sizeof(why));
        RibbonSay(w, RIBBON_CODE_RECORD_SHAPE, "a unit of the ribbon at sector %llu, seq %llu, is %s: no stitch is "
                  "so shaped (§1.5, §2.2) — the ribbon is not taken past it, and its records are not read",
                  (unsigned long long)RibbonPlaceVlba(&u->start), (unsigned long long)u->seq, why);
        RibbonUnitLetGo(u);
    }
    w->npast = kept;
}

/* What the chain holds of stitch `seq` — one sewn on, or the one the Tally
 * took in: true, its Clasp in *clasp and whether it reads in *known. */
static bool RibbonSewn(const RibbonWalk* w, uint64_t seq, uint32_t* clasp, bool* known)
{
    if (seq == w->slot->head.stitch_seq) {
        *clasp = w->slot->head.stitch_clasp;
        *known = true;
        return true;
    }
    for (uint32_t i = 0; i < w->nstitch; i++) {
        if (w->stitch[i]->seq == seq) {
            *clasp = w->stitch[i]->clasp.crc;
            *known = w->stitch[i]->clasp_read;
            return true;
        }
    }
    return false;
}

/* Whether a member of a branch names `seq` past the chain's end that is not
 * on the medium whole — what settled and is lost. */
static bool RibbonLostNamed(const RibbonWalk* w, uint64_t seq)
{
    RibbonUnit* there = RibbonPastOf(w, seq);
    return seq > w->seq && (!there || !there->sound);
}

/* The first member of a branch, in the order of its seq, that names what
 * settled and is lost: NULL when none does. */
static RibbonUnit* RibbonLostNamer(const RibbonWalk* w, RibbonUnit* const* m, uint32_t nm)
{
    for (uint32_t i = 0; i < nm; i++) {
        uint64_t x = 0;
        if (RibbonUnitNames(m[i], &x) && RibbonLostNamed(w, x))
            return m[i];
    }
    return NULL;
}

/* A branch some unit of which (`namer`, the first) names one that settled —
 * for it began only after that one's flush — and is not on the medium whole
 * (H-3); and when its root, whole, names one the medium does not hold at all,
 * the lie is proven (FAST'13, H-2). Both lie: MEDIUM_LIED. */
static void RibbonLost(RibbonWalk* w, const RibbonUnit* r, const RibbonUnit* namer, const char* seqs)
{
    uint64_t named = 0, x = 0;
    if (r->sound && RibbonUnitNames(r, &named) && named > w->seq && !RibbonPastOf(w, named)) {
        RibbonSay(w, RIBBON_CODE_DEAD_BRANCH, "the lie is proven: stitch seq %llu lies past the end of the ribbon "
                  "and names seq %llu, which the medium does not hold — a flush that lied, the cycles after it "
                  "written over a hole (FAST'13, H-2); seq %s are not applied — MEDIUM_LIED",
                  (unsigned long long)r->seq, (unsigned long long)named, seqs);
        return;
    }
    RibbonUnitNames(namer, &x);
    RibbonSay(w, RIBBON_CODE_DEAD_BRANCH, "what settled is lost: seq %s past the end of the ribbon — stitch seq %llu "
              "names %llu, begun only after %llu had settled, and %llu %s (H-3) — MEDIUM_LIED", seqs,
              (unsigned long long)namer->seq, (unsigned long long)x, (unsigned long long)x, (unsigned long long)x,
              RibbonPastOf(w, x) ? "does not read whole" : "is not on the medium");
}

/* The stitch right after the end, torn, and nothing past it naming it: a cut
 * while it was written — or, every sector of it there and some sum or its
 * Clasp not holding, a cut or rot, said loud (M-4). */
static void RibbonTornSaid(RibbonWalk* w, const RibbonUnit* t)
{
    uint32_t held = 0;
    for (uint32_t k = 0; k < t->of; k++)
        held += t->valid[k] ? 1u : 0u;
    if (RibbonUnitWhole(t))
        RibbonSay(w, RIBBON_CODE_RIBBON_TORN, "the last cycle does not read whole: stitch seq %llu has all %u "
                  "sectors and its stream or its Clasp does not hold — a cut or rot (M-4)",
                  (unsigned long long)t->seq, t->of);
    else
        RibbonSay(w, RIBBON_CODE_RIBBON_TORN, "the ribbon ends on stitch %llu; seq %llu past it is torn — %u of "
                  "its %u sectors hold, a cut while it was written; its numbers are not given again",
                  (unsigned long long)w->seq, (unsigned long long)t->seq, held, t->of);
}

/* A whole root past the end, naming a stitch of the chain by that stitch's
 * own Clasp: the end itself from where the ribbon does not go on, or one
 * before it — the ribbon forks (§16.3 п.10). Settled, not taken, no lie. */
static void RibbonNamesChain(RibbonWalk* w, const RibbonUnit* r, uint64_t n, const char* seqs)
{
    if (n == w->seq) {
        RibbonSay(w, RIBBON_CODE_DEAD_BRANCH, "stitch seq %llu names the end of the ribbon, seq %llu, by its Clasp, "
                  "and lies where the ribbon does not go on — not the first atom after it, nor the start of its "
                  "next_quire naming its Quire (§4.5, step 5): seq %s, settled, are not taken",
                  (unsigned long long)r->seq, (unsigned long long)w->seq, seqs);
        return;
    }
    uint64_t after = n;
    for (uint32_t i = 0; i < w->nstitch; i++) {
        if (w->stitch[i]->seq > n) {
            after = w->stitch[i]->seq;
            break;
        }
    }
    RibbonSay(w, RIBBON_CODE_DEAD_BRANCH, "the ribbon forks at stitch %llu: seq %llu names it by its Clasp, as seq "
              "%llu of the ribbon does, and lies past the ribbon's end — seq %s, settled, are not taken (§16.3, "
              "amendment 10)",
              (unsigned long long)n, (unsigned long long)r->seq, (unsigned long long)after, seqs);
}

/* A root naming a stitch of the chain by a Clasp not its own, a seq the
 * chain does not hold at or before its end, or — by a prev past its own seq —
 * no stitch at all: a dead branch of another history (H-2) — said, not sewn
 * on, no lie. */
static void RibbonStranger(RibbonWalk* w, const RibbonUnit* r, uint64_t n, bool sewn, uint32_t clasp,
                           const char* seqs)
{
    char how[64];
    if (r->head.prev > r->seq) {
        RibbonSay(w, RIBBON_CODE_CLASP_STRANGER, "seq %s past the end of the ribbon: a dead branch of another "
                  "history — stitch seq %llu names, by a prev of %llu past its own seq, no stitch this ribbon "
                  "does hold; it is not sewn on", seqs, (unsigned long long)r->seq,
                  (unsigned long long)r->head.prev);
        return;
    }
    if (sewn)
        ksnprintf(how, sizeof(how), "a Clasp that is not its own (0x%08X, not 0x%08X)", r->head.prev_clasp, clasp);
    else
        ksnprintf(how, sizeof(how), "which this ribbon does not hold");
    RibbonSay(w, RIBBON_CODE_CLASP_STRANGER, "seq %s past the end of the ribbon: a dead branch of another history — "
              "stitch seq %llu names stitch %llu with %s; it is not sewn on", seqs, (unsigned long long)r->seq,
              (unsigned long long)n, how);
}

/* What one branch past the end is (§3.9's table, §16.3 п.10), said once, by
 * what its first unit `r` names and what its units name of each other. */
static void RibbonBranch(RibbonWalk* w, RibbonUnit* r, RibbonUnit* const* m, uint32_t nm)
{
    char        seqs[160];
    uint64_t    n     = 0;
    uint32_t    clasp = 0;
    bool        known = false, has = RibbonUnitNames(r, &n), tail = w->torn && r->seq == w->torn->seq;
    bool        sewn  = has && RibbonSewn(w, n, &clasp, &known);
    bool        ours  = sewn && (!known || r->head.prev_clasp == clasp);
    RibbonUnit* namer = RibbonLostNamer(w, m, nm);
    bool        below = r->head_read && r->head.prev > r->seq;
    RibbonSeqsSaid(m, nm, seqs, sizeof(seqs));
    if (below || (has && !ours && (sewn || n <= w->seq))) {
        if (tail) {
            RibbonUnitLetGo(w->torn);
            w->torn = NULL;
        }
        RibbonStranger(w, r, n, sewn, clasp, seqs);
    } else if (namer) {
        w->lied = true;
        RibbonLost(w, r, namer, seqs);
    } else if (tail && (!has || n == w->seq)) {
        RibbonTornSaid(w, w->torn);
    } else if (!has) {
        RibbonSay(w, RIBBON_CODE_DEAD_BRANCH, "past the end of the ribbon lie pieces of seq %s whose StitchHead does "
                  "not read: a retry, or nothing known — no loss of what settled is shown", seqs);
    } else if (!r->sound) {
        RibbonSay(w, RIBBON_CODE_DEAD_BRANCH, "past the end of the ribbon lie pieces of seq %s naming stitch %llu of "
                  "it: a retry cut while it was written — no loss of what settled is shown", seqs,
                  (unsigned long long)n);
    } else {
        RibbonNamesChain(w, r, n, seqs);
    }
}

/* The units past the end joined into branches, each to the unit its
 * StitchHead names when that one lies past the end too (RibbonUnitNames names
 * only a seq below the namer's, so the join ends), and each branch judged by
 * its root in the order of its seq. */
static error_t RibbonBranches(RibbonWalk* w)
{
    uint32_t     n       = w->npast ? w->npast : 1u;
    RibbonUnit** root    = kmalloc(sizeof(RibbonUnit*) * n);
    RibbonUnit** members = kmalloc(sizeof(RibbonUnit*) * n);
    if (!root || !members) {
        kfree(root);
        kfree(members);
        return ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < w->npast; i++) {
        RibbonUnit* r = w->past[i];
        uint64_t    named;
        while (RibbonUnitNames(r, &named) && RibbonPastOf(w, named))
            r = RibbonPastOf(w, named);
        root[i] = r;
    }
    for (uint32_t r = 0; r < w->npast; r++) {
        uint32_t nm = 0;
        for (uint32_t i = 0; i < w->npast; i++)
            if (root[i] == w->past[r])
                members[nm++] = w->past[i];
        if (nm)
            RibbonBranch(w, w->past[r], members, nm);
    }
    kfree(root);
    kfree(members);
    return OK;
}

/* The direct scan (§2.2): every unit past the chain's end, the torn tail,
 * and what each branch there is. */
static error_t RibbonScan(RibbonWalk* w)
{
    RibbonUnit* tail = NULL;
    uint32_t    cap  = 0;
    if (!w->has_end)
        return OK;
    error_t rc = RibbonScanUnits(w, &cap);
    if (rc != OK)
        return rc;
    RibbonScanShapes(w);
    rc = RibbonUnitFind(&w->end, &w->next, w->seq, &tail);
    if (rc != OK)
        return rc;
    if (tail && !tail->sound && tail->shape == RIBBON_SHAPE_KEPT)
        w->torn = tail;
    else
        RibbonUnitLetGo(tail);
    return RibbonBranches(w);
}

// ----------------------------------------------------------------------------
// The walk
// ----------------------------------------------------------------------------

/* The numbers every record read gives — of a stitch sewn on, applied or not,
 * of the torn tail and of what lies past the end: never given again (§4.5
 * п.8). */
static void RibbonSeenOf(RibbonWalk* w, const RibbonUnit* u)
{
    for (uint32_t i = 0; u && i < u->nrecords; i++) {
        const RibbonRecord* r = &u->records[i];
        const uint8_t*      b = u->stream + r->body;
        uint64_t            id = 0;
        uint32_t            used = 0;
        TallyTag            tag;
        RibbonBorn          born;
        if (r->kind == RIBBON_BORN && RibbonBornGet(b, r->len, u->seq, &born, &used) != TALLY_BODY_BROKEN) {
            if (born.fid + 1u > w->seen_fid)
                w->seen_fid = born.fid + 1u;
        } else if (r->kind == RIBBON_COINED && RibbonCoinedGet(b, r->len, &id, &tag, &used) != TALLY_BODY_BROKEN) {
            if (id + 1u > w->seen_tag)
                w->seen_tag = id + 1u;
        } else if (r->kind == RIBBON_FROZEN && RibbonFrozenGet(b, r->len, &id, &used) != TALLY_BODY_BROKEN) {
            if (id + 1u > w->seen_bookmark)
                w->seen_bookmark = id + 1u;
        }
    }
}

error_t RibbonWalkBegun(const SpineSlot* slot, bool stood, RibbonWalk** out)
{
    const SpineRibbon* r = &slot->ribbon;
    char               who[48];
    RibbonWalk*        w = kmalloc(sizeof(RibbonWalk));
    *out = w;
    if (!w)
        return ERR_NO_MEMORY;
    memset(w, 0, sizeof(*w));
    w->slot        = slot;
    w->stood       = stood;
    w->lied        = (slot->flags & SPINE_MEDIUM_LIED) != 0;
    w->seq         = slot->head.stitch_seq;
    w->clasp       = slot->head.stitch_clasp;
    w->clasp_known = true;
    w->quire       = (RibbonQuire){ r->quire_block, r->quire_blocks };
    if (!RibbonQuireLies(&w->quire) || r->at < r->quire_block || r->at - r->quire_block >= r->quire_blocks) {
        RibbonSay(w, RIBBON_CODE_RIBBON_TORN, "slot %u puts the ribbon on at block %llu of the Quire %llu+%llu, "
                  "which does not hold it or lies outside the data run or off an atom — no stitch past its Tally "
                  "is read", slot->index, (unsigned long long)r->at, (unsigned long long)r->quire_block,
                  (unsigned long long)r->quire_blocks);
        return OK;
    }
    w->end     = (RibbonPlace){ w->quire, (r->at - r->quire_block) * VOLUME_BLOCK_SECTORS };
    w->has_end = true;
    RibbonQuire next = { r->next_block, r->next_blocks };
    ksnprintf(who, sizeof(who), "slot %u's SpineRibbon", slot->index);
    return RibbonQuireAdd(w, &w->quire) && RibbonNamed(w, &next, who) ? OK : ERR_NO_MEMORY;
}

error_t RibbonCollate(const SpineSlot* slot, RibbonWalk** out)
{
    error_t     rc = RibbonWalkBegun(slot, true, out);
    RibbonWalk* w  = *out;
    if (rc == OK)
        rc = RibbonSew(w);
    if (rc == OK)
        rc = RibbonScan(w);
    if (w) {
        kfree(w->ground);
        w->ground  = NULL;
        w->nground = 0;
    }
    for (uint32_t i = 0; rc == OK && i < w->nstitch; i++)
        RibbonSeenOf(w, w->stitch[i]);
    if (rc == OK) {
        RibbonSeenOf(w, w->torn);
        for (uint32_t i = 0; i < w->npast; i++)
            RibbonSeenOf(w, w->past[i]);
    }
    return rc;
}
