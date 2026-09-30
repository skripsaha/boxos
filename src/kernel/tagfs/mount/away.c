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

/* A volume found holding less than a settled cycle, for the rest of the boot. */
static uint8_t  s_lied_uuid[16];
static bool     s_lied_known;

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
        memcpy(s_lied_uuid, tagfs_get_state()->uuid, sizeof(s_lied_uuid));
        s_lied_known = true;
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
    default:        /* AWAY_HALT: a book as it was left is taken up, never given up */
        kprintf("[TagFS] the machine is stopping while the volume's medium is away" AWAY_LOST,
                (unsigned long long)lost);
        break;
    }
    HelmBookGivenUp(lost);
    SettleWaitersAnswerAll(ERR_DEVICE_NOT_READY);
    return lost;
}

bool AwayLiedLearned(const uint8_t uuid[16])
{
    return s_lied_known && memcmp(s_lied_uuid, uuid, sizeof(s_lied_uuid)) == 0;
}
