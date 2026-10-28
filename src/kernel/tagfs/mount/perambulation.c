#include "perambulation.h"
#include "book_mount.h"
#include "../settle/sewing.h"
#include "../settle/binder.h"
#include "volume_book.h"
#include "spine_read.h"
#include "boardroom.h"
#include "crypto.h"
#include "kheap.h"

/*
 * Perambulation (perambulation.h): the medium that came back walked against
 * what this kernel left on it. Every read here is the Boardroom's, of `seat`
 * from `base`; a refusal of one leaves no verdict standing (s_refused): the
 * caller sees the seat refuse and keeps the book open until it answers again
 * (tagfs.c, TagFSVolumeReturned), as format v1's judge is kept.
 */

#define PERAMBULATION_RUN 256u     /* sectors a read of the scan takes at once */

static PerambulationSaid s_said;
static char              s_why[200];
static bool              s_refused;

const PerambulationSaid* PerambulationLast(void)
{
    return &s_said;
}

/* `n` sectors from the volume's sector `vlba` into `buf`: true — a sector the
 * medium cannot read left zeros, which hold as no sector of the book — or
 * false for the medium's refusal, noted. */
static bool PerambulationRead(uint8_t seat, uint64_t base, uint64_t vlba, uint32_t n, uint8_t* buf)
{
    int rc = BoardroomRead(seat, base + vlba, n, buf);
    if (rc == (int)ERR_BAD_SECTOR) {
        rc = 0;
        for (uint32_t k = 0; rc == 0 && k < n; k++) {
            int one = BoardroomRead(seat, base + vlba + k, 1, buf + (uint64_t)k * BOOK_SECTOR_BYTES);
            if (one == (int)ERR_BAD_SECTOR)
                memset(buf + (uint64_t)k * BOOK_SECTOR_BYTES, 0, BOOK_SECTOR_BYTES);
            else
                rc = one;
        }
    }
    if (rc != 0)
        s_refused = true;
    return rc == 0;
}

/* The book's not as left, for `why`: false, said by the give-up's line. */
static bool PerambulationNot(const char* why)
{
    s_said.why = why;
    return false;
}

// ----------------------------------------------------------------------------
// The newest slot
// ----------------------------------------------------------------------------

/*
 * The newest slot the medium holds VALID — by the mount's own rule
 * (spine_read.h): its eight sectors one SLOT unit where it lies and its
 * stream parsing — is the newest this kernel left: by its seq the one it
 * knows written; or, in a slot held unsure (binder.h: a write of it refused,
 * it may hold either Tally), byte for byte a unit this kernel sealed into it
 * (BinderSlotSealed) — never by its seq alone: a seq this kernel gave and the
 * medium never showed, another machine that never saw it may give again, over
 * that very slot (R11). A newer one is another machine's Tally; an older one a
 * slot the medium lost.
 */
static bool PerambulationSlot(uint8_t seat, uint64_t base, uint8_t* buf)
{
    const VolumeFrame* f     = &g_book.frame;
    uint64_t           found = 0, known = 0;
    for (uint32_t i = 0; i < g_binder.ring; i++)
        if (g_binder.slot[i].holds && !g_binder.slot[i].unsure && g_binder.slot[i].seq > known)
            known = g_binder.slot[i].seq;
    bool own = known == 0;
    for (uint32_t i = 0; i < f->spine_slots && !s_refused; i++) {
        SpineSlot slot = { .index = i, .block = f->spine_block + (uint64_t)i * f->spine_stride };
        if (!PerambulationRead(seat, base, slot.block * VOLUME_BLOCK_SECTORS, VOLUME_BLOCK_SECTORS, buf))
            break;
        error_t rc = SpineSlotJudgeSectors(&slot, buf);
        SpineSlotLetGo(&slot);
        if (rc != OK)
            return PerambulationNot("memory would not hold a slot of it to judge it");
        if (slot.state == SPINE_SLOT_VALID && slot.seq > found) {
            found = slot.seq;
            own   = slot.seq == known || BinderSlotSealed(i, buf);
        }
    }
    s_said.slot = found;
    if (s_refused || own)
        return true;
    ksnprintf(s_why, sizeof(s_why), "its newest slot is seq %llu, where the newest this kernel left is seq %llu%s",
              (unsigned long long)found, (unsigned long long)known,
              found > known ? " — and no slot this kernel sealed holds it byte for byte" : "");
    return PerambulationNot(s_why);
}

// ----------------------------------------------------------------------------
// The stitches this kernel left
// ----------------------------------------------------------------------------

/* Mark `m` read where its stitch was sewn into `buf` (its `of` sectors): every
 * sector holding there, one STITCH unit of its seq, and its stream's crc up
 * to its Clasp the Clasp this kernel sealed. */
