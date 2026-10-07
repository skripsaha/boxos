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
// stand_chain_tests.c    the stand_chain suite: the registry's and the file
//                        table's chains as they grow, registries that are not
//                        the volume's, and what a registry hands out; and the
//                        chain walk below
// stand_spelling_tests.c the stand_spelling suite: one spelling of a tag at
//                        the stations a program reaches, and the registry's
//                        widths and its full wall
// stand_roadstead_tests.c  the stand_roadstead suite: reads that find every
//                        place on the medium taken wait for their turn
// stand_heap_tests.c     the kernel heap growing by regions, its large pieces,
//                        and a class of cabins refused while the reserve serves
// stand_touch_tests.c    the touch queue's tick firing its nodes in one pass
// stand_map_tests.c      the stand_map suite: where a take goes, a give that
//                        comes twice, and the ground a growing file keeps
// stand_plot_tests.c     the stand_plot suite: where a write may begin, what a
//                        gap reads as, how a file's extents grow, and span
// stand_index_tests.c    the stand_index suite: what the indexes of the live
//                        files cost, a tag's members across a window's edge,
//                        the registry's hashes growing, more files than the
//                        old ceiling asked a page at a time, and the last
//                        file id a volume issues
// stand_ink_tests.c      the stand_ink suite: where the pool lays every file's
//                        record, a deleted file's newest record, and a volume
//                        with no free block that still deletes
// stand_helm_tests.c     the stand_helm suite: two writers of a file taking
//                        turns at its Helm, a reader's blocks outliving a
//                        delete, a defrag beside a delete, a write inked a
//                        window at a time, a write refused part way, the
//                        record's room claimed, and a guarded file refused
// stand_port_tests.c     the stand_port suite: a program's write through the
//                        port on an AHCI port or a USB disk, a window at a
//                        time, and a block's write refused part way
// stand_use_tests.c      the stand_use suite: a context said without a number
//                        issued, two sayings that race to the volume, and the
//                        scheduler's question under a context of any size
// stand_seal_tests.c     the stand_seal suite: the seal a cycle leaves, a cycle
//                        cut at its pool and its Ledger, seals the medium does
//                        not bear out, a table torn under a seal, and the next
//                        file id derived
// stand_settle_tests.c   the stand_settle suite: a change settled with no
//                        anchor, an anchor answered through its Chit, the idle
//                        context owed a quantum on one core, and a protected
//                        medium turning the volume read-only
// stand_glean_tests.c    the stand_glean suite: ten thousand overwrites of one
//                        file, and a uniform load of sixty-four, within the
//                        chain's slack; the Deed's head still the pool's and
//                        walked whole from; a block taken off resting past its
//                        cycle; and a lie after a glean falling back to the
//                        other copy's own chain
// stand_snapshot_tests.c the stand_snapshot suite: the snapshots an earlier
//                        kernel left, named and refused, and what they kept
//                        claimed by no owner
// stand_run_tests.c      the stand_run suite: a megabyte in one run on the
//                        medium going out in as few commands as the seat
//                        takes, and the k-th block of a run refused answering
//                        the k-1 before it — standing in the call and through
//                        the port
// stand_medium_tests.c   the stand_medium suite: a USB disk's NO MEDIUM to a
//                        flush refused and its ILLEGAL REQUEST remembered, and
//                        the open book taken up and given up — its fate done
//                        where it is decided, a medium that never left never
//                        judged as its return, and the volume taking writes
//                        again once its book is taken up
// stand_mount_tests.c    the road a mount takes, run last by stand_medium: a
//                        medium leaving mid-mount, a refused read the seat's
//                        bell answers or a reset of the seat does, a mount
//                        owed until memory comes back, and a return judged
//                        only on reads the medium did not refuse
// stand_warrant_tests.c  the stand_warrant suite: a file the system keeps
//                        changed only with the warrant, the loader's blocks by
//                        no call, never gathered, the warrant read off the
//                        cabin, and an untag answered with its reason
// stand_room_tests.c     the stand_room suite: room a cycle gives back waited
//                        for and never answered — a write through the port
//                        that needs a Spare, a create that grows the registry,
//                        and writes on the core whose cycle stands between two
//                        of its steps
// stand_integrity_tests.c the stand_integrity suite: the integrity map covers
//                        no block of the volume's own — a page pushed out into
//                        a map being laid records nothing, and a pool block
//                        whose digest went stale is read by Glean all the same
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

/* Whether the block map calls a data-run block taken. */
bool StandBlockInUse(uint32_t block);

/* The newest intact Ledger's sequence number, read off the medium by the
 * Ledger's own reader; 0 when neither copy reads. */
uint64_t StandLedgerSeq(void);

/* The file's row of the file table as the MEDIUM holds it, and the data-run
 * block that carries it (out_block may be NULL). False when the chain on the
 * medium does not reach the row. */
bool StandTableRowOnMedium(uint32_t file_id, FileTableEntry* out, uint32_t* out_block);

/*
 * One metadata chain — the registry's, the file table's or the pool's — as
 * the MEDIUM holds it, walked from the head the Deed names (a volume block) by
 * the magic and next link every block of the three carries at offsets 0 and 4.
 * The walk stops at a link of zero, at a block without the magic (broken, and
 * where), or after as many hops as the data run has blocks.
 */
typedef struct {
    uint32_t  links;        /* blocks walked that carry the magic */
    uint32_t  last;         /* the last of them, a block of the data run */
    bool      broken;       /* a link named a block without the magic */
    uint32_t  broken_at;    /* that block */
    uint32_t* blocks;       /* the caller's room for the links in chain order, or NULL */
    uint32_t  capacity;     /* its length; links past it are counted, not listed */
} StandChain;

/* False only when a block of the chain would not read. */
bool StandChainOnMedium(uint32_t head_volume_block, uint32_t magic, StandChain* out);

/* Both chains as the medium holds them, said in one [TESTS] line once the
 * startup tests are done — for the host, which aims what grows them next. */
void StandSayChainsLeft(void);

/* Whether the volume's tag registry, read off the medium block by block, holds
 * a record of `key` and `value` (NULL: a record with no value). */
bool StandRegistryHoldsOnMedium(const char* key, const char* value);

/* The volume's free ground taken whole and given back (stand_ink_tests.c),
 * for the tests that ask what a volume with no free block does. */
typedef struct {
    uint32_t* at;
    uint32_t* n;
    uint32_t  count;
    uint32_t  cap;
} StandInkGround;

/* Every free block taken, run by run: the answer that ended it — the
 * allocator's refusal — or ERR_NO_MEMORY for no memory to list a run, with
 * what was taken so far still listed for StandInkGiveBack. */
error_t StandInkTakeAll(StandInkGround* g);
void    StandInkGiveBack(StandInkGround* g);

/* The kernel heap (stand_heap_tests.c). */
TestResult TestStandHeapGrowsByRegionsAndGivesBack(void);
TestResult TestStandHeapLargeTakesPagesAndGivesBack(void);
TestResult TestStandHeapClassRefusedReserveServes(void);

/* The touch queue's tick (stand_touch_tests.c). */
TestResult TestStandTouchQueueTickOnePass(void);

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

/* The road a mount takes (stand_mount_tests.c), run last by stand_medium.
 * StandMountSettles: the room's and the volume's service passes run until the
 * mount road has nothing left to do, bounded. StandSettledBare: cycles until
 * nothing rests in Fallow, and one more. StandMountClose: whatever a mount
 * test left taken down — its turn, BLOCKFAIL, the volume up again, the runner
 * counted back in. */
bool       StandMountSettles(void);
bool       StandSettledBare(void);
void       StandMountClose(uint8_t seat);
TestResult TestStandMediumMountLosesItsSeat(void);
TestResult TestStandMediumMountRingsInItsOwnBreath(void);
TestResult TestStandMediumMountBadSectorWaits(void);
TestResult TestStandMediumMountRefusedReadIsNoVerdict(void);
TestResult TestStandMediumGivenUpIdsNotGivenAgain(void);
TestResult TestStandMediumReturnRefusedOnce(void);
TestResult TestStandMediumMountResetsTheSeat(void);
TestResult TestStandMediumMountWaitsForMemory(void);

#endif /* CONFIG_VOLUME_TESTS */

#endif /* TAGFS_STAND_TESTS_H */
