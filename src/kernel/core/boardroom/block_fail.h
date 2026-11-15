#ifndef BLOCK_FAIL_H
#define BLOCK_FAIL_H

#include "ktypes.h"

/*
 * BLOCKFAIL — one command, refused the way the medium refuses it.
 *
 * A write path is only as honest as its answer to a medium that says no, and a
 * medium that says no cannot be summoned on a desk: QEMU's disks answer every
 * command, so every road a refusal takes through TagFS had been read and none
 * of them had been walked. This is how a startup test walks one. It names a
 * seat, a kind of command and a run of sectors, and the Nth matching command
 * that reaches the Boardroom is answered with exactly what that seat's driver
 * answers after its own retries have run out — and is never issued, so a
 * refused write leaves the medium as it was, which is what a refused write is.
 * A write that reaches the plan's first sector part way is refused FROM that
 * sector, the way a medium that names the sector it failed at refuses it: the
 * sectors of the command before it reach the medium, and the refusal says how
 * many (BoardroomWrite's `landed`), so the Nth block of a run is refusable as
 * it was when every block went out alone.
 *
 * Once, and only the Nth: every matching command before and after it reaches
 * the medium, and the count goes on while the plan stands, so a test can prove
 * that the one it meant was the one refused. A transient error, not a bad
 * sector: a sector the medium says it cannot read keeps saying so, and is a
 * plan of its own (BlockFailArmBadSectors).
 *
 * Chosen by seat and sectors, never by core: the command is refused wherever
 * it was issued from. Asked on the synchronous reads and writes, the flush,
 * and the reads and writes nobody stands over (BoardroomReadAsync,
 * BoardroomWriteAsync) — an asynchronous WriteJob's among them, on an AHCI
 * port or a USB disk alike.
 *
 * Built only into a VOLUME_TESTS=on kernel. A kernel that ships has no plan to
 * consult, and pays nothing.
 */

#ifdef CONFIG_VOLUME_TESTS

typedef enum {
    BLOCK_FAIL_READ = 1,
    BLOCK_FAIL_WRITE,
    BLOCK_FAIL_FLUSH
} BlockFailKind;

/* The nth of a plan that only counts. The count runs in 31 bits and turns over
 * before it could reach this, so no command is ever the one refused: a test
 * that asks how many commands of a kind met a run of sectors arms with it and
 * reads BlockFailSeen, and nothing is said on the log. */
#define BLOCK_FAIL_NEVER 0xFFFFFFFFu

/* Refuse the nth command of `kind` on `seat` that touches any of the sectors
 * [first_sector, first_sector + sectors); sectors == 0 means the whole seat,
 * and a flush has no sectors. nth == 0 is taken as 1. Replaces any plan that
 * was standing, and starts the count again. */
void     BlockFailArm(uint8_t seat, BlockFailKind kind,
                      uint64_t first_sector, uint64_t sectors, uint32_t nth);

/* Take the plan down. True when one was standing — fired or not. From its
 * return on, no command is counted toward the plan it took down, and none is
 * chosen to be refused by it: a strike still between reading the plan and
 * counting its command lets the command through, uncounted, on any core. A
 * command chosen before the disarm — its strike had fired, and BlockFailFired
 * already said so — is still refused on its way back. What the plan counted
 * stays readable (BlockFailSeen, BlockFailFired) until the next arm. */
bool     BlockFailDisarm(void);

/* Matching commands met since the plan was armed, the refused one included. */
uint32_t BlockFailSeen(void);

/* Whether the plan has refused its command. */
bool     BlockFailFired(void);

/* The Boardroom's question, asked once per command before it reaches a
 * driver: is this the one to refuse? Says so on the log when it is. Never
 * called from an interrupt handler. `keep`: NULL refuses the whole command;
 * else the caller issues the sectors before the plan's first one itself, and
 * learns here how many that is — 0 when the command begins inside the plan. */
bool     BlockFailStrikes(uint8_t seat, BlockFailKind kind,
                          uint64_t lba, uint32_t count, uint32_t* keep);

/* A flush answered, the nth time, the way a USB disk answers SYNCHRONIZE
 * CACHE it did not carry out: with the SCSI sense key, additional code and
 * qualifier given here, which the disk's own driver then judges and remembers
 * as it would the device's (xhci_msd_flush_answered) — NOT READY / 0x3A, no
 * medium; ILLEGAL REQUEST, no such command. On a seat that is not a USB disk
 * the sense has no one to be said to, and the flush is refused as a medium
 * error. Replaces any plan that was standing. */
