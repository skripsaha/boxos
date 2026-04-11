#ifndef BOX_SYNC_H
#define BOX_SYNC_H

#include "box/defs.h"

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

#endif // BOX_SYNC_H
