#ifndef BOX_SYNC_H
#define BOX_SYNC_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/defs.h"
#include "box/types.h"   /* INLINE, STATIC_ASSERT — sync.h must stand alone */
#include "box/error.h"   /* error_t, OK, ERR_TIMEOUT, ERR_ADDR_VALUE_MISMATCH */

/* -------------------------------------------------------------------------
 * addr_park / addr_wake — park/wake on a memory address.
 *
 * addr_park: atomically checks that *addr == expected, then parks the
 * calling process until addr_wake is called for the same address or the
 * timeout expires.  Returns:
 *   OK                      — woken by addr_wake
 *   ERR_TIMEOUT             — timeout elapsed before wake
 *   ERR_ADDR_VALUE_MISMATCH — *addr != expected at park time (no sleep)
 *   other                   — kernel error
 * timeout_ms == 0 means wait forever.
 *
 * addr_wake: wake up to `count` processes parked on `addr`.
 * count == 0 means wake all.
 *
 * The park is a real kernel park, not a userspace spin: the handler sets the
 * caller PROC_WAITING and hands the completion over as a Result, so a parked
 * strand costs no cycles. The parked-on word must be 64-bit and 8-byte
 * aligned — the handler compares it as a uint64_t and the wait bucket hashes
 * the physical address at 8-byte granularity.
 * ------------------------------------------------------------------------- */
error_t addr_park(const volatile void *addr, uint64_t expected,
                  uint32_t timeout_ms);
error_t addr_wake(const volatile void *addr, uint32_t count);

/* Declared in system.h, implemented in yield.c */
void yield(void);

// ---------------------------------------------------------------------------
// uspin_t — the pure spin lock, for the allocator's own lock ONLY.
//
// Everything else wants umutex_t below, which parks. The heap lock cannot:
// parking goes through addr_park -> MfCall1 -> result_wait, and result_wait's
// ring drain routes a ferry (box::ferry) completion into the strand's ferry
// stash, which result.c allocates lazily with malloc on first use. A malloc
// that parked on a contended heap lock would therefore malloc again from
// inside its own park, and the nesting repeats for as long as the contention
// lasts. A separate TYPE, not a calling convention, keeps the two apart: a new
// heap-path lock cannot pick the wrong one by accident.
//
// (The ferry stash is the only allocation on that path — the touch stash also
// allocates lazily, but the ResultRing consumers discard KCTX_TOUCH rather
// than stash it, so it is reached only from touch_pop / touch_wait.)
//
// The same seam can deadlock without any parking, and this type does not fix
// that: prefault_huge_locked issues MfCall1 while HOLDING the heap lock, so
// that drain can malloc into a lock the caller itself owns. It needs a spawned
// strand with a pending storage completion and no ferry stash yet. Fixing it
// needs an allocation-free drain path — a substrate change, not a header one.
// ---------------------------------------------------------------------------

#define USPIN_SPIN_LIMIT  16   /* spins before yielding */

typedef struct {
    volatile uint32_t locked;
} uspin_t;

#define USPIN_INIT  { 0 }

INLINE void uspin_init(uspin_t *s) {
    s->locked = 0;
}

INLINE bool uspin_trylock(uspin_t *s) {
    return !__sync_lock_test_and_set(&s->locked, 1);
}

INLINE void uspin_lock(uspin_t *s) {
    /* Fast path: uncontended acquire (common case) */
    if (__builtin_expect(!__sync_lock_test_and_set(&s->locked, 1), 1))
        return;

    /* Slow path: spin with backoff, then yield */
    for (;;) {
        for (int i = 0; i < USPIN_SPIN_LIMIT; i++) {
            if (!__sync_lock_test_and_set(&s->locked, 1))
                return;
            __asm__ volatile("pause");
        }
        yield();
    }
}

INLINE void uspin_unlock(uspin_t *s) {
    __sync_lock_release(&s->locked);
}

