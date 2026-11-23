#include "perambulation.h"
#include "book_mount.h"
#include "../settle/sewing.h"
#include "../settle/binder.h"
#include "volume_book.h"
#include "spine_read.h"
#include "boardroom.h"
#include "crypto.h"
#include "kheap.h"
#include "tagfs_volume.h"

/*
 * Perambulation (perambulation.h): the medium that came back walked against
 * what this kernel left on it. Every read here is the Boardroom's, of `seat`
 * from `base`; a refusal of one leaves no verdict standing (perambulation.refused): the
 * caller sees the seat refuse and keeps the book open until it answers again
 * (tagfs.c, TagFSVolumeReturned).
 */

#define PERAMBULATION_RUN 256u     /* sectors a read of the scan takes at once */


const PerambulationSaid* PerambulationLast(TagFSVolume* vol)
{
    return &vol->perambulation.said;
}

/* `n` sectors from the volume's sector `vlba` into `buf`: true — a sector the
 * medium cannot read left zeros, which hold as no sector of the book — or
 * false for the medium's refusal, noted. */
static bool PerambulationRead(TagFSVolume* vol, uint8_t seat, uint64_t base, uint64_t vlba, uint32_t n, uint8_t* buf)
{
    int rc = BoardroomRead(seat, base + vlba, n, buf);
    if (rc == (int)ERR_BAD_SECTOR) {
        rc = 0;
        for (uint32_t k = 0; rc == 0 && k < n; k++) {
            int one = BoardroomRead(seat, base + vlba + k, 1, buf + (uint64_t)k * BOOK_SECTOR_BYTES);
            if (one == (int)ERR_BAD_SECTOR) {
                memset(buf + (uint64_t)k * BOOK_SECTOR_BYTES, 0, BOOK_SECTOR_BYTES);
                vol->perambulation.unreadable++;
            } else {
                rc = one;
            }
        }
    }
    if (rc != 0)
        vol->perambulation.refused = true;
    return rc == 0;
}

/* The book's not as left, for `why`: false, said by the give-up's line. */
static bool PerambulationNot(TagFSVolume* vol, const char* why)
{
    vol->perambulation.said.why = why;
    return false;
}

/* Whether a flush the medium said it kept covers everything the walk holds it
 * to: a stitch of this mount settled — its last flush came after every read
 * of the mount and every unit laid before it (perambulation.h, the lie). */
static bool PerambulationCovered(TagFSVolume* vol)
{
    return SettledCycles(vol) > 0;
}

// ----------------------------------------------------------------------------
// The newest slot
// ----------------------------------------------------------------------------

/*
 * The newest slot the medium holds VALID — by the mount's own rule
 * (spine_read.h; newest as every reader of a book orders them, volume_spine.h
 * SpineSlotNewer): its eight sectors one SLOT unit where it lies and its
 * stream parsing — is the newest this kernel left: by its seq the one it
 * knows written; or, in a slot held unsure (binder.h: a write of it refused,
 * it may hold either Tally), byte for byte a unit this kernel sealed into it
 * (BinderSlotSealed) — never by its seq alone: a seq this kernel gave and the
 * medium never showed, another machine that never saw it may give again, over
 * that very slot (R11). A newer one is another machine's Tally; an older one a
 * slot the medium lost.
 */
