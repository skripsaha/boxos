#include "away.h"
#include "book_mount.h"
#include "perambulation.h"
#include "errata.h"
#include "../scribe/scribe.h"
#include "../settle/settle.h"
#include "../helm/helm.h"
#include "../ink/ink.h"
#include "boardroom.h"

/*
 * The open book (away.h). Everything here runs where the medium is attended
 * to — one core at a time — or where the machine stops.
 */

static volatile uint64_t s_departures;  /* the medium left, this boot: AwayLeft counts it */

/*
 * ‼ WHAT A BOOK GIVEN UP LEAVES KNOWN, ONE ENTRY A VOLUME, FOR THE REST OF THE
 * BOOT.
 *
 * The programs live on when a book is given up, holding the file ids and tag
 * ids the volume's memory gave them — a create no cycle settled among them —
 * while the next mount reads its numbers off the medium alone, which never
 * saw those ids. Given out again, one program's id named another's file. So
 * the ids memory had reached are remembered by the volume's UUID, and its
 * mounts give out nothing below them (AwayNumbersKnown), every volume's, not
 * only the last.
 */
typedef struct {
    uint8_t  uuid[16];
    uint64_t next_file_id;
    uint32_t next_tag_id;
} AwayKnown;

/* The entries, under their own leaf lock: a book is given up where the medium
 * is attended to and where the machine stops, and a mount reads them. The
 * array grows with no lock held and is swapped in under it. */
static TagFSLock  s_known_lock = TAGFS_LOCK_INIT("away", LOCK_RANK_LEAF);
static AwayKnown* s_known;
static uint32_t   s_known_count;
static uint32_t   s_known_cap;

/* The entry of the volume `uuid`, or NULL. Under s_known_lock. */
static AwayKnown* AwayKnownFind(const uint8_t uuid[16])
{
    for (uint32_t i = 0; i < s_known_count; i++)
        if (memcmp(s_known[i].uuid, uuid, sizeof(s_known[i].uuid)) == 0)
            return &s_known[i];
    return NULL;
}

/* `next_file` and `next_tag` into the volume `uuid`'s entry, each where it is
 * further; the entry made when there is none. False when there is no memory
 * for it. */
static bool AwayKnownRaise(const uint8_t uuid[16], uint64_t next_file, uint32_t next_tag)
{
    for (;;) {
        TagFSLockTake(&s_known_lock);
        AwayKnown* k = AwayKnownFind(uuid);
        if (!k && s_known_count < s_known_cap) {
            k = &s_known[s_known_count++];
            memset(k, 0, sizeof(*k));
            memcpy(k->uuid, uuid, sizeof(k->uuid));
        }
        if (k) {
            if (next_file > k->next_file_id)
                k->next_file_id = next_file;
            if (next_tag > k->next_tag_id)
                k->next_tag_id = next_tag;
            TagFSLockDrop(&s_known_lock);
            return true;
        }
        uint32_t cap = s_known_cap;
        TagFSLockDrop(&s_known_lock);

        uint32_t   room = cap ? cap * 2u : 4u;
        AwayKnown* more = kmalloc(sizeof(AwayKnown) * room);
        AwayKnown* old  = NULL;
        if (!more)
            return false;
        TagFSLockTake(&s_known_lock);
        if (s_known_cap == cap) {
            if (s_known_count)
                memcpy(more, s_known, sizeof(AwayKnown) * s_known_count);
            old         = s_known;
            s_known     = more;
            s_known_cap = room;
            more        = NULL;
        }
        TagFSLockDrop(&s_known_lock);
        kfree(more);
        kfree(old);
    }
}

/* The numbers the book's memory had reached, remembered for its volume. */
static void AwayRemember(void)
{
    TagFSState* fs        = tagfs_get_state();
    uint64_t    next_file;
    uint32_t    next_tag  = 0;

    TagFSLockTake(&fs->lock);
    next_file = g_book.next_fid;
    TagFSLockDrop(&fs->lock);
    if (fs->registry) {
        TagFSLockTake(&fs->registry->lock);
        next_tag = fs->registry->next_id;
        TagFSLockDrop(&fs->registry->lock);
    }
    if (!AwayKnownRaise(fs->uuid, next_file, next_tag))
        kprintf("[TagFS] there is no memory to remember the ids this kernel gave out on the "
                "volume whose book is given up (file ids below %llu, tag ids below %u) — its "
                "next mount may give one of them again, to a program other than the one "
                "holding it\n", (unsigned long long)next_file, next_tag);
}

