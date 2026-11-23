#include "ribbon.h"
#include "book_mount.h"
#include "book_unit.h"
#include "ground_census.h"
#include "../file_map/file_map.h"
#include "tagfs_volume.h"

/*
 * RibbonCollate — pass 1 of the ribbon: from the `at` of a slot, the stitches
 * sewn on, one after another, by the Clasp each names (H-2) — rules (а)–(г) —
 * and the Quires the chain runs through; then, for the slot the mount stands
 * on, the direct scan of what lies past the end of the chain (§2.2) and what
 * each branch there is (§3.9, §16.3 пп.10–11), made by ribbon_scan.c. Nothing
 * is applied here: pass 2 replays what pass 1 sewed on (ribbon_transcribe.c).
 *
 * ‼ THE CHAIN IS SEWN BY IDENTITY, NOT BY SEQ. A stitch is taken only when its
 * StitchHead names the last one taken by its seq AND its Clasp: a dead branch
 * of another session, whatever seq it carries, does not fit (ZIL's GUID+SEQ
 * in a block's sum, zil.h:109-115). The walk is bounded twice: every stitch
 * taken has a larger seq than the last, and no more are taken than the data
 * run has blocks (`hops >= data_blocks`).
 *
 * ‼ THE NEXT UNIT IS WHERE THE LAST ONE ENDS BY ITS WRITER'S ATOM (§16.2
 * п.11): `start + of`, never this mount's atom — a volume moved onto a medium
 * of another physical block is read the same.
 *
 * The rules and the lines are volume-read's (tools/volumeread/stitch_ribbon.py
 * Ribbon), word for word where the kernel can say them so: the census scenario
 * plants every rule and holds the two readers to one answer.
 */

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
    [RIBBON_CODE_RETIRED_WORN]    = "RETIRED_WORN",
};