static bool PerambulationSlot(TagFSVolume* vol, uint8_t seat, uint64_t base, uint8_t* buf)
{
    const VolumeFrame* f     = &vol->book.frame;
    uint64_t           found = 0, known = 0;
    uint32_t           found_at = 0;
    for (uint32_t i = 0; i < vol->binder.ring; i++)
        if (vol->binder.slot[i].holds && !vol->binder.slot[i].unsure && vol->binder.slot[i].seq > known)
            known = vol->binder.slot[i].seq;
    bool own = known == 0;
    for (uint32_t i = 0; i < f->spine_slots && !vol->perambulation.refused; i++) {
        SpineSlot slot = { .index = i, .block = f->spine_block + (uint64_t)i * f->spine_stride };
        if (!PerambulationRead(vol, seat, base, slot.block * VOLUME_BLOCK_SECTORS, VOLUME_BLOCK_SECTORS, buf))
            break;
        error_t rc = SpineSlotJudgeSectors(vol, &slot, buf);
        SpineSlotLetGo(&slot);
        if (rc != OK)
            return PerambulationNot(vol, "memory would not hold a slot of it to judge it");
        if (slot.state == SPINE_SLOT_VALID && SpineSlotNewer(slot.seq, i, found, found_at)) {
            found    = slot.seq;
            found_at = i;
            own      = slot.seq == known || BinderSlotSealed(vol, i, buf);
        }
    }
    vol->perambulation.said.slot = found;
    if (vol->perambulation.refused || own)
        return true;
    vol->perambulation.said.lied = found && found < known && vol->binder.slot[found_at].holds && vol->binder.slot[found_at].seq == found &&
                  PerambulationCovered(vol);
    ksnprintf(vol->perambulation.why, sizeof(vol->perambulation.why), "its newest slot is seq %llu, where the newest this kernel left is seq %llu%s",
              (unsigned long long)found, (unsigned long long)known,
              found > known ? " — and no slot this kernel sealed holds it byte for byte" : "");
    return PerambulationNot(vol, vol->perambulation.why);
}

// ----------------------------------------------------------------------------
// The stitches this kernel left
// ----------------------------------------------------------------------------

/* Mark `m` read where its stitch was sewn into `buf` (its `of` sectors): every
 * sector holding there as a sector of its own unit — one STITCH unit of its
 * seq, each at its place in it — and its stream's crc up to its Clasp the
 * Clasp this kernel sealed. Not holding, *lost whether the stitch is plainly
 * gone from there (perambulation.h, the lie): a sector of it no sector of the
 * book or one of an older seq, none the medium said it cannot read, and every
 * other sector still of its own unit. */