/* The changes the book holds: inked, and no cycle settled. */
static uint64_t AwayUnsettled(void)
{
    uint64_t inked   = InkedGen();
    uint64_t settled = SettledGen();
    return inked > settled ? inked - settled : 0;
}

uint64_t AwayDepartures(void)
{
    return __atomic_load_n(&s_departures, __ATOMIC_ACQUIRE);
}

bool AwayDeedSame(const DeedCopy* head)
{
    uint16_t           bytes = 0;
    const VolumeFrame* frame = (const VolumeFrame*)DeedStamp(head, VOLUME_STAMP_FRAME, &bytes);
    return frame && bytes >= sizeof(VolumeFrame) && memcmp(frame, &g_book.frame, sizeof(VolumeFrame)) == 0;
}

void AwayLeft(bool swapped)
{
    __atomic_add_fetch(&s_departures, 1u, __ATOMIC_ACQ_REL);
    kprintf("[TagFS] the medium the volume lives on has %s — its book is held open: %llu "
            "change(s) no cycle settled, and everyone waiting on them, wait for the same volume "
            "to come back as it was left; every read and write of it is refused until then\n",
            swapped ? "been replaced by another one in the same seat" : "left",
            (unsigned long long)AwayUnsettled());
}

/* A book taken up (perambulation.h): said with what was found, and the
 * verdict its mount holds asked for again — the session's first cycle owes it
 * still, and the departure may have refused the one that was asked. */
static void AwayBookTakenUp(uint8_t seat)
{
    const PerambulationSaid* p = PerambulationLast();
    char                     last[96];
    if (p->last)
        ksnprintf(last, sizeof(last), "its last stitch, seq %llu, where it was sewn", (unsigned long long)p->last);
    else
        ksnprintf(last, sizeof(last), "no stitch of it past its newest slot, which stands for every one");
    kprintf("[TagFS] the volume is back, in seat %u: %s — its book as this kernel left it: %s and nothing another "
            "writer laid past it, its newest slot seq %llu, and %llu block(s) of data no stitch settled read back as "
            "written. Its book is taken up: %llu change(s) no cycle settled go to the medium with the next cycle, "
            "none lost%s\n",
            seat, BoardroomSeatName(seat), last, (unsigned long long)p->slot, (unsigned long long)p->proved,
            (unsigned long long)AwayUnsettled(),
            p->tried ? "; its own stitch cut short lies past the last, and the next goes on past it (M-6)" : "");
    ErrataAsk();
}

void AwayTakeUp(uint8_t seat)
{
    AwayBookTakenUp(seat);
    SettleSeatReady();
    HelmBookTakenUp();
}

#define AWAY_LOST " — its book is given up: %llu change(s) no cycle settled are lost, and " \
                  "everyone waiting on them hears so\n"

uint64_t AwayGiveUp(AwayVerdict why, uint8_t seat)
{
    uint64_t lost = AwayUnsettled();
    AwayRemember();
    switch (why) {
    case AWAY_NOT_LANDED:
        kprintf("[TagFS] the volume came back in seat %u as this kernel left its book, but %llu of the %llu block(s) "
                "of data no stitch settled do not read back as written — a medium's cache goes with its power, "
                "whatever a flush said (ERR_TAGFS_NOT_LANDED to everyone waiting)" AWAY_LOST, seat,
                (unsigned long long)PerambulationLast()->missed, (unsigned long long)PerambulationLast()->proved,
                (unsigned long long)lost);
        break;
    case AWAY_OTHER:
        kprintf("[TagFS] the volume came back in seat %u not as this kernel left its book: %s" AWAY_LOST, seat,
                PerambulationLast()->why, (unsigned long long)lost);
        break;
    case AWAY_SEAT_TAKEN:
        kprintf("[TagFS] another volume sits in seat %u, where the book of the volume that left "
                "it was held" AWAY_LOST, seat, (unsigned long long)lost);
        break;
    case AWAY_HALT:
        kprintf("[TagFS] the machine is stopping while the volume's medium is away" AWAY_LOST,
                (unsigned long long)lost);
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
    HelmBookGivenUp(lost);
    SettleWaitersAnswerAll(why == AWAY_NOT_LANDED ? ERR_TAGFS_NOT_LANDED : ERR_DEVICE_NOT_READY);
    return lost;
}

void AwayNumbersKnown(const uint8_t uuid[16], uint64_t* next_file_id, uint32_t* next_tag_id)
{
    TagFSLockTake(&s_known_lock);
    const AwayKnown* k = AwayKnownFind(uuid);
    *next_file_id = k ? k->next_file_id : 0;
    *next_tag_id  = k ? k->next_tag_id : 0;
    TagFSLockDrop(&s_known_lock);
}