// ---------------------------------------------------------------------------
// umutex_t — the general userspace mutex: event-driven, not a spin lock.
//
// A cabin holds many strands, so contention is real and a waiter must stop
// burning its core. Two words:
//
//      state    0 (UMUTEX_FREE) or 1 (UMUTEX_HELD) — the parked-on word
//      waiters  how many contenders are parked or about to park
//
// A contender spins UMUTEX_SPIN_LIMIT times — a short critical section
// finishes inside that budget and the syscall is skipped — then announces
// itself in `waiters` and parks on `state` until an unlock frees it. unlock
// enters the kernel only when `waiters` is non-zero, so an uncontended
// lock/unlock pair stays two atomic RMWs and no syscall.
//
// The wake decision is deliberately NOT derived from the lock word. Encoding
// "someone is waiting" as a third state of `state` looks cheaper and is a
// trap: whichever contender then takes the lock erases the mark, and if that
// contender is the one the wake was spent on — a woken timed waiter re-taking
// the lock, say — nobody is left to restore it and the remaining sleeper is
// stranded until its own timeout. A separate count cannot be consumed by an
// acquisition. This mirrors the version-pool gate in atomic_support.cpp.
//
// Two ordering pairs carry the whole algorithm, and both are Dekker-shaped —
// each side writes its own word with a locked RMW and only then reads the
// other's, so at least one of them observes the other:
//   * announce-then-recheck: a waiter bumps `waiters` before re-testing
//     `state`, so an unlock racing it either sees the count and wakes it, or
//     had already freed the word, in which case the re-test takes the lock and
//     the waiter never sleeps at all.
//   * free-then-read-count: unlock frees `state` with a locked exchange before
//     reading `waiters`, so it cannot miss a contender that announced itself.
// The park itself is the third guard: it sleeps only while `state` still reads
// HELD, so an unlock landing after the re-test but before the park makes the
// kernel's park-time compare return immediately instead of sleeping.
//
// The park has no clock of its own. There was one — 100 ms, re-parking for
// ever — over two things the machine "might" do: drop the wake syscall (a
// full pocket ring; pocket_submit now waits for room instead) and move the
// parked-on page from under the sleeper (the kernel keyed the wait by
// physical address; it is keyed by (cabin, VA) now, see addr_wait.h). Neither
// remains, and a clock over a wake that cannot be lost only woke every sleeper
// ten times a second for nothing. Every path still re-checks after the park
// returns, so a spurious wake costs one more loop. Any park failure degrades
// to yield(): correctness never depends on the park syscall being available,
// so a lock taken before the cabin's rings are up still makes progress the
// old way.
//
// ‼ A CONTENDED unlock makes a syscall, and unlock is destructor-reachable
// (~lock_guard, ~unique_lock), including while an exception unwinds. It cannot
// fail the unlock — the lock word is already free before the wake is attempted
// — but it does wait for the kernel's answer to the wake.
//
// Still intentionally simple:
//   - No recursion (recursive locking deadlocks)
//   - No ownership tracking (any context may unlock)
//   - No fairness: a fresh contender may barge past a woken waiter
//   - Suitable for short critical sections (heap, etc.)
// ---------------------------------------------------------------------------

#define UMUTEX_SPIN_LIMIT        16     /* spins before announcing a waiter */

#define UMUTEX_FREE  0u
#define UMUTEX_HELD  1u

typedef struct {
    volatile uint64_t state;     /* UMUTEX_FREE / UMUTEX_HELD — parked-on */
    volatile uint32_t waiters;   /* gate: skip the wake syscall when zero */
} umutex_t;

/* Load-bearing: addr_park reads the parked-on word as a uint64_t and buckets
 * it by physical address at 8-byte granularity, so `state` must be a 64-bit,
 * 8-byte-aligned object. A narrower or misaligned word would have the kernel
 * compare the bytes behind it. */
STATIC_ASSERT(sizeof(((umutex_t *)0)->state) == 8, "umutex state must be the 64-bit park word");
STATIC_ASSERT(OFFSETOF(umutex_t, state) == 0, "umutex state must lead the object");
#ifdef __cplusplus
STATIC_ASSERT(alignof(umutex_t) == 8, "umutex_t must be 8-byte aligned for addr_park");
#else
STATIC_ASSERT(_Alignof(umutex_t) == 8, "umutex_t must be 8-byte aligned for addr_park");
#endif

