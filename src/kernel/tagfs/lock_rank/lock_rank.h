#ifndef TAGFS_LOCK_RANK_H
#define TAGFS_LOCK_RANK_H

#include "../../../lib/kernel/ktypes.h"
#include "../../../lib/kernel/klib.h"

/*
 * LockRank — every TagFS lock carries a rank, and the kernel names the places
 * that hold one where it must not.
 *
 * A TagFS lock is an interrupts-off spinlock. While one is held its core takes
 * no interrupt, and every other core that wants the same lock spins with its
 * interrupts off too. Two things turn that into a stopped machine, and both
 * were only ever found by reading:
 *
 *   a lock held across a command to the medium — the holder waits as long as
 *   the medium takes, and on a core whose completion only arrives as an
 *   interrupt it waits for ever;
 *
 *   two locks taken in opposite orders on two cores (ABBA), or one taken again
 *   by the core that holds it — nothing moves again, and nothing says why.
 *
 * So each lock knows its rank, each core knows what it holds, and:
 *
 *   the medium's three doors (BoardroomRead / BoardroomWrite / BoardroomFlush)
 *   ask LockRankWitness first, and a door passed with a TagFS lock held on this
 *   core names that lock and where it was taken;
 *
 *   a take is checked BEFORE it spins: only strictly downward is allowed,
 *   COW -> STATE -> LEAF, and a leaf under a leaf is out of order. The ABBA
 *   and the self-deadlock are named before the core could wedge in them. A
 *   TagFSLockTry that fails waits for nobody and is not checked; one that
 *   succeeds is, as a take.
 *
 * A file's writer is no lock: it holds the file's Helm (helm/helm.h), which
 * nobody spins for and which is held across the medium by design.
 *
 * The key is "this core holds a TagFS lock", never "interrupts are off": on
 * one core every system call runs with interrupts off, and a plain file read
 * that holds nothing must say nothing.
 *
 * A place is named once per boot, in one line with the "[LockRank]" prefix;
 * the counters count every time. The host holds each named place under the
 * step of the plan that removes it (tools/stand_caveat.sh, lockrank_judge).
 */

typedef enum {
    LOCK_RANK_NONE   = 0,   /* never given: a lock nobody ranked is named */
    LOCK_RANK_COW    = 1,   /* g_cow_state.lock */
    LOCK_RANK_STATE  = 2,   /* g_state.lock */
    LOCK_RANK_LEAF   = 3    /* while held, no other TagFS lock is taken */
} LockRankLevel;

typedef struct TagFSLock {
    spinlock_t        Spin;
    const char       *Name;        /* word used in [LockRank] lines */
    const char       *HeldBy;      /* __func__ of the holder; written by the holder only */
    uint8_t           Rank;        /* LockRankLevel */
    volatile uint32_t Witnessed;   /* medium commands and Scribe waits made while held, every core */
    volatile uint32_t OutOfOrder;  /* takes of THIS lock that broke the order */
} TagFSLock;

#define TAGFS_LOCK_INIT(name, rank) { .Spin = { 0, 0 }, .Name = (name), .HeldBy = NULL, .Rank = (rank) }

void TagFSLockInit(TagFSLock *lock, const char *name, LockRankLevel rank);
void TagFSLockTakeAt(TagFSLock *lock, const char *site);
bool TagFSLockTryAt (TagFSLock *lock, const char *site);
void TagFSLockDropAt(TagFSLock *lock, const char *site);
#define TagFSLockTake(l) TagFSLockTakeAt((l), __func__)
#define TagFSLockTry(l)  TagFSLockTryAt((l), __func__)
#define TagFSLockDrop(l) TagFSLockDropAt((l), __func__)

/* At a door to the medium, or before a wait for a Scribe: op is "read",
 * "write", "flush" or "wait"; asked_by is the door's return address. */
void LockRankWitness(const char *op, uintptr_t asked_by);

/* Whether this core holds `lock`, or any TagFS lock at all. */
bool LockRankHeldHere(const TagFSLock *lock);
bool LockRankAnyHeldHere(void);

/* This core's counts since boot: doors passed holding a TagFS lock, takes out
 * of order, and places this core named. For tests, as a difference. */
typedef struct { uint32_t Witnessed, OutOfOrder, Named; } LockRankCount;
LockRankCount LockRankCountHere(void);

#endif /* TAGFS_LOCK_RANK_H */
