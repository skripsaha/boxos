#ifndef TAGFS_MOUNT_AWAY_H
#define TAGFS_MOUNT_AWAY_H

#include "../tagfs.h"
#include "../deed/deed.h"

/*
 * The open book — a volume whose medium left, held as it was until an event
 * decides it (RT-13).
 *
 * The medium leaving takes nothing down. The volume keeps everything it holds
 * in memory, the changes no cycle settled among them; every read and write of
 * it is refused, no Ink changes it (ERR_DEVICE_NOT_READY), and every waiter —
 * a Helm's FIFO, a program waiting for its cycle — waits with it. No timer
 * decides: an event does.
 *
 *   The same volume comes back (its UUID, in any seat), its Deed saying what
 *   the held one says — format v1's LAYOUT, a book's FRAME. A book is judged
 *   by Perambulation (perambulation.h): its ribbon and its newest slot as this
 *   kernel left them, and every block written since the last kept flush read
 *   back as written. Format v1's Ledger is judged against what this kernel left on
 *   the medium — the record a cycle settled, and the image the last S4 aimed
 *   at the other copy, landed or not — each by its seq and its seal's
 *   {settle, closed_crc32, tail_crc32}, and every write the volume saw complete
 *   must have been covered by a flush the medium kept: a cache the medium had
 *   not committed went with the power. All of it holding, the book is TAKEN UP
 *   — the next cycle is built from S0 and settles what the book held, the
 *   parked Helms hand on, and nothing is lost, where ext4 and NTFS lose every
 *   dirty page. The check proves only that no BoxOS kernel settled a cycle on
 *   the medium meanwhile: a host that changed its bytes and left the Ledger
 *   alone moves no seq, and is not seen — the line says so.
 *
 *   Anything else GIVES THE BOOK UP: every waiter is answered with the count
 *   of changes no cycle settled, and the volume is let go of and read again.
 *   So does another volume taking the seat the book was held for, and the
 *   machine stopping. A medium found holding an older Ledger than a cycle this
 *   kernel settled lost a cycle it said it kept: the volume's seals carry
 *   SETTLE_MEDIUM_LIED from the next mount on (AwayLiedLearned). And the
 *   programs, which live on, hold the file ids and tag ids the book's memory
 *   gave them, which the medium never saw: the volume's next mounts give out
 *   none below them (AwayNumbersKnown).
 *
 * There is no declared eject in this kernel: the events above are every one
 * there is.
 */

typedef enum {
    AWAY_AS_LEFT = 0,   /* its Ledger as this kernel left it, every write seen kept */
    AWAY_IN_DOUBT,      /* as left, but a write seen complete no kept flush covered */
    AWAY_OLDER,         /* older than a cycle this kernel settled: the medium lied */
    AWAY_OTHER,         /* a Ledger, or a Deed, this kernel did not leave */
    AWAY_UNREAD,        /* no copy of its Ledger reads intact */
    AWAY_SEAT_TAKEN,    /* another volume sits in the seat the book was held for */
    AWAY_HALT,          /* the machine is stopping */
    AWAY_NOT_LANDED,    /* a book as left, but a block of data no stitch settled reads otherwise */
} AwayVerdict;

/* The medium left, just now latched (TagFSMediumLeft): said once, with the
 * changes the book holds — and counted. `swapped`: another medium already
 * sits in its seat. AwayDepartures: how many times it has left this boot —
 * what a book's write that laid blocks before a departure and inks them after
 * it holds its blocks to (perambulation.h, AwayProofPlot). */
void        AwayLeft(bool swapped);
uint64_t    AwayDepartures(void);

/* Whether the Deed `head` lays the volume out as the held one is laid out:
 * format v1's LAYOUT, a book's FRAME (§16.1 п.3 — a book has no LAYOUT). */
bool        AwayDeedSame(const DeedCopy* head);

/* The volume's newest intact Ledger on `seat`, its ground beginning at sector
 * `base`, read and judged (AwayJudge); AWAY_UNREAD when no copy reads intact.
 * Reads the medium: with no TagFS lock held, by the one attending to it. */
AwayVerdict AwayJudgeOn(uint8_t seat, uint64_t base);

/* `found` — the newest Ledger the medium holds — against what this kernel
 * left, and whether every write the volume saw is kept. */
AwayVerdict AwayJudge(const VolumeLedger* found);