void RibbonSay(TagFSVolume* vol, RibbonWalk* w, RibbonCode code, const char* fmt, ...)
{
    va_list ap;
    w->said[code]++;
    if (!w->stood)
        return;
    vol->book.said.ribbon[code]++;
    va_start(ap, fmt);
    kvsnprintf(vol->ribbon_collate.line, sizeof(vol->ribbon_collate.line), fmt, ap);
    va_end(ap);
    kprintf("[TagFS] ribbon %s: %s\n", g_ribbon_codes[code], vol->ribbon_collate.line);
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
    RibbonUnitLetGo(w->ahead);
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

/* A Quire on the grid of the volume's own atom — the one it was laid out for,
 * GEOMETRY's — never the medium's this mount found (§16.2 п.11: the reader's
 * rules are the volume's, not the mount's): a volume moved onto a medium of a
 * coarser block reads the Quires its writers laid, one rule with volume-read
 * (stitch.py Lies). The writer goes on in none off the medium's atom
 * (ribbon.c, RibbonGoesOnAt; §1.5). */
bool RibbonQuireLies(TagFSVolume* vol, const RibbonQuire* q)
{
    uint64_t lo = vol->book.frame.data_block, hi = vol->book.data_end;
    uint64_t grid = BookAtomOf(vol->state.geometry.physical_bytes, 0, 0) / TAGFS_BLOCK_SIZE;
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
static bool RibbonGroundBuilt(TagFSVolume* vol, RibbonWalk* w)
{
    TagFSState* fs   = &vol->state;
    uint64_t    from = 0, cap = 0, ids[16];
    uint32_t    n;
    if (w->ground_built)
        return true;
    while ((n = FileMapFiles(fs->files, from, ids, 16)) > 0) {
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
bool RibbonNamed(TagFSVolume* vol, RibbonWalk* w, const RibbonQuire* q, const char* who)
{
    if (!q->blocks || RibbonQuireSame(q, &w->quire) || RibbonQuireSame(q, &w->next))
        return true;
    if (w->stood && RibbonQuireLies(vol, q) && !RibbonGroundBuilt(vol, w))
        return false;
    if (!RibbonQuireLies(vol, q) ||
        (w->stood && (GroundClaimedBy(vol, GROUND_TALLY, q->block, q->blocks) || RibbonOnGround(w, q->block, q->blocks)))) {
        RibbonSay(vol, w, RIBBON_CODE_RIBBON_TORN, "%s names Quire %llu+%llu as the ribbon's next, which lies outside "
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
static void RibbonLeadsSaid(TagFSVolume* vol, RibbonWalk* w, const RibbonUnit* u)
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
        RibbonSay(vol, w, RIBBON_CODE_RECORD_SHAPE, "stitch %llu's sector(s) %s name lead(s) %s where its records "
                  "begin at %s (§2.1) — were a sector before them to rot, its walk would be taken up in the "
                  "wrong place", (unsigned long long)u->seq, sectors, named, given);
}

/* `u` sewn on: the walk's last stitch from here, its Quire the walk's when it
 * ended in another, the next_quire its StitchHead names weighed. False with no
 * memory (the unit is the walk's either way). */
static bool RibbonTake(TagFSVolume* vol, RibbonWalk* w, RibbonUnit* u)
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
        if (!RibbonNamed(vol, w, &next, who))
            return false;
        w->lied |= (u->head.flags & STITCH_MEDIUM_LIED) != 0;
    }
    if (u->sound)
        RibbonLeadsSaid(vol, w, u);
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

/* Rule (а) gives `here`, rules (б) and (б′) `there`; when both do, the one of
 * the larger seq is taken — a retry always takes a new, larger seq and
 * carries the same records of its cycle (§16.3 п.11, M-6) — and the other
 * said as the pieces of a retry: no lie, the medium kept a write it refused.
 * The one not taken is let go. */
static RibbonUnit* RibbonRetried(TagFSVolume* vol, RibbonWalk* w, RibbonUnit* here, RibbonUnit* there)
{
    if (!here || !there || here->seq == there->seq) {
        if (here && there)
            RibbonUnitLetGo(there);
        return here ? here : there;
    }
    RibbonUnit* take = here->seq > there->seq ? here : there;
    RibbonUnit* left = take == here ? there : here;
    RibbonSay(vol, w, RIBBON_CODE_DEAD_BRANCH, "stitch seq %llu at the end of the ribbon and seq %llu in its "
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
static error_t RibbonClosed(TagFSVolume* vol, RibbonWalk* w, RibbonUnit* u, bool at_next, bool* closed)
{
    RibbonUnit* f = NULL;
    uint64_t    names = 0;
    char        lost[96], foreign[96];
    *closed = false;
    if (!RibbonFollows(w, u, at_next) || !u->has_end)
        return OK;
    RibbonQuire own = { u->head.next_block, u->head.next_blocks };
    RibbonQuire hop = u->head_read && RibbonQuireLies(vol, &own) ? own : w->next;
    error_t rc = RibbonUnitFind(vol, &u->end, &hop, u->seq, &f);
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
    if (!RibbonTake(vol, w, u)) {
        RibbonUnitLetGo(f);
        return ERR_NO_MEMORY;
    }
    w->lied |= nforeign != 0;
    RibbonSay(vol, w, RIBBON_CODE_STITCH_TORN, "stitch %llu is closed and damaged: sector(s) %s of its %u will not read, "
              "and %llu byte(s) of its records with them; what reads by `lead` is replayed (§3.9)%s%s%s",
              (unsigned long long)seq, lost, of, (unsigned long long)bytes,
              nforeign ? "; sector(s) " : "", nforeign ? foreign : "",
              nforeign ? " hold another unit's — a write out of order or a lie, and a later stitch landed: "
                         "MEDIUM_LIED" : "");
    return RibbonTake(vol, w, f) ? OK : ERR_NO_MEMORY;
}

/* `seq` into the list of what a walk passed over, once in a row. */
static void RibbonPassedAdd(char* out, size_t room, size_t* at, uint64_t* last, uint64_t seq)
{
    if (seq == *last || *at + 24u >= room)
        return;
    *at  += (size_t)ksnprintf(out + *at, room - *at, *at ? ", %llu" : "%llu", (unsigned long long)seq);
    *last = seq;
}

/* Past the stitch `take` found in the next_quire, while a whole stitch naming
 * the last one taken, with a larger seq, lies right after it — a try refused
 * that landed, and the retry after it (§16.3 п.11) — that one in its place,
 * the one it replaces into the list passed. */
static error_t RibbonFurtherOn(TagFSVolume* vol, RibbonWalk* w, RibbonUnit** take, char* passed, size_t room, size_t* at,
                               uint64_t* last)
{
    while (*take && (*take)->has_end) {
        RibbonUnit* f   = NULL;
        RibbonQuire own = { (*take)->head.next_block, (*take)->head.next_blocks };
        error_t     rc  = RibbonUnitFind(vol, &(*take)->end, RibbonQuireLies(vol, &own) ? &own : NULL, (*take)->seq, &f);
        if (rc != OK || !f || !f->sound || !RibbonFollows(w, f, true)) {
            RibbonUnitLetGo(f);
            return rc;
        }
        RibbonPassedAdd(passed, room, at, last, (*take)->seq);
        RibbonUnitLetGo(*take);
        *take = f;
    }
    return OK;
}

/*
 * Rules (б) and (б′) (§4.5 п.5, §16.4 п.4): the stitch the ribbon goes on
 * with in the next_quire — the whole one at its start naming the last stitch
 * taken by its Clasp and the Quire it ended in as the one it came from, or,
 * where none lies there, the first such further in: a unit holding a sector
 * there stepped over to its end, a sector holding none of a seq past the last
 * one stepped over alone — a retry is written on the atom past the try refused
 * before it, never over it (settle/ribbon.c, RibbonRetry). What was walked
 * over is said as the pieces of a retry: no lie. The walk ends at the
 * next_quire's end. *out NULL when nothing goes on; *blank when no sector
 * there holds a stitch of a seq past the last one — then no unit begins at its
 * start either, and rule (в) need not look for one.
 */
static error_t RibbonFurther(TagFSVolume* vol, RibbonWalk* w, RibbonUnit** out, bool* blank)
{
    RibbonQuire n     = w->next;
    uint64_t    total = n.blocks * VOLUME_BLOCK_SECTORS, pos = 0, last = 0;
    char        passed[96];
    size_t      at = 0;
    error_t     rc = OK;
    passed[0] = '\0';
    *out      = NULL;
    *blank    = true;
    while (rc == OK && pos < total && !*out) {
        RibbonPlace    p = { n, pos };
        BookSectorHead h;
        RibbonUnit*    u = NULL;
        uint64_t       j = 0;
        bool           found = false;
        rc = RibbonUnitAhead(vol, &p, w->seq, &j, &h, &found, &u);
        if (rc != OK || !found)
            break;
        *blank = false;
        if (u && u->sound && RibbonFollows(w, u, true)) {
            *out = u;
            break;
        }
        RibbonUnitLetGo(u);
        RibbonPassedAdd(passed, sizeof(passed), &at, &last, h.seq);
        pos += j + (h.at < h.of ? (uint64_t)h.of - h.at : 1u);
    }
    if (rc == OK)
        rc = RibbonFurtherOn(vol, w, out, passed, sizeof(passed), &at, &last);
    if (rc != OK) {
        RibbonUnitLetGo(*out);
        *out = NULL;
        return rc;
    }
    if (*out && (!RibbonQuireSame(&(*out)->start.quire, &n) || (*out)->start.off))
        RibbonSay(vol, w, RIBBON_CODE_DEAD_BRANCH, "no whole stitch at the start of the next_quire %llu+%llu names stitch "
                  "%llu and goes on from there: seq %s walked over to seq %llu at its sector %llu, which names it "
                  "and the Quire the first try came from — the pieces of a retry, not applied (§16.4 п.4)",
                  (unsigned long long)n.block, (unsigned long long)n.blocks, (unsigned long long)w->seq,
                  at ? passed : "none that reads", (unsigned long long)(*out)->seq,
                  (unsigned long long)(*out)->start.off);
    return OK;
}

/* Whether `u`'s StitchHead, read, names stitch `s` by its seq and its Clasp. */
static bool RibbonNamesStitch(const RibbonUnit* u, const RibbonUnit* s)
{
    uint64_t names = 0;
    return u && RibbonUnitNames(u, &names) && names == s->seq && s->clasp_read && u->head.prev_clasp == s->clasp.crc;
}

/*
 * ‼ RULES (б) AND (б′) ARE WEIGHED ONLY WHERE NOTHING SAYS THE STITCH RULE (а)
 * GIVES WAS KEPT (§16.4 п.4). A writer lays nothing that names a try the
 * medium refused — a retry names the stitch before it (settle/ribbon.c,
 * RibbonRetry) — so a unit past `s` whose StitchHead names it, at its end or
 * at the start of the next_quire it names, says the writer was told `s` was
 * kept, and no retry of the stitch before it lies anywhere. Without this, the
 * next_quire would be walked unit by unit for every stitch of the Quire before
 * it, where only the last one can have a retry. *kept so; the unit at its end
 * into *ahead, read through *hop — the next step's rule (а) when the walk
 * takes `s` on through that Quire: every unit of the chain is read once
 * (stitch_ribbon.py Ribbon._Kept).
 */
static error_t RibbonKept(TagFSVolume* vol, const RibbonWalk* w, const RibbonUnit* s, bool* kept, RibbonUnit** ahead, RibbonQuire* hop)
{
    RibbonQuire own  = { s->head.next_block, s->head.next_blocks };
    RibbonQuire none = { 0, 0 };
    RibbonPlace last;
    RibbonUnit* v    = NULL;
    error_t     rc   = OK;
    *kept  = false;
    *ahead = NULL;
    *hop   = none;
    if (!s->has_end)
        return OK;
    (void)RibbonPlaceOn(&s->start, s->of - 1u, &s->hop, &last);
    *hop = RibbonQuireLies(vol, &own) ? own : RibbonQuireSame(&last.quire, &w->quire) ? w->next : none;
    rc    = RibbonUnitFind(vol, &s->end, hop, s->seq, ahead);
    *kept = rc == OK && RibbonNamesStitch(*ahead, s);
    if (rc == OK && !*kept && hop->blocks && !RibbonQuireSame(&s->end.quire, hop)) {
        RibbonPlace head = { *hop, 0 };
        rc    = RibbonUnitFind(vol, &head, NULL, s->seq, &v);
        *kept = rc == OK && RibbonNamesStitch(v, s);
        RibbonUnitLetGo(v);
    }
    return rc;
}

/* The unit rule (а) weighs: the one read at the end of the stitch last taken
 * while it was weighed, else looked for there now. */
static error_t RibbonAt(TagFSVolume* vol, RibbonWalk* w, RibbonUnit** u)
{
    if (!w->ahead_read)
        return RibbonUnitFind(vol, &w->end, &w->next, w->seq, u);
    *u            = w->ahead;
    w->ahead      = NULL;
    w->ahead_read = false;
    return OK;
}

/* `take` sewn on, and what was read past it kept for the next step when the
 * walk goes on through the Quire it was read through. */
static error_t RibbonTakeOn(TagFSVolume* vol, RibbonWalk* w, RibbonUnit* take, RibbonUnit* ahead, const RibbonQuire* hop, bool read)
{
    if (!RibbonTake(vol, w, take)) {
        RibbonUnitLetGo(ahead);
        return ERR_NO_MEMORY;
    }
    if (read && RibbonQuireSame(hop, &w->next)) {
        w->ahead      = ahead;
        w->ahead_read = true;
    } else {
        RibbonUnitLetGo(ahead);
    }
    return OK;
}

/* One step of rules (а)–(в): *took whether a stitch was sewn on. */
static error_t RibbonSewOne(TagFSVolume* vol, RibbonWalk* w, bool* took)
{
    RibbonUnit *u = NULL, *v = NULL, *here = NULL, *there = NULL, *ahead = NULL;
    RibbonQuire hop  = { 0, 0 };
    bool        kept = false, read = false, blank = true;
    error_t     rc   = RibbonAt(vol, w, &u);
    *took = false;
    if (rc == OK)
        here = u && u->sound && RibbonFollows(w, u, false) ? u : NULL;
    if (rc == OK && here && w->next.blocks) {
        rc   = RibbonKept(vol, w, here, &kept, &ahead, &hop);
        read = rc == OK && here->has_end;
    }
    if (rc == OK && w->next.blocks && !kept)
        rc = RibbonFurther(vol, w, &there, &blank);
    if (rc == OK && !here && !there && !blank && !RibbonQuireSame(&w->end.quire, &w->next)) {
        RibbonPlace head = { w->next, 0 };
        rc = RibbonUnitFind(vol, &head, NULL, w->seq, &v);
    }
    if (rc == OK && (here || there)) {
        RibbonUnit* take = RibbonRetried(vol, w, here, there);
        if (u != here)
            RibbonUnitLetGo(u);
        *took = true;
        return RibbonTakeOn(vol, w, take, ahead, &hop, read && take == here);
    }
    RibbonUnitLetGo(ahead);
    if (rc != OK) {
        if (u != here)
            RibbonUnitLetGo(u);
        RibbonUnitLetGo(here);
        RibbonUnitLetGo(there);
        RibbonUnitLetGo(v);
        return rc;
    }
    RibbonUnit* tried[2] = { u, v };
    for (uint32_t i = 0; rc == OK && i < 2 && !*took; i++) {
        RibbonUnit* c = tried[i];
        if (!c || c->sound || c->shape != RIBBON_SHAPE_KEPT)
            continue;
        rc = RibbonClosed(vol, w, c, c == v, took);
        if (*took)
            tried[i] = NULL;
    }
    RibbonUnitLetGo(tried[0]);
    RibbonUnitLetGo(tried[1]);
    return rc;
}

/* Rules (а)–(г) until none takes the next unit, bounded by the seq every
 * stitch raises and by the data run's length. */
static error_t RibbonSew(TagFSVolume* vol, RibbonWalk* w)
{
    uint64_t hops = 0;
    bool     took = true;
    error_t  rc   = OK;
    while (rc == OK && took && w->has_end && hops <= vol->book.data_end) {
        hops++;
        rc = RibbonSewOne(vol, w, &took);
    }
    RibbonUnitLetGo(w->ahead);
    w->ahead      = NULL;
    w->ahead_read = false;
    return rc;
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

error_t RibbonWalkBegun(TagFSVolume* vol, const SpineSlot* slot, bool stood, RibbonWalk** out)
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
    if (!RibbonQuireLies(vol, &w->quire) || r->at < r->quire_block || r->at - r->quire_block >= r->quire_blocks) {
        RibbonSay(vol, w, RIBBON_CODE_RIBBON_TORN, "slot %u puts the ribbon on at block %llu of the Quire %llu+%llu, "
                  "which does not hold it or lies outside the data run or off an atom — no stitch past its Tally "
                  "is read", slot->index, (unsigned long long)r->at, (unsigned long long)r->quire_block,
                  (unsigned long long)r->quire_blocks);
        return OK;
    }
    w->end     = (RibbonPlace){ w->quire, (r->at - r->quire_block) * VOLUME_BLOCK_SECTORS };
    w->has_end = true;
    RibbonQuire next = { r->next_block, r->next_blocks };
    ksnprintf(who, sizeof(who), "slot %u's SpineRibbon", slot->index);
    return RibbonQuireAdd(w, &w->quire) && RibbonNamed(vol, w, &next, who) ? OK : ERR_NO_MEMORY;
}

error_t RibbonCollate(TagFSVolume* vol, const SpineSlot* slot, RibbonWalk** out)
{
    error_t     rc = RibbonWalkBegun(vol, slot, true, out);
    RibbonWalk* w  = *out;
    if (rc == OK)
        rc = RibbonSew(vol, w);
    if (rc == OK)
        rc = RibbonScan(vol, w);
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

error_t RibbonCollateOlder(TagFSVolume* vol, const SpineSlot* slot, RibbonWalk** out)
{
    error_t     rc = RibbonWalkBegun(vol, slot, false, out);
    RibbonWalk* w  = *out;
    if (rc == OK)
        rc = RibbonSew(vol, w);
    if (w) {
        kfree(w->ground);
        w->ground  = NULL;
        w->nground = 0;
    }
    return rc;
}