#define UMUTEX_INIT  { 0, 0 }

INLINE void umutex_init(umutex_t *m) {
    __atomic_store_n(&m->waiters, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&m->state, UMUTEX_FREE, __ATOMIC_RELEASE);
}

INLINE bool umutex_trylock(umutex_t *m) {
    uint64_t expected = UMUTEX_FREE;
    return __atomic_compare_exchange_n(&m->state, &expected, UMUTEX_HELD, false,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

/* One contended attempt: test-and-test-and-set for the spin budget, then
 * announce, re-test, and park until an unlock wakes us — for `timeout_ms` at
 * most, or for as long as it takes when timeout_ms is 0 (addr_park's own
 * meaning of 0).
 *
 * Returns true iff the lock is now held by this caller. A false return means
 * "re-check and come back": the park ended (wake, value change, timeout, or an
 * unavailable syscall), and the caller decides whether to try again. */
INLINE bool umutex_lock_attempt(umutex_t *m, uint32_t timeout_ms) {
    for (int i = 0; i < UMUTEX_SPIN_LIMIT; i++) {
        /* Read before writing: hammering the cacheline with RMWs under
         * contention costs every other core the line. */
        if (__atomic_load_n(&m->state, __ATOMIC_RELAXED) == UMUTEX_FREE &&
            umutex_trylock(m))
            return true;
        __asm__ volatile("pause");
    }

    __atomic_fetch_add(&m->waiters, 1u, __ATOMIC_SEQ_CST);
    bool got = umutex_trylock(m);
    if (!got) {
        error_t rc = addr_park(&m->state, UMUTEX_HELD, timeout_ms);
        if (rc != OK && rc != ERR_ADDR_VALUE_MISMATCH && rc != ERR_TIMEOUT)
            yield();   /* park unavailable — stay correct by yielding */
    }
    __atomic_fetch_sub(&m->waiters, 1u, __ATOMIC_SEQ_CST);
    return got;
}

INLINE void umutex_lock(umutex_t *m) {
    if (__builtin_expect(umutex_trylock(m), 1)) return;   /* uncontended */
    while (!umutex_lock_attempt(m, 0)) { }                 /* until unlocked */
}

/* Timed acquire: one attempt bounded by `timeout_ms`. Returns true iff the lock
 * is held. The caller owns the deadline — it re-checks its own clock and calls
 * again with a fresh budget (same contract as __boxcxx_atomic_wait_until). A
 * budget already spent (0) is one try, never the forever-park that 0 means to
 * addr_park. */
INLINE bool umutex_lock_timeout(umutex_t *m, uint32_t timeout_ms) {
    if (umutex_trylock(m)) return true;
    if (timeout_ms == 0) return false;
    return umutex_lock_attempt(m, timeout_ms);
}

/* A caller that gives up may have been handed the one wake an unlock issued,
 * and it cannot use it. Pass it on, so a sibling still parked does not wait out
 * the backstop on a mutex that is standing free. Only worth a syscall when
 * somebody is actually waiting AND the lock is actually free. */
INLINE void umutex_wake_waiter(umutex_t *m) {
    if (__atomic_load_n(&m->waiters, __ATOMIC_SEQ_CST) != 0 &&
        __atomic_load_n(&m->state, __ATOMIC_ACQUIRE) == UMUTEX_FREE)
        addr_wake(&m->state, 1);
}

INLINE void umutex_unlock(umutex_t *m) {
    /* A locked exchange, not a plain store: it must not sink past the read of
     * `waiters` below (see the Dekker pairs in the header comment). */
    (void)__atomic_exchange_n(&m->state, UMUTEX_FREE, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&m->waiters, __ATOMIC_SEQ_CST) != 0)
        addr_wake(&m->state, 1);
}

#ifdef __cplusplus
}
#endif

#endif // BOX_SYNC_H