static bool PerambulationMarkHolds(uint8_t seat, uint64_t base, const BookLeftMark* m, uint8_t* buf)
{
    const uint8_t* uuid  = tagfs_get_state()->uuid;
    uint64_t       left  = RibbonPlaceLeft(&m->at);
    uint32_t       first = m->of < left ? m->of : (uint32_t)left;
    BookSectorHead h;
    if (!PerambulationRead(seat, base, RibbonPlaceVlba(&m->at), first, buf) ||
        (m->of > first && !PerambulationRead(seat, base, m->hop.block * TAGFS_BLOCK_SECTORS, m->of - first,
                                             buf + (uint64_t)first * BOOK_SECTOR_BYTES)))
        return false;
    for (uint32_t k = 0; k < m->of; k++) {
        uint64_t vlba = k < first ? RibbonPlaceVlba(&m->at) + k : m->hop.block * TAGFS_BLOCK_SECTORS + (k - first);
        if (!BookSectorCheck(buf + (uint64_t)k * BOOK_SECTOR_BYTES, uuid, vlba))
            return false;
    }
    if (!BookUnitHeadsAgree(buf, m->of, &h) || h.kind != BOOK_UNIT_STITCH || h.seq != m->seq ||
        m->clasp_at > (uint64_t)m->of * BOOK_PAYLOAD_BYTES)
        return false;
    BookUnitStream(buf, m->of, buf);      /* the payloads close up in place: each moves to a lower address */
    return KCrc32c(buf, m->clasp_at) == m->clasp;
}

/*
 * The last stitch this kernel left still where it was sewn — and, on a medium
 * shown to lie, every one past the slot the ribbon is replayed over (§16.5
 * п.11): such a medium may lose a stitch it confirmed as well as the last.
 * None past the slot — a Tally took every one in — and the slot stands for
 * them (PerambulationSlot).
 */
static bool PerambulationMarks(uint8_t seat, uint64_t base)
{
    const RibbonCursor* c = RibbonCursorNow();
    uint32_t            n = 0;
    uint64_t            lacking = 0, seen = 0;
    RibbonPlace         past;
    const BookLeftMark* m = RibbonLeftMarks(&n, &lacking, &past, &seen);
    if (lacking) {
        ksnprintf(s_why, sizeof(s_why), "this kernel holds no mark of its stitch %llu to hold the medium to — "
                  "memory refused it, or its mount read that stitch in pieces", (unsigned long long)lacking);
        return PerambulationNot(s_why);
    }
    if (n && m[n - 1u].seq != c->last)
        return PerambulationNot("the last stitch this kernel kept a mark of is not the last it sewed");
    for (uint32_t i = c->lied || !n ? 0 : n - 1u; i < n; i++) {
        uint8_t* buf  = kmalloc((uint64_t)m[i].of * BOOK_SECTOR_BYTES);
        bool     held = buf && PerambulationMarkHolds(seat, base, &m[i], buf);
        kfree(buf);
        if (!buf)
            return PerambulationNot("memory would not hold a stitch of it to read it back");
        if (!held && !s_refused) {
            ksnprintf(s_why, sizeof(s_why), "its stitch %llu no longer stands where this kernel sewed it, by its "
                      "seq and its Clasp%s", (unsigned long long)m[i].seq,
                      c->lied ? " (a medium that lied: every stitch past its slot is held)" : "");
            return PerambulationNot(s_why);
        }
    }
    s_said.last = n ? m[n - 1u].seq : 0;
    return true;
}

// ----------------------------------------------------------------------------
// Past the last stitch
// ----------------------------------------------------------------------------

/* `sectors` sectors from `first`: no sector of the book with a seq past
 * `floor` but one of this kernel's own attempts, byte for byte. */
static bool PerambulationClear(uint8_t seat, uint64_t base, uint64_t first, uint64_t sectors, uint64_t floor,
                               uint8_t* buf)
{
    const uint8_t* uuid = tagfs_get_state()->uuid;
    for (uint64_t k = 0; k < sectors;) {
        uint32_t n = sectors - k < PERAMBULATION_RUN ? (uint32_t)(sectors - k) : PERAMBULATION_RUN;
        if (!PerambulationRead(seat, base, first + k, n, buf))
            return true;
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t* s      = buf + (uint64_t)i * BOOK_SECTOR_BYTES;
            bool           newest = false;
            BookSectorHead h;
            if (!BookSectorCheck(s, uuid, first + k + i) || !BookSectorHeadHolds(s, &h) || h.seq <= floor)
                continue;
            if (RibbonTryOwns(first + k + i, s, &newest)) {
                s_said.tried = s_said.tried || newest;
                continue;
            }
            ksnprintf(s_why, sizeof(s_why), "sector %llu, past its last stitch, holds seq %llu — past seq %llu, "
                      "every one its mount saw or this kernel sewed — of a unit this kernel did not lay: another "
                      "writer laid it", (unsigned long long)(first + k + i), (unsigned long long)h.seq,
                      (unsigned long long)floor);
            return PerambulationNot(s_why);
        }
        k += n;
    }
    return true;
}

