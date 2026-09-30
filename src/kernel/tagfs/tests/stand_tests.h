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
//                        record's room claimed, a medium that left answering
//                        the waiters, and a guarded file refused
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
//
// Built only into a VOLUME_TESTS=on kernel, which is the only kernel that has
// the faults these tests arm.
// ============================================================================

#ifdef CONFIG_VOLUME_TESTS

/* Why a stand test cannot be asked on this volume right now, or NULL. A
 * healthy Braid takes every block (tagfs.c, read_block and write_block), so a
 * fault armed on a sector of the volume would never meet the command. */
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

/* The kernel heap (stand_heap_tests.c). */
TestResult TestStandHeapGrowsByRegionsAndGivesBack(void);
TestResult TestStandHeapLargeTakesPagesAndGivesBack(void);
TestResult TestStandHeapClassRefusedReserveServes(void);

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
TestResult TestStandReadAheadHitIsVerified(void);

#endif /* CONFIG_VOLUME_TESTS */

#endif /* TAGFS_STAND_TESTS_H */
