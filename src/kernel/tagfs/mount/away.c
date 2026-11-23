#include "away.h"
#include "book_mount.h"
#include "perambulation.h"
#include "errata.h"
#include "../scribe/scribe.h"
#include "../settle/settle.h"
#include "../helm/helm.h"
#include "../ink/ink.h"
#include "boardroom.h"
#include "tagfs_volume.h"

/*
 * The open book (away.h). Everything here runs where the medium is attended
 * to — one core at a time — or where the machine stops.
 */


/* The entry of the volume `uuid`, or NULL. Under away.known_lock. */
static AwayKnown* AwayKnownFind(TagFSVolume* vol, const uint8_t uuid[16])
{
    for (uint32_t i = 0; i < vol->away.known_count; i++)
        if (memcmp(vol->away.known[i].uuid, uuid, sizeof(vol->away.known[i].uuid)) == 0)
            return &vol->away.known[i];
    return NULL;
}

/* `next_file` and `next_tag` into the volume `uuid`'s entry, each where it is
 * further, and `lied` with it; the entry made when there is none. False when
 * there is no memory for it. */
static bool AwayKnownRaise(TagFSVolume* vol, const uint8_t uuid[16], uint64_t next_file, uint32_t next_tag, bool lied)
{
    for (;;) {
        TagFSLockTake(&vol->away.known_lock);
        AwayKnown* k = AwayKnownFind(vol, uuid);
        if (!k && vol->away.known_count < vol->away.known_cap) {
            k = &vol->away.known[vol->away.known_count++];
            memset(k, 0, sizeof(*k));
            memcpy(k->uuid, uuid, sizeof(k->uuid));
        }
        if (k) {
            if (next_file > k->next_file_id)
                k->next_file_id = next_file;
            if (next_tag > k->next_tag_id)
                k->next_tag_id = next_tag;
            k->lied |= lied;
            TagFSLockDrop(&vol->away.known_lock);
            return true;
        }
        uint32_t cap = vol->away.known_cap;
        TagFSLockDrop(&vol->away.known_lock);

        uint32_t   room = cap ? cap * 2u : 4u;
        AwayKnown* more = kmalloc(sizeof(AwayKnown) * room);
        AwayKnown* old  = NULL;
        if (!more)
            return false;
        TagFSLockTake(&vol->away.known_lock);
        if (vol->away.known_cap == cap) {
            if (vol->away.known_count)
                memcpy(more, vol->away.known, sizeof(AwayKnown) * vol->away.known_count);
            old         = vol->away.known;
            vol->away.known     = more;
            vol->away.known_cap = room;
            more        = NULL;
        }
        TagFSLockDrop(&vol->away.known_lock);
        kfree(more);
        kfree(old);
    }
}

/* The numbers the book's memory had reached, and whether its medium lied,
 * remembered for its volume. */
static void AwayRemember(TagFSVolume* vol, bool lied)
{
    TagFSState* fs        = &vol->state;
    uint64_t    next_file;
    uint32_t    next_tag  = 0;

    TagFSLockTake(&fs->lock);
    next_file = vol->book.next_fid;
    TagFSLockDrop(&fs->lock);
    if (fs->registry) {
        TagFSLockTake(&fs->registry->lock);
        next_tag = fs->registry->next_id;
        TagFSLockDrop(&fs->registry->lock);
    }
    if (!AwayKnownRaise(vol, fs->uuid, next_file, next_tag, lied))
        kprintf("[TagFS] there is no memory to remember the ids this kernel gave out on the "
                "volume whose book is given up (file ids below %llu, tag ids below %u)%s — its "
                "next mount may give one of them again, to a program other than the one "
                "holding it\n", (unsigned long long)next_file, next_tag,
                lied ? ", nor that its medium lied" : "");
}

/* The changes the book holds: inked, and no cycle settled. */
static uint64_t AwayUnsettled(TagFSVolume* vol)
{
    uint64_t inked   = InkedGen(vol);
    uint64_t settled = SettledGen(vol);
    return inked > settled ? inked - settled : 0;
}

uint64_t AwayDepartures(TagFSVolume* vol)
{
    return __atomic_load_n(&vol->away.departures, __ATOMIC_ACQUIRE);
}

bool AwayDeedSame(TagFSVolume* vol, const DeedCopy* head)
{
    uint16_t           bytes = 0;
    const VolumeFrame* frame = (const VolumeFrame*)DeedStamp(head, VOLUME_STAMP_FRAME, &bytes);
    return frame && bytes >= sizeof(VolumeFrame) && memcmp(frame, &vol->book.frame, sizeof(VolumeFrame)) == 0;
}

void AwayLeft(TagFSVolume* vol, bool swapped)
{
    __atomic_add_fetch(&vol->away.departures, 1u, __ATOMIC_ACQ_REL);
    kprintf("[TagFS] volume %u: the medium the volume lives on has %s — its book is held open: %llu "
            "change(s) no cycle settled, and everyone waiting on them, wait for the same volume "
            "to come back as it was left; every read and write of it is refused until then\n",
            TagFSVolumeNumber(vol), swapped ? "been replaced by another one in the same seat" : "left",
            (unsigned long long)AwayUnsettled(vol));
}

/* A book taken up (perambulation.h): said with what was found, and the
 * verdict its mount holds asked for again — the session's first cycle owes it
 * still, and the departure may have refused the one that was asked. */
