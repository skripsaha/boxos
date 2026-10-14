#include "away.h"
#include "../ledger/ledger.h"
#include "../scribe/scribe.h"
#include "../settle/settle.h"
#include "../helm/helm.h"
#include "../ink/ink.h"
#include "boardroom.h"

/*
 * The open book (away.h). Everything here runs where the medium is attended
 * to — one core at a time — or where the machine stops; the seqs the judge
 * found are kept for the line its verdict says.
 */

static uint64_t s_found_seq;    /* the newest Ledger the returned medium holds */
static uint64_t s_left_seq;     /* the one a cycle this kernel settled on it */

/*
 * ‼ WHAT A BOOK GIVEN UP LEAVES KNOWN, ONE ENTRY A VOLUME, FOR THE REST OF THE
 * BOOT.
 *
 * The programs live on when a book is given up, holding the file ids and tag
 * ids the volume's memory gave them — a create no cycle settled among them —
 * while the next mount reads its numbers off the medium alone, which never
 * saw those ids. Given out again, one program's id named another's file. So
 * the ids memory had reached are remembered by the volume's UUID, and its
 * mounts give out nothing below them (AwayNumbersKnown); and a volume found
 * holding less than a settled cycle is remembered the same way, every one of
 * them, not only the last.
 */
typedef struct {
    uint8_t  uuid[16];
    uint64_t next_file_id;
    uint32_t next_tag_id;
    bool     lied;
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
 * further, and `lied` with it; the entry made when there is none. False when
 * there is no memory for it. A count an earlier kernel let wrap (0) is the
 * medium's to keep refusing creates on: the mount keeps it, whatever is
 * remembered here. */
static bool AwayKnownRaise(const uint8_t uuid[16], uint64_t next_file, uint32_t next_tag,
                           bool lied)
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
            k->lied |= lied;
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

/* The numbers the book's memory had reached, and whether the medium lied,
 * remembered for its volume. */
static void AwayRemember(bool lied)
{
    TagFSState* fs        = tagfs_get_state();
    uint64_t    next_file;
    uint32_t    next_tag  = 0;

    TagFSLockTake(&fs->lock);
    next_file = fs->ledger.next_file_id;
    TagFSLockDrop(&fs->lock);
    if (fs->registry) {
        TagFSLockTake(&fs->registry->lock);
        next_tag = fs->registry->next_id;
        TagFSLockDrop(&fs->registry->lock);
    }
    if (!AwayKnownRaise(fs->uuid, next_file, next_tag, lied))
        kprintf("[TagFS] there is no memory to remember the ids this kernel gave out on the "
                "volume whose book is given up (file ids below %llu, tag ids below %u)%s — its "
                "next mount may give one of them again, to a program other than the one "
                "holding it\n", (unsigned long long)next_file, next_tag,
                lied ? ", nor that its medium lied" : "");
}

/* The changes the book holds: inked, and no cycle settled. */
static uint64_t AwayUnsettled(void)
{
    uint64_t inked   = InkedGen();
    uint64_t settled = SettledGen();
    return inked > settled ? inked - settled : 0;
}

void AwayLeft(bool swapped)
{
    kprintf("[TagFS] the medium the volume lives on has %s — its book is held open: %llu "
            "change(s) no cycle settled, and everyone waiting on them, wait for the same volume "
            "to come back as it was left; every read and write of it is refused until then\n",
            swapped ? "been replaced by another one in the same seat" : "left",
            (unsigned long long)AwayUnsettled());
}

AwayVerdict AwayJudge(const VolumeLedger* found)
{
    uint64_t   settled_seq = 0;
    LedgerLeft as          = LedgerLeftAs(found, &settled_seq);
    s_found_seq = found->seq;
    s_left_seq  = settled_seq;
    if (as == LEDGER_OLDER)
        return AWAY_OLDER;
    if (as == LEDGER_OTHER)
        return AWAY_OTHER;
    return VolumeWritesKept() ? AWAY_AS_LEFT : AWAY_IN_DOUBT;
}

AwayVerdict AwayJudgeOn(uint8_t seat, uint64_t base)
{
    VolumeLedger found;
    if (!LedgerNewestOn(seat, base, &found))
        return AWAY_UNREAD;
    return AwayJudge(&found);
}

void AwayTakeUp(uint8_t seat)
{
    kprintf("[TagFS] the volume is back, in seat %u: %s — as this kernel left it: its Ledger "
            "at seq %llu with the seal it was left with, and every write it saw kept by a flush. "
            "Its book is taken up: %llu change(s) no cycle settled go to the medium with the "
            "next cycle, none lost. This proves only that no BoxOS kernel settled a cycle on it "
            "meanwhile — a host that changed its bytes and left the Ledger alone is not seen\n",
            seat, BoardroomSeatName(seat), (unsigned long long)s_found_seq,
            (unsigned long long)AwayUnsettled());
    SettleSeatReady();
    HelmBookTakenUp();
}

#define AWAY_LOST " — its book is given up: %llu change(s) no cycle settled are lost, and " \
                  "everyone waiting on them hears so\n"

uint64_t AwayGiveUp(AwayVerdict why, uint8_t seat)
{
    uint64_t lost = AwayUnsettled();
    AwayRemember(why == AWAY_OLDER);
    switch (why) {
    case AWAY_IN_DOUBT:
        kprintf("[TagFS] the volume came back in seat %u with its Ledger as this kernel left it "
                "(seq %llu), but a write it saw complete was never covered by a flush the medium "
                "kept, and a medium's cache goes with its power" AWAY_LOST,
                seat, (unsigned long long)s_found_seq, (unsigned long long)lost);
        break;
    case AWAY_OLDER:
        kprintf("[TagFS] the volume came back in seat %u holding a Ledger older (seq %llu) than "
                "the one a cycle this kernel settled on it (seq %llu): the medium lost a cycle it "
                "said it kept, and its seals carry SETTLE_MEDIUM_LIED from its next mount on"
                AWAY_LOST, seat, (unsigned long long)s_found_seq,
                (unsigned long long)s_left_seq, (unsigned long long)lost);
        break;
    case AWAY_REREAD:
        kprintf("[TagFS] the format-v2 volume came back in seat %u: this kernel reads such a volume "
                "again from its book whenever its medium comes back, and takes up no open book of it"
                AWAY_LOST, seat, (unsigned long long)lost);
        break;
    case AWAY_OTHER:
        kprintf("[TagFS] the volume came back in seat %u with a Ledger or a Deed this kernel did "
                "not leave on it — a BoxOS kernel changed it meanwhile" AWAY_LOST,
                seat, (unsigned long long)lost);
        break;
    case AWAY_UNREAD:
        kprintf("[TagFS] the volume came back in seat %u and neither copy of its Ledger reads "
                "intact — nothing says it is as it was left" AWAY_LOST,
                seat, (unsigned long long)lost);
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
    SettleWaitersAnswerAll(ERR_DEVICE_NOT_READY);
    return lost;
}

bool AwayLiedLearned(const uint8_t uuid[16])
{
    TagFSLockTake(&s_known_lock);
    const AwayKnown* k    = AwayKnownFind(uuid);
    bool             lied = k && k->lied;
    TagFSLockDrop(&s_known_lock);
    return lied;
}

void AwayNumbersKnown(const uint8_t uuid[16], uint64_t* next_file_id, uint32_t* next_tag_id)
{
    TagFSLockTake(&s_known_lock);
    const AwayKnown* k = AwayKnownFind(uuid);
    *next_file_id = k ? k->next_file_id : 0;
    *next_tag_id  = k ? k->next_tag_id : 0;
    TagFSLockDrop(&s_known_lock);
}
