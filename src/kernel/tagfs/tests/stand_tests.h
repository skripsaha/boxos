#ifndef TAGFS_STAND_TESTS_H
#define TAGFS_STAND_TESTS_H

#include "tests.h"
#include "../tagfs.h"

// ============================================================================
// The stand — the files it is written in, and what they share
//
// stand_tests.c          the helpers below, the self-tests of BLOCKFAIL and
//                        HEAPFAIL, the tag-row precondition, and the table
// stand_caveat_tests.c   the tests that hold a known defect under a caveat
// stand_lock_tests.c     the stand_lock suite: what crosses the medium, or
//                        turns interrupts on, under a lock that must not allow it
// stand_spelling_tests.c the stand_spelling suite: one spelling of a tag at
//                        the stations a program reaches, and the registry's
//                        widths and its full wall
// stand_roadstead_tests.c  the stand_roadstead suite: reads that find every
//                        place on the medium taken wait for their turn
// stand_heap_tests.c     the kernel heap growing by regions, its large pieces,
//                        and a class of cabins refused while the reserve serves
// stand_touch_tests.c    the touch queue's tick firing its nodes in one pass
// stand_gone_tests.c     process.gone asked of a child the reaper takes down,
//                        through every stage of its teardown
// stand_plot_tests.c     the stand_plot suite: where a write may begin, how a
//                        file's extents grow, and a span that only grows
// stand_index_tests.c    the stand_index suite: what the indexes of the live
//                        files cost, a tag's members across a window's edge,
//                        the registry's hashes growing, more files than the
//                        old ceiling asked a page at a time, and what a
//                        registry hands out
// stand_helm_tests.c     the stand_helm suite: two writers of a file taking
//                        turns at its Helm, a reader's blocks outliving a
//                        delete, a defrag beside a delete, a write inked a
//                        window at a time, a write refused part way, and a
//                        guarded file refused
// stand_port_tests.c     the stand_port suite: a program's write through the
//                        port on an AHCI port or a USB disk, a window at a
//                        time
// stand_use_tests.c      the stand_use suite: a context said without a number
//                        issued, two sayings that race to the volume, and the
//                        scheduler's question under a context of any size
// stand_settle_tests.c   the stand_settle suite: a change settled with no
//                        anchor, the idle context owed a quantum on one core,
//                        and a protected medium turning the volume read-only
// stand_run_tests.c      the stand_run suite: a megabyte in one run on the
//                        medium going out in as few commands as the seat
//                        takes, and the k-th block of a run refused answering
//                        the k-1 before it — standing in the call and through
//                        the port
// stand_medium_tests.c   the stand_medium suite: a USB disk's NO MEDIUM to a
//                        flush refused and its ILLEGAL REQUEST remembered, and
//                        the open book taken up — a medium that never left
//                        never judged as its return, and the volume taking
//                        writes again once its book is taken up
// stand_mount_tests.c    the road a mount takes, run last by stand_medium: a
//                        medium leaving mid-mount, a refused read the seat's
//                        bell answers or a reset of the seat does, and a mount
//                        owed until memory comes back
// stand_warrant_tests.c  the stand_warrant suite: a file the system keeps
//                        changed only with the warrant, the loader's blocks by
//                        no call, never gathered, the warrant read off the
//                        cabin, an untag answered with its reason, and a tag
//                        of the system's keys put on only with the warrant
// stand_room_tests.c     the stand_room suite: room a cycle gives back waited
//                        for and never answered
//
// Built only into a VOLUME_TESTS=on kernel, which is the only kernel that has
// the faults these tests arm.
// ============================================================================

#ifdef CONFIG_VOLUME_TESTS

/* Why a stand test cannot be asked on this volume right now, or NULL. A
 * volume assembled in Braid has its every block taken by it (tagfs.c,
 * read_block and write_block), so a fault armed on a sector of the volume
 * would never meet the command. */
const char* StandStepsAsideFor(void);

/* Why this machine has no port road for a program's read or write to walk —
 * one core, or a legacy channel — or NULL when it has one; and one turn of the
 * machine a job through the port continues in, done by hand before the
 * machine runs: the seat's answers brought in, the drain core's batons
 * pumped, the service point run (stand_port_tests.c). */
const char* StandPortAside(uint8_t seat);
void        StandPortTurn(uint8_t seat);

/* Every Quire of the grid of `q` blocks a slot that holds names a node in —
 * its own nodes and the Quires its Tally names a node in as it lies — and the
 * current pair, marked a bit a Quire into a new array (*marks, the caller's to
 * let go; *nquire bits): the blocks they take, a Quire whole — the Tallies'
 * ground as a mount's census holds it. UINT64_MAX with no
 * memory (tests.c). */
uint64_t StandTallyQuires(uint64_t q, uint64_t** marks, uint64_t* nquire);

/* One turn of the machine a cycle of the Settle goes on in, done by hand: the
 * knock, the batons on this core, the service point (stand_settle_tests.c). */
void StandSettleTurn(void);

/* A file of `bytes` bytes of `data`, created and written through the handle
 * API, named `name` and carrying no tag but its name's. False — with nothing
 * left behind — when either step did not answer in full. */
bool StandMakeFile(const char* name, const void* data, uint32_t bytes, uint64_t* out_file_id);

/* Whether the file's record says it was made by the program whose file is
 * `made_by` — 0: by none — the field and TAGFS_FILE_MADE_BY agreeing, as a
 * record always holds them. False too for a file the volume does not have. */