static bool PerambulationMarkHolds(TagFSVolume* vol, uint8_t seat, uint64_t base, const BookLeftMark* m, uint8_t* buf, bool* lost)
{
    const uint8_t* uuid       = vol->state.uuid;
    uint64_t       left       = RibbonPlaceLeft(&m->at);
    uint32_t       first      = m->of < left ? m->of : (uint32_t)left;
    uint64_t       unreadable = vol->perambulation.unreadable;
    uint32_t       gone       = 0;
    bool           other      = false;
    *lost = false;
    if (!PerambulationRead(vol, seat, base, RibbonPlaceVlba(&m->at), first, buf) ||
        (m->of > first && !PerambulationRead(vol, seat, base, m->hop.block * TAGFS_BLOCK_SECTORS, m->of - first,
                                             buf + (uint64_t)first * BOOK_SECTOR_BYTES)))
        return false;
    for (uint32_t k = 0; k < m->of; k++) {
        const uint8_t* sector = buf + (uint64_t)k * BOOK_SECTOR_BYTES;
        uint64_t       vlba   = k < first ? RibbonPlaceVlba(&m->at) + k
                                          : m->hop.block * TAGFS_BLOCK_SECTORS + (k - first);
        BookSectorHead h;
        if (!BookSectorCheck(sector, uuid, vlba) || !BookSectorHeadHolds(sector, &h) || h.seq < m->seq)
            gone++;
        else if (h.kind != BOOK_UNIT_STITCH || h.seq != m->seq || h.at != k || h.of != m->of)
            other = true;
    }
    if (gone || other) {
        *lost = gone && !other && vol->perambulation.unreadable == unreadable;
        return false;
    }
    if (m->clasp_at > (uint64_t)m->of * BOOK_PAYLOAD_BYTES)
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
static bool PerambulationMarks(TagFSVolume* vol, uint8_t seat, uint64_t base)
{
    const RibbonCursor* c = RibbonCursorNow(vol);
    uint32_t            n = 0;
    uint64_t            lacking = 0, seen = 0;
    RibbonPlace         past;
    const BookLeftMark* m = RibbonLeftMarks(vol, &n, &lacking, &past, &seen);
    if (lacking) {
        ksnprintf(vol->perambulation.why, sizeof(vol->perambulation.why), "this kernel holds no mark of its stitch %llu to hold the medium to — "
                  "memory refused it, or its mount read that stitch in pieces", (unsigned long long)lacking);
        return PerambulationNot(vol, vol->perambulation.why);
    }
    if (n && m[n - 1u].seq != c->last)
        return PerambulationNot(vol, "the last stitch this kernel kept a mark of is not the last it sewed");
    for (uint32_t i = c->lied || !n ? 0 : n - 1u; i < n; i++) {
        uint8_t* buf  = kmalloc((uint64_t)m[i].of * BOOK_SECTOR_BYTES);
        bool     lost = false;
        bool     held = buf && PerambulationMarkHolds(vol, seat, base, &m[i], buf, &lost);
        kfree(buf);
        if (!buf)
            return PerambulationNot(vol, "memory would not hold a stitch of it to read it back");
        if (!held && !vol->perambulation.refused) {
            vol->perambulation.said.lied = lost && PerambulationCovered(vol);
            ksnprintf(vol->perambulation.why, sizeof(vol->perambulation.why), "its stitch %llu no longer stands where this kernel sewed it, by its "
                      "seq and its Clasp%s", (unsigned long long)m[i].seq,
                      c->lied ? " (a medium that lied: every stitch past its slot is held)" : "");
            return PerambulationNot(vol, vol->perambulation.why);
        }
    }
    vol->perambulation.said.last = n ? m[n - 1u].seq : 0;
    return true;
}

// ----------------------------------------------------------------------------
// Past the last stitch
// ----------------------------------------------------------------------------

/* `sectors` sectors from `first`: no sector of the book with a seq past
 * `floor` but one of this kernel's own attempts, byte for byte. */
static bool PerambulationClear(TagFSVolume* vol, uint8_t seat, uint64_t base, uint64_t first, uint64_t sectors, uint64_t floor,
                               uint8_t* buf)
{
    const uint8_t* uuid = vol->state.uuid;
    for (uint64_t k = 0; k < sectors;) {
        uint32_t n = sectors - k < PERAMBULATION_RUN ? (uint32_t)(sectors - k) : PERAMBULATION_RUN;
        if (!PerambulationRead(vol, seat, base, first + k, n, buf))
            return true;
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t* s      = buf + (uint64_t)i * BOOK_SECTOR_BYTES;
            bool           newest = false;
            BookSectorHead h;
            if (!BookSectorCheck(s, uuid, first + k + i) || !BookSectorHeadHolds(s, &h) || h.seq <= floor)
                continue;
            if (RibbonTryOwns(vol, first + k + i, s, &newest)) {
                vol->perambulation.said.tried = vol->perambulation.said.tried || newest;
                continue;
            }
            ksnprintf(vol->perambulation.why, sizeof(vol->perambulation.why), "sector %llu, past its last stitch, holds seq %llu — past seq %llu, "
                      "every one its mount saw or this kernel sewed — of a unit this kernel did not lay: another "
                      "writer laid it", (unsigned long long)(first + k + i), (unsigned long long)h.seq,
                      (unsigned long long)floor);
            return PerambulationNot(vol, vol->perambulation.why);
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
static bool PerambulationScan(TagFSVolume* vol, uint8_t seat, uint64_t base, uint8_t* buf)
{
    const RibbonCursor* c = RibbonCursorNow(vol);
    uint32_t            n = 0;
    uint64_t            lacking = 0, seen = 0;
    RibbonPlace         p;
    (void)RibbonLeftMarks(vol, &n, &lacking, &p, &seen);
    uint64_t            floor = c->last > seen ? c->last : seen;
    uint64_t            end   = p.quire.blocks * TAGFS_BLOCK_SECTORS;
    if (p.off < end && !PerambulationClear(vol, seat, base, RibbonPlaceVlba(&p), end - p.off, floor, buf))
        return false;
    if (c->named.blocks && !RibbonQuireSame(&c->named, &p.quire) &&
        !PerambulationClear(vol, seat, base, c->named.block * TAGFS_BLOCK_SECTORS,
                            c->named.blocks * TAGFS_BLOCK_SECTORS, floor, buf))
        return false;
    return true;
}

// ----------------------------------------------------------------------------
// The judge
// ----------------------------------------------------------------------------

/* The blocks written since the last kept flush read back (AwayProof). */
static AwayVerdict PerambulationProved(TagFSVolume* vol, uint8_t seat, uint64_t base)
{
    error_t rc = AwayProof(vol, seat, base, &vol->perambulation.said.proved, &vol->perambulation.said.missed);
    if (rc == ERR_NO_MEMORY) {
        vol->perambulation.said.why = "memory would not hold a block of it to read it back";
        return AWAY_OTHER;
    }
    if (rc != OK) {
        vol->perambulation.refused = true;
        return AWAY_OTHER;
    }
    return vol->perambulation.said.missed ? AWAY_NOT_LANDED : AWAY_AS_LEFT;
}

/* The walk itself, the Settle held still: the slot, the marks, the scan, then
 * the blocks read back. */
static AwayVerdict PerambulationWalked(TagFSVolume* vol, uint8_t seat, uint64_t base)
{
    if (!RibbonCursorNow(vol)->up) {
        vol->perambulation.said.why = "its book was mounted to be read alone, and no writer of it stands to go on";
        return AWAY_OTHER;
    }
    uint8_t* buf = kmalloc((uint64_t)PERAMBULATION_RUN * BOOK_SECTOR_BYTES);
    if (!buf) {
        vol->perambulation.said.why = "memory would not hold a read of it to judge it";
        return AWAY_OTHER;
    }
    bool holds = PerambulationSlot(vol, seat, base, buf) && PerambulationMarks(vol, seat, base) &&
                 PerambulationScan(vol, seat, base, buf);
    kfree(buf);
    return holds && !vol->perambulation.refused ? PerambulationProved(vol, seat, base) : AWAY_OTHER;
}

AwayVerdict Perambulation(TagFSVolume* vol, uint8_t seat, uint64_t base, bool same)
{
    uint64_t written = 0;
    memset(&vol->perambulation.said, 0, sizeof(vol->perambulation.said));
    vol->perambulation.refused = false;
    if (!same) {
        vol->perambulation.said.why = "its Deed lays the volume out otherwise than the one held (FRAME)";
        return AWAY_OTHER;
    }
    if (!StitchHoldStill(vol, &written)) {
#if CONFIG_STRICT
        panic("[TagFS] DEFECT: a book's return was judged on a core holding the Settle in the middle of a step");
#endif
        kprintf("[TagFS] DEFECT: a book's return was judged on a core holding the Settle in the middle of a step — "
                "nothing of the book can be read still, and it is given up\n");
        vol->perambulation.said.why = "its judge stood beneath a step of the Settle";
        return AWAY_OTHER;
    }
    AwayVerdict v = PerambulationWalked(vol, seat, base);
    if (vol->perambulation.refused) {
        vol->perambulation.said.why = "the medium refused a read of it while it was judged";
        v          = AWAY_OTHER;
    } else if (v == AWAY_AS_LEFT && vol->perambulation.said.tried) {
        const char* nowhere = RibbonTryPast(vol);
        if (nowhere) {
            ksnprintf(vol->perambulation.why, sizeof(vol->perambulation.why), "its own attempt lies past its last stitch, and %s", nowhere);
            vol->perambulation.said.why = vol->perambulation.why;
            v          = AWAY_OTHER;
        }
    }
    StitchStillLetGo(vol, written);
    return v;
}