static void AwayBookTakenUp(TagFSVolume* vol, uint8_t seat)
{
    const PerambulationSaid* p = PerambulationLast(vol);
    char                     last[96];
    if (p->last)
        ksnprintf(last, sizeof(last), "its last stitch, seq %llu, where it was sewn", (unsigned long long)p->last);
    else
        ksnprintf(last, sizeof(last), "no stitch of it past its newest slot, which stands for every one");
    kprintf("[TagFS] volume %u: the volume is back, in seat %u: %s — its book as this kernel left it: %s and nothing another "
            "writer laid past it, its newest slot seq %llu, and %llu block(s) of data no stitch settled read back as "
            "written. Its book is taken up: %llu change(s) no cycle settled go to the medium with the next cycle, "
            "none lost%s\n",
            TagFSVolumeNumber(vol), seat, BoardroomSeatName(seat), last, (unsigned long long)p->slot,
            (unsigned long long)p->proved,
            (unsigned long long)AwayUnsettled(vol),
            p->tried ? "; its own stitch cut short lies past the last, and the next goes on past it (M-6)" : "");
    ErrataAsk(vol);
}

void AwayTakeUp(TagFSVolume* vol, uint8_t seat)
{
    AwayBookTakenUp(vol, seat);
    SettleSeatReady(vol);
    HelmBookTakenUp(vol);
}

#define AWAY_LOST " — its book is given up: %llu change(s) no cycle settled are lost, and " \
                  "everyone waiting on them hears so\n"

uint64_t AwayGiveUp(TagFSVolume* vol, AwayVerdict why, uint8_t seat)
{
    uint64_t lost = AwayUnsettled(vol);
    bool     lied = why == AWAY_OTHER && PerambulationLast(vol)->lied;
    AwayRemember(vol, lied);
    switch (why) {
    case AWAY_NOT_LANDED:
        kprintf("[TagFS] volume %u: the volume came back in seat %u as this kernel left its book, but %llu of the %llu block(s) "
                "of data no stitch settled do not read back as written — a medium's cache goes with its power, "
                "whatever a flush said (ERR_TAGFS_NOT_LANDED to everyone waiting)" AWAY_LOST, TagFSVolumeNumber(vol), seat,
                (unsigned long long)PerambulationLast(vol)->missed, (unsigned long long)PerambulationLast(vol)->proved,
                (unsigned long long)lost);
        break;
    case AWAY_OTHER:
        kprintf("[TagFS] volume %u: the volume came back in seat %u not as this kernel left its book: %s%s" AWAY_LOST,
                TagFSVolumeNumber(vol), seat,
                PerambulationLast(vol)->why,
                lied ? " — gone, nothing another writer's in its place, though a flush the medium said it kept "
                       "covered it: the medium lied, and its book carries MEDIUM_LIED from its next mount on" : "",
                (unsigned long long)lost);
        break;
    case AWAY_SEAT_TAKEN:
        kprintf("[TagFS] volume %u: another volume sits in seat %u, where the book of the volume that left "
                "it was held" AWAY_LOST, TagFSVolumeNumber(vol), seat, (unsigned long long)lost);
        break;
    case AWAY_HALT:
        kprintf("[TagFS] volume %u: the machine is stopping while the volume's medium is away" AWAY_LOST,
                TagFSVolumeNumber(vol), (unsigned long long)lost);
        break;
    default:
        /* A book as it was left is taken up (AwayTakeUp), never given up: a
         * verdict that reaches here is a caller's defect, and the book is
         * given up all the same, so no waiter waits for nothing. */
#if CONFIG_STRICT
        panic("[TagFS] DEFECT: a book was given up for verdict %d, which gives no book up",
              (int)why);
#endif
        kprintf("[TagFS] DEFECT: a book was given up for verdict %d, which gives no book up"
                AWAY_LOST, (int)why, (unsigned long long)lost);
        break;
    }
    HelmBookGivenUp(vol, lost);
    SettleWaitersAnswerAll(vol, why == AWAY_NOT_LANDED ? ERR_TAGFS_NOT_LANDED : ERR_DEVICE_NOT_READY);
    return lost;
}

bool AwayLiedLearned(TagFSVolume* vol, const uint8_t uuid[16])
{
    TagFSLockTake(&vol->away.known_lock);
    const AwayKnown* k    = AwayKnownFind(vol, uuid);
    bool             lied = k && k->lied;
    TagFSLockDrop(&vol->away.known_lock);
    return lied;
}

#ifdef CONFIG_VOLUME_TESTS
bool TagFSStandLiedForget(TagFSVolume* vol)
{
    bool was = false;
    TagFSLockTake(&vol->away.known_lock);
    for (uint32_t i = 0; i < vol->away.known_count; i++) {
        was |= vol->away.known[i].lied;
        vol->away.known[i].lied = false;
    }
    TagFSLockDrop(&vol->away.known_lock);
    return was;
}
#endif

void AwayNumbersKnown(TagFSVolume* vol, const uint8_t uuid[16], uint64_t* next_file_id, uint32_t* next_tag_id)
{
    TagFSLockTake(&vol->away.known_lock);
    const AwayKnown* k = AwayKnownFind(vol, uuid);
    *next_file_id = k ? k->next_file_id : 0;
    *next_tag_id  = k ? k->next_tag_id : 0;
    TagFSLockDrop(&vol->away.known_lock);
}

/* The volume's away part as it begins: what was not nothing. */
void AwayVolumeBegin(TagFSVolume* vol)
{
    vol->away.known_lock = (TagFSLock)TAGFS_LOCK_INIT("away", LOCK_RANK_LEAF);
}