/*
 * Wherever another writer's next stitch could begin (§3.8, the direct scan):
 * the rest of the Quire past the last stitch sewn — where a writer of a mount
 * of it goes on — and the whole of the next_quire that stitch names — where
 * one goes on that closes the Quire early, or retries. A sector there of a
 * seq past the last stitch is another's only past every seq the mount saw:
 * one no larger — a torn tail, a fragment of a retry the mount found — was
 * there before this kernel, and any writer that mounts the medium gives seqs
 * past it.
 */
static bool PerambulationScan(uint8_t seat, uint64_t base, uint8_t* buf)
{
    const RibbonCursor* c = RibbonCursorNow();
    uint32_t            n = 0;
    uint64_t            lacking = 0, seen = 0;
    RibbonPlace         p;
    (void)RibbonLeftMarks(&n, &lacking, &p, &seen);
    uint64_t            floor = c->last > seen ? c->last : seen;
    uint64_t            end   = p.quire.blocks * TAGFS_BLOCK_SECTORS;
    if (p.off < end && !PerambulationClear(seat, base, RibbonPlaceVlba(&p), end - p.off, floor, buf))
        return false;
    if (c->named.blocks && !RibbonQuireSame(&c->named, &p.quire) &&
        !PerambulationClear(seat, base, c->named.block * TAGFS_BLOCK_SECTORS,
                            c->named.blocks * TAGFS_BLOCK_SECTORS, floor, buf))
        return false;
    return true;
}

// ----------------------------------------------------------------------------
// The judge
// ----------------------------------------------------------------------------

/* The blocks written since the last kept flush read back (AwayProof). */
static AwayVerdict PerambulationProved(uint8_t seat, uint64_t base)
{
    error_t rc = AwayProof(seat, base, &s_said.proved, &s_said.missed);
    if (rc == ERR_NO_MEMORY) {
        s_said.why = "memory would not hold a block of it to read it back";
        return AWAY_OTHER;
    }
    if (rc != OK) {
        s_refused = true;
        return AWAY_OTHER;
    }
    return s_said.missed ? AWAY_NOT_LANDED : AWAY_AS_LEFT;
}

/* The walk itself, the Settle held still: the slot, the marks, the scan, then
 * the blocks read back. */
static AwayVerdict PerambulationWalked(uint8_t seat, uint64_t base)
{
    if (!RibbonCursorNow()->up) {
        s_said.why = "its book was mounted to be read alone, and no writer of it stands to go on";
        return AWAY_OTHER;
    }
    uint8_t* buf = kmalloc((uint64_t)PERAMBULATION_RUN * BOOK_SECTOR_BYTES);
    if (!buf) {
        s_said.why = "memory would not hold a read of it to judge it";
        return AWAY_OTHER;
    }
    bool holds = PerambulationSlot(seat, base, buf) && PerambulationMarks(seat, base) &&
                 PerambulationScan(seat, base, buf);
    kfree(buf);
    return holds && !s_refused ? PerambulationProved(seat, base) : AWAY_OTHER;
}

AwayVerdict Perambulation(uint8_t seat, uint64_t base, bool same)
{
    uint64_t written = 0;
    memset(&s_said, 0, sizeof(s_said));
    s_refused = false;
    if (!same) {
        s_said.why = "its Deed lays the volume out otherwise than the one held (FRAME)";
        return AWAY_OTHER;
    }
    if (!StitchHoldStill(&written)) {
#if CONFIG_STRICT
        panic("[TagFS] DEFECT: a book's return was judged on a core holding the Settle in the middle of a step");
#endif
        kprintf("[TagFS] DEFECT: a book's return was judged on a core holding the Settle in the middle of a step — "
                "nothing of the book can be read still, and it is given up\n");
        s_said.why = "its judge stood beneath a step of the Settle";
        return AWAY_OTHER;
    }
    AwayVerdict v = PerambulationWalked(seat, base);
    if (s_refused) {
        s_said.why = "the medium refused a read of it while it was judged";
        v          = AWAY_OTHER;
    } else if (v == AWAY_AS_LEFT && s_said.tried) {
        const char* nowhere = RibbonTryPast();
        if (nowhere) {
            ksnprintf(s_why, sizeof(s_why), "its own attempt lies past its last stitch, and %s", nowhere);
            s_said.why = s_why;
            v          = AWAY_OTHER;
        }
    }
    StitchStillLetGo(written);
    return v;
}
