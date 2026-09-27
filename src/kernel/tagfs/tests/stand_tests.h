#ifndef TAGFS_STAND_TESTS_H
#define TAGFS_STAND_TESTS_H

#include "tests.h"

// ============================================================================
// The stand suite — the two files it is written in, and what they share
//
// stand_tests.c          the helpers below, the self-tests of BLOCKFAIL and
//                        HEAPFAIL, the tag-row precondition, and the table
// stand_caveat_tests.c   the tests that hold a known defect under a caveat
//
// Built only into a VOLUME_TESTS=on kernel, which is the only kernel that has
// the faults these tests arm.
// ============================================================================

#ifdef CONFIG_VOLUME_TESTS

/* Why a stand test cannot be asked on this volume right now, or NULL. A
 * healthy Braid takes every block (tagfs.c, read_block and write_block), so a
 * fault armed on a sector of the volume would never meet the command; and a
 * snapshot of the whole volume turns every write into a copy. */
const char* StandStepsAsideFor(void);

/* A file of `bytes` bytes of `data`, created and written through the handle
 * API, named `name` and carrying no tag but its name's. False — with nothing
 * left behind — when either step did not answer in full. */
bool StandMakeFile(const char* name, const void* data, uint32_t bytes, uint32_t* out_file_id);

/* The data-run block that holds the k-th block of the file, read off its
 * record — never off the block itself, which would put it in the read-ahead
 * cache a read fault must not be answered from. */
bool StandBlockOf(uint32_t file_id, uint32_t k, uint32_t* out_block);

/* Write `bytes` at `offset` through a fresh handle; tagfs_write's answer, or
 * -1 when the file would not open. */
int  StandWriteAt(uint32_t file_id, uint64_t offset, const void* data, uint32_t bytes);

/* Whether the allocator's bitmap calls a data-run block taken. */
bool StandBlockInUse(uint32_t block);

/* The caveated tests (stand_caveat_tests.c). */
TestResult TestStandRmwReadRefused(void);
TestResult TestStandCreateAllOrNothing(void);
TestResult TestStandSyncOwnsARefusedWrite(void);
TestResult TestStandSyncOwnsARefusedFlush(void);
TestResult TestStandWriteReturnsItsGround(void);
TestResult TestStandWriteAllOrRefusal(void);
TestResult TestStandOpenHoldsWhatItNames(void);
TestResult TestStandOpenRefusesUnreadMetadata(void);
TestResult TestStandOpenJoinsTheWriters(void);
TestResult TestStandFrozenBlockFreedTwice(void);

#endif /* CONFIG_VOLUME_TESTS */

#endif /* TAGFS_STAND_TESTS_H */