/* The book taken up, the volume's medium in `seat` again and its latch let
 * go of by the caller: said, the refused cycle free to run from S0 and the
 * parked Helms handed on, here. */
void        AwayTakeUp(uint8_t seat);

/* The book given up, for `why`: said — with `seat`, where the medium or the
 * other volume is — and every waiter answered with the changes no cycle
 * settled, which is returned: ERR_TAGFS_NOT_LANDED for a book whose blocks
 * did not land, ERR_DEVICE_NOT_READY for any other. The volume is then the
 * caller's to let go of. */
uint64_t    AwayGiveUp(AwayVerdict why, uint8_t seat);

/* Whether a book given up found the volume `uuid` holding less than a cycle
 * this kernel settled: its mounts from then on seal SETTLE_MEDIUM_LIED. */
bool        AwayLiedLearned(const uint8_t uuid[16]);

/* The furthest next file id and next tag id memory had reached when a book of
 * the volume `uuid` was given up — 0 for one never given up. Its mounts give
 * out no id below them, for the rest of the boot. */
void        AwayNumbersKnown(const uint8_t uuid[16], uint64_t* next_file_id,
                             uint32_t* next_tag_id);

#ifdef CONFIG_VOLUME_TESTS
/*
 * The stand's departure and return (tagfs.c, which holds the latch), the
 * medium where it is. Leaves: latched and said as a departure is. Returns: the
 * return road's judge — of `found` when given, a Ledger the test altered to
 * stand for a medium another kernel wrote, else of the medium's own — and its
 * verdict acted on: the book taken up, or given up with every waiter
 * answered. A book given up is not followed by a take-down: the runner stands
 * inside the volume's door, where none can finish, so the stand's volume stays
 * mounted and its latch is let go of — the changes the answers called lost are
 * still held, and the next cycle settles them.
 */
void        TagFSStandMediumLeaves(void);
AwayVerdict TagFSStandMediumReturns(const VolumeLedger* found);
/* The medium leaves (TagFSStandMediumLeaves) once the next run of a file's
 * blocks has landed, before its writer hears so (run/run.c, TagFSWriteRun):
 * the departure a core attending the seat latches while a writer's command is
 * answered. Once. */
void        TagFSStandLeavesAfterRun(void);

/*
 * And the stand's own take-down and mount, for the road a mount takes when the
 * medium does what a hand does to it. The runner stands inside the volume's
 * door, so LetGo first lets go of the runner's hold and then takes the volume
 * down as a book given up does, writing nothing; HoldAgain counts the runner
 * back in, a volume up or not, because the runner lets go of it once more.
 *
 * The turn of the next mount: SEAT_EMPTIES empties the volume's seat at the
 * mount's nth block read — a format-v1 metadata block's, or a unit of a
 * book's (book_unit.c, which says when the read comes) — as a medium pulled
 * out in the middle of its reads;
 * SEAT_RINGS has the seat say seat:ready, and the room say it on the spot,
 * every time a block read of a mount is refused — before the attempt has
 * ended, as a driver's recovery of the refused command says it; HEAP_REFUSES
 * has the first allocation of the registry's load answered NULL, as an
 * exhausted heap answers it (mount.c, and book_mount.c for a book's, which
 * say when the load begins).
 * Fired: the turn happened. Idle: no arrival owed, nobody attending, and no
 * owed mount waiting to be asked. Owed: a mount is owed until a seat answers
 * again after its last attempt, or the memory it was refused for comes back.
 * Counts: the mount's attempts and the seats it reset, this boot.
 */
typedef enum {
    STAND_MOUNT_AS_IS = 0,
    STAND_MOUNT_SEAT_EMPTIES,
    STAND_MOUNT_SEAT_RINGS,
    STAND_MOUNT_HEAP_REFUSES
} StandMountTurn;

bool        TagFSStandLetGo(void);
void        TagFSStandHoldAgain(void);
void        TagFSStandMountTurn(StandMountTurn turn, uint32_t nth);
void        TagFSStandMountReadComes(void);
void        TagFSStandMountRegistryLoads(void);
bool        TagFSStandMountTurnFired(void);
bool        TagFSStandMountIdle(void);
bool        TagFSStandMountOwed(void);
void        TagFSStandMountCounts(uint32_t* attempts, uint32_t* resets);
#endif

#endif /* TAGFS_MOUNT_AWAY_H */
