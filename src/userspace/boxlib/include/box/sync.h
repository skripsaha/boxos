#ifndef BOX_SYNC_H
#define BOX_SYNC_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/defs.h"
#include "box/types.h"   /* INLINE — sync.h must stand alone */

// ---------------------------------------------------------------------------
// Lightweight userspace mutex.
//
// Implementation: atomic test-and-set with backoff.
// On contention (rare on uniprocessor without threads) spins briefly with
// PAUSE, then yields to the scheduler to avoid priority inversion.
//
// This mutex is intentionally simple:
//   - No recursion (recursive locking deadlocks)
//   - No ownership tracking (any context may unlock)
//   - Suitable for short critical sections (heap, etc.)
// ---------------------------------------------------------------------------

#define UMUTEX_SPIN_LIMIT  16   // spins before yielding

typedef struct {
    volatile uint32_t locked;
} umutex_t;

#define UMUTEX_INIT  { 0 }

INLINE void umutex_init(umutex_t *m) {
    m->locked = 0;
}

// Declared in system.h, implemented in yield.c
void yield(void);

INLINE void umutex_lock(umutex_t *m) {
    // Fast path: uncontended acquire (common case)
    if (__builtin_expect(!__sync_lock_test_and_set(&m->locked, 1), 1))
        return;

    // Slow path: spin with backoff, then yield
    for (;;) {
        for (int i = 0; i < UMUTEX_SPIN_LIMIT; i++) {
            if (!__sync_lock_test_and_set(&m->locked, 1))
                return;
            __asm__ volatile("pause");
        }
        yield();
    }
}

INLINE bool umutex_trylock(umutex_t *m) {
    return !__sync_lock_test_and_set(&m->locked, 1);
}

INLINE void umutex_unlock(umutex_t *m) {
    __sync_lock_release(&m->locked);
}

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
 * ------------------------------------------------------------------------- */
#include "box/error.h"  /* error_t, ERR_ADDR_VALUE_MISMATCH */

error_t addr_park(const volatile void *addr, uint64_t expected,
                  uint32_t timeout_ms);
error_t addr_wake(const volatile void *addr, uint32_t count);

#ifdef __cplusplus
}
#endif

#endif // BOX_SYNC_H
