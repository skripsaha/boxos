#ifndef BOX_SYNC_H
#define BOX_SYNC_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/defs.h"
#include "box/types.h"
#include "box/error.h"

error_t addr_park(const volatile void *addr, uint64_t expected,
                  uint32_t timeout_ms);
error_t addr_wake(const volatile void *addr, uint32_t count);

void yield(void);


#define USPIN_SPIN_LIMIT  16

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
    if (__builtin_expect(!__sync_lock_test_and_set(&s->locked, 1), 1))
        return;

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


#define UMUTEX_SPIN_LIMIT        16

#define UMUTEX_FREE  0u
#define UMUTEX_HELD  1u

typedef struct {
    volatile uint64_t state;
    volatile uint32_t waiters;
} umutex_t;

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

INLINE bool umutex_lock_attempt(umutex_t *m, uint32_t timeout_ms) {
    for (int i = 0; i < UMUTEX_SPIN_LIMIT; i++) {
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
            yield();
    }
    __atomic_fetch_sub(&m->waiters, 1u, __ATOMIC_SEQ_CST);
    return got;
}

INLINE void umutex_lock(umutex_t *m) {
    if (__builtin_expect(umutex_trylock(m), 1)) return;
    while (!umutex_lock_attempt(m, 0)) { }
}

INLINE bool umutex_lock_timeout(umutex_t *m, uint32_t timeout_ms) {
    if (umutex_trylock(m)) return true;
    if (timeout_ms == 0) return false;
    return umutex_lock_attempt(m, timeout_ms);
}

INLINE void umutex_wake_waiter(umutex_t *m) {
    if (__atomic_load_n(&m->waiters, __ATOMIC_SEQ_CST) != 0 &&
        __atomic_load_n(&m->state, __ATOMIC_ACQUIRE) == UMUTEX_FREE)
        addr_wake(&m->state, 1);
}

INLINE void umutex_unlock(umutex_t *m) {
    (void)__atomic_exchange_n(&m->state, UMUTEX_FREE, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&m->waiters, __ATOMIC_SEQ_CST) != 0)
        addr_wake(&m->state, 1);
}

#ifdef __cplusplus
}
#endif

#endif