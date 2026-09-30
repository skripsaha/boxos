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
 *
 * Once, and only the Nth: every matching command before and after it reaches
 * the medium, and the count goes on while the plan stands, so a test can prove
 * that the one it meant was the one refused. A transient error, not a bad
 * sector — a sector that keeps refusing is a different question, for the
 * driver-level stand (QEMU blkdebug).
 *
 * Chosen by seat and sectors, never by core: the command is refused wherever
 * it was issued from. Asked on the synchronous reads and writes, the flush,
 * the asynchronous read, and the asynchronous WriteJob's reads and writes —
 * the job submits to AHCI itself instead of through the Boardroom, and asks
 * the plan there (write_job.c).
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
 * called from an interrupt handler. */
bool     BlockFailStrikes(uint8_t seat, BlockFailKind kind,
                          uint64_t lba, uint32_t count);

/* A medium that takes no write, on `seat`: while it stands, every write is
 * refused the way the seat's driver refuses one, never issued, and the seat
 * says it refuses writes as protected (BoardroomSeatRefusesWrites) — what
 * DATA PROTECT is on USB and an abort on a write is on AHCI. Off lifts it; the
 * volume a protection turned read-only stays so until it is mounted again. */
void     BlockFailProtect(uint8_t seat, bool on);
bool     BlockFailProtects(uint8_t seat);

#endif /* CONFIG_VOLUME_TESTS */

#endif /* BLOCK_FAIL_H */