bool StandMadeByIs(uint64_t file_id, uint64_t made_by);

/* The file kept in `role` (TAGFS_FILE_ROLE_*; TAGFS_FILE_ROLE_ORDINARY lets it
 * go), as the image builder keeps a file — the stand's own way, since no call
 * a program makes puts a role on a file (helm/engross.h, EngrossRoled: the
 * tail laid in its block first, then InkRoleLaid). OK, or the refusal. */
error_t StandKeepInRole(uint64_t file_id, uint32_t role);

/* The first file of the volume whose record keeps it in a role and names a
 * block — the image's own system files. False when there is none. */
bool StandKeptFile(uint64_t* out_file_id);

/* The data-run block that holds the k-th block of the file, read off its
 * record — never off the block itself, which would put it in the read-ahead
 * cache a read fault must not be answered from. */
bool StandBlockOf(uint64_t file_id, uint64_t k, uint64_t* out_block);

/* Write `bytes` at `offset` through a fresh handle; tagfs_write's answer, or
 * -1 when the file would not open. */
int  StandWriteAt(uint64_t file_id, uint64_t offset, const void* data, uint32_t bytes);

/* The commands BLOCKFAIL answered under the plan just let go (BlockFailSeen
 * keeps them past BlockFailDisarm, until the next plan), said by a test whose
 * count the Helm's service makes with it — the service and the test race to
 * read the block — so the stand's judge adds what was said to what it counts
 * of every other test (tools/logcheck_book.sh, STAND_BOOK_RACED). */
void StandBlockFailRaced(const char* suite);

/* Whether the volume calls a block taken: the map derived from its Tally and
 * ribbon — where a block that rests in Fallow is taken too, until the cycles
 * after it give it back. */
bool StandBlockInUse(uint64_t block);

/* One storage-deck op asked as `ctx` asks it — NULL for a caller in the
 * kernel, which carries no warrant: the params after the op's head, `in` the
 * in crate when there is one. The handler's answer, or ERR_NOT_INITIALIZED
 * when the op is not registered or its params do not fit. A context whose
 * process has a cabin has its in crate read through that cabin's memory, so a
 * cabin a test makes asks only ops that bring none. */
struct OpContext;
int StandDeckAsk(uint16_t opcode, const void* params, uint16_t param_size, const void* in,
                 uint64_t in_size, const struct OpContext* ctx);

/* The kernel heap (stand_heap_tests.c). */
TestResult TestStandHeapGrowsByRegionsAndGivesBack(void);
TestResult TestStandHeapLargeTakesPagesAndGivesBack(void);
TestResult TestStandHeapClassRefusedReserveServes(void);

/* The touch queue's tick (stand_touch_tests.c). */
TestResult TestStandTouchQueueFireOnePass(void);

/* A death asked about through its teardown (stand_gone_tests.c). */
TestResult TestStandGoneThroughTheTeardown(void);

/* The caveated tests (stand_caveat_tests.c). */
TestResult TestStandRmwReadRefused(void);
TestResult TestStandCreateAllOrNothing(void);
TestResult TestStandWriteReturnsItsGround(void);
TestResult TestStandWriteAllOrRefusal(void);
TestResult TestStandOpenHoldsWhatItNames(void);
TestResult TestStandOpenRefusesUnreadMetadata(void);
TestResult TestStandOpenJoinsTheWriters(void);

/* The road a mount takes (stand_mount_tests.c), run last by stand_medium.
 * StandMountSettles: the room's and the volume's service passes run until the
 * mount road has nothing left to do, bounded. StandSettledBare: cycles until
 * nothing rests in Fallow, and one more. StandMountClose: whatever a mount
 * test left taken down — its turn, BLOCKFAIL, the volume up again, the runner
 * counted back in. */
bool       StandMountSettles(void);
bool       StandSettledBare(void);

/* Room asked for on a book, as a full volume asks it: the ribbon's Quire
 * closed early and the lag's stitches laid past it (SettleForRoom) — what a
 * book's Fallow waits for before it gives ground back. Its answer. */
error_t    StandBookRoom(void);
void       StandMountClose(uint8_t seat);
TestResult TestStandMediumMountLosesItsSeat(void);
TestResult TestStandMediumMountRingsInItsOwnBreath(void);
TestResult TestStandMediumMountBadSectorWaits(void);
TestResult TestStandMediumMountResetsTheSeat(void);
TestResult TestStandMediumMountWaitsForMemory(void);

/* stand_ribbon_tests.c: the volume's medium arriving again, attended until the
 * mount road has nothing left to do — mounted or refused — bounded; and the
 * volume, let go of, mounted again on what the medium holds now and the
 * runner counted back in: true when it came up. A book's suites share them
 * (stand_stitch). */
void       StandRibbonArrive(void);
bool       StandRibbonUp(void);

/* stand_tally_tests.c: why a test written for a book stands aside here, or
 * NULL; the volume's medium arriving again, attended until the mount road has
 * nothing left to do, bounded; and whatever a test left — the plan down, the
 * volume up again if it is not, and the runner counted back in, once. The
 * stand_tally suite's, shared with its case in stand_tally_range_tests.c. */
const char* StandTallyAside(void);
void        StandTallyArrive(void);
void        StandTallyClose(void);

#endif /* CONFIG_VOLUME_TESTS */

#endif /* TAGFS_STAND_TESTS_H */