void     BlockFailArmFlushSense(uint8_t seat, uint8_t key, uint8_t asc, uint8_t ascq,
                                uint32_t nth);

/* The flush's question: BlockFailStrikes for a flush, and the sense its plan
 * answers with in out_sense[key, asc, ascq] — all zero for a plan that
 * refuses it as a medium error. */
bool     BlockFailFlushStrikes(uint8_t seat, uint8_t out_sense[3]);

/* Sectors the medium cannot read, on `seat`: from now on every read that
 * touches [first_sector, first_sector + sectors) is answered the way that
 * seat's driver answers a sector the device says it cannot read — UNC on ATA
 * and AHCI, MEDIUM ERROR on USB — and is never issued, until the plan comes
 * down. Not a refusal that may pass: a sector that went bad stays bad until it
 * is written over, and every read of it, a retry's included, is the medium's
 * same answer (ERR_BAD_SECTOR at the Boardroom). Writes go through. Every
 * refused read is said and counted. Replaces any plan that was standing. */
void     BlockFailArmBadSectors(uint8_t seat, uint64_t first_sector, uint64_t sectors);

/* Sectors the medium will not write, on `seat`: from now on every write that
 * touches [first_sector, first_sector + sectors) is refused as a medium error,
 * from the first sector of the plan's ranges it reaches — the sectors before it
 * land, as a medium that names the sector it failed at keeps them — and never
 * issued past it, until the plan comes down. Not once but for good: a block of
 * flash that will not take a write, or a medium whose every write fails, which
 * a writer meets again however often it tries (M-6, C25). Reads go through.
 * Every refused write is said and counted. Replaces any plan that was standing. */
void     BlockFailArmRefusedWrites(uint8_t seat, uint64_t first_sector, uint64_t sectors);

/* One more run of sectors the medium cannot read — or will not write — in the
 * plan of bad sectors or of refused writes that stands on `seat`: rot in two
 * places at once, two copies of one node (C19); a writer's every Quire. The
 * plan is stood again with it, its count from zero. False, and nothing
 * changed, when no such plan stands for the seat or it holds
 * BLOCK_FAIL_BAD_RANGES runs already. */
#define BLOCK_FAIL_BAD_RANGES  4u
bool     BlockFailAddBadSectors(uint8_t seat, uint64_t first_sector, uint64_t sectors);

/* The read's question: BlockFailStrikes for a read, and in *bad whether its
 * plan answers it as sectors the medium cannot read. */
bool     BlockFailReadStrikes(uint8_t seat, uint64_t lba, uint32_t count, bool* bad);

/* A medium that takes no write, on `seat`: while it stands, every write is
 * refused the way the seat's driver refuses one, never issued, and the seat
 * says it refuses writes as protected (BoardroomSeatRefusesWrites) — what
 * DATA PROTECT is on USB and an abort on a write is on AHCI. Off lifts it; the
 * volume a protection turned read-only stays so until it is mounted again. */
void     BlockFailProtect(uint8_t seat, bool on);
bool     BlockFailProtects(uint8_t seat);

/* A seek the stand's medium does not make (M-13; the build key
 * SEEKLAG=): every read command on `seat` waits `us` microseconds before its
 * driver is handed it, as a disk's head travels before the first sector comes
 * — so what a read costs is the medium's own measure, and the mount's check of
 * the newest stitch is held to the D the writer judged it by on a medium that
 * seeks. One seat at a time, the volume's; 0 lifts it. Never a refusal: the
 * command goes out after its wait, and no plan counts it. */
void     BlockFailReadLag(uint8_t seat, uint32_t us);

/* The wait a read command on `seat` makes before it goes out, made here — on
 * every road a read takes (BoardroomRead, BoardroomReadAsync). */
void     BlockFailReadLagged(uint8_t seat);

/* The pace the stand names for the volume's reads, in place of the one the
 * medium was seen to keep (read_pace.h): while it stands, the writer judges a
 * stitch's check by `rate` bytes a second and `seek_us` a seek — `rate` 0 for
 * no measure at all — so a test meets either answer of the hybrid on a
 * medium as fast as QEMU's, whose own times drift (R11). Unpinned,
 * the medium's own measure is the writer's again. */
void     BlockFailReadPace(bool pinned, uint64_t rate, uint64_t seek_us);

/* Whether a pace stands, and it. */
bool     BlockFailReadPaced(uint64_t* rate, uint64_t* seek_us);

#endif /* CONFIG_VOLUME_TESTS */

#endif /* BLOCK_FAIL_H */
