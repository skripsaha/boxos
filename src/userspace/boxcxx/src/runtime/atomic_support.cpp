/*
 * atomic_support.cpp — out-of-line atomic machinery behind <atomic>:
 *
 *   1. __atomic_*_16 sized entry points (libatomic ABI) via LOCK
 *      CMPXCHG16B. GCC lowers every 16-byte __atomic builtin to these
 *      calls when -mcx16 is not passed — one consistent path. CMPXCHG16B
 *      is baseline on every x86_64 CPU BoxOS targets (CET-era silicon);
 *      operands MUST be 16-aligned (atomic<T> storage guarantees it).
 *      Note the classic libatomic caveat: the 16-byte LOAD is a locked
 *      RMW that rewrites the same bytes — the object must live in
 *      writable memory.
 *
 *   2. The generic size_t-first libatomic protocol (__atomic_load /
 *      store / exchange / compare_exchange / is_lock_free) for
 *      non-power-of-two sizes, backed by an address-hashed lock pool.
 *      The pool is cabin-private: non-lock-free atomics are coherent
 *      only within one cabin (cross-cabin Bay contract = lock-free
 *      types only, see <atomic> header).
 *
 *   3. __boxcxx_atomic_wait_cycle / __boxcxx_atomic_notify — atomic wait/
 *      notify backed by a real kernel park on a per-pool version counter.
 *      A waiter bounded-spins first (UMONITOR/UMWAIT on the value's
 *      cacheline when WAITPKG exists, PAUSE budget otherwise), then parks
 *      via addr_park on its pool's `ver`; a notifier bumps `ver` and calls
 *      addr_wake. The value/width compare stays in userspace (memcmp), so
 *      the kernel never compares variable widths. The pools are a cabin-
 *      private hashed table (shared across strands via one CR3).
 */

#include <cstddef>
#include <cstdint>

#include "box/cpu.h"
#include "box/string.h"
#include "box/sync.h"

using u128 = unsigned __int128;

namespace {

struct alignas(16) Pair128 {
    uint64_t lo;
    uint64_t hi;
};

// LOCK CMPXCHG16B — full barrier. On match writes `desired` and returns
// true; on mismatch loads the current value into *expected and returns
// false. With desired == *expected it doubles as the atomic 16-byte load.
bool Cas16(volatile void *mem, u128 *expected, u128 desired)
{
    auto *e = reinterpret_cast<Pair128 *>(expected);
    Pair128 d;
    __builtin_memcpy(&d, &desired, 16);

    bool ok;
    __asm__ volatile("lock cmpxchg16b %[mem]"
                     : [mem] "+m"(*reinterpret_cast<volatile Pair128 *>(mem)),
                       "+a"(e->lo), "+d"(e->hi), "=@ccz"(ok)
                     : "b"(d.lo), "c"(d.hi)
                     : "memory");
    return ok;
}

} // namespace

// ── sized 16-byte entry points (libatomic ABI) ──────────────────────────

extern "C" {

u128 __atomic_load_16(const volatile void *mem, int)
{
    u128 cur = 0;
    (void)Cas16(const_cast<volatile void *>(mem), &cur, cur);
    return cur;
}

void __atomic_store_16(volatile void *mem, u128 val, int)
{
    u128 cur = 0;
    while (!Cas16(mem, &cur, val)) {
    }
}

u128 __atomic_exchange_16(volatile void *mem, u128 val, int)
{
    u128 cur = 0;
    while (!Cas16(mem, &cur, val)) {
    }
    return cur;
}

bool __atomic_compare_exchange_16(volatile void *mem, void *expected,
                                  u128 desired, bool /*weak*/, int, int)
{
    u128 e;
    __builtin_memcpy(&e, expected, 16);
    bool ok = Cas16(mem, &e, desired);
    if (!ok) __builtin_memcpy(expected, &e, 16);
    return ok;
}

#define BOXCXX_FETCH16(name, expr)                                  \
    u128 __atomic_fetch_##name##_16(volatile void *mem, u128 val,   \
                                    int model)                      \
    {                                                               \
        u128 cur = __atomic_load_16(mem, model);                    \
        while (!Cas16(mem, &cur, (expr))) {                         \
        }                                                           \
        return cur;                                                 \
    }

BOXCXX_FETCH16(add, cur + val)
BOXCXX_FETCH16(sub, cur - val)
BOXCXX_FETCH16(and, cur & val)
BOXCXX_FETCH16(or, cur | val)
BOXCXX_FETCH16(xor, cur ^ val)
BOXCXX_FETCH16(nand, ~(cur & val))

#undef BOXCXX_FETCH16

// OP-fetch twins (returning the NEW value) — GCC lowers the
// __atomic_OP_fetch builtins (operator sugar like `a += d`) to these.
#define BOXCXX_OPFETCH16(name, expr)                                  \
    u128 __atomic_##name##_fetch_16(volatile void *mem, u128 val,     \
                                    int model)                        \
    {                                                                 \
        u128 cur = __atomic_load_16(mem, model);                      \
        u128 next;                                                    \
        do {                                                          \
            next = (expr);                                            \
        } while (!Cas16(mem, &cur, next));                            \
        return next;                                                  \
    }

BOXCXX_OPFETCH16(add, cur + val)
BOXCXX_OPFETCH16(sub, cur - val)
BOXCXX_OPFETCH16(and, cur & val)
BOXCXX_OPFETCH16(or, cur | val)
BOXCXX_OPFETCH16(xor, cur ^ val)
BOXCXX_OPFETCH16(nand, ~(cur & val))

#undef BOXCXX_OPFETCH16

} // extern "C"

// ── generic protocol (odd sizes) over the lock pool ─────────────────────

namespace {

constexpr unsigned kLockPoolSize = 64; // power of two

volatile uint32_t g_atomic_lock_pool[kLockPoolSize];

struct PoolGuard {
    volatile uint32_t *lock;

    explicit PoolGuard(const volatile void *mem)
    {
        uintptr_t a = reinterpret_cast<uintptr_t>(mem);
        lock = &g_atomic_lock_pool[(a >> 4) & (kLockPoolSize - 1)];
        while (__atomic_exchange_n(lock, 1u, __ATOMIC_ACQUIRE))
            __asm__ volatile("pause");
    }
    ~PoolGuard() { __atomic_store_n(lock, 0u, __ATOMIC_RELEASE); }
};

// ── park/wake version-pool ──────────────────────────────────────────────
// Cabin-private (libboxcxx .bss, shared across strands via one CR3). A waiter
// parks on its pool's 8-byte `ver`; a notifier bumps `ver` and wakes. Hashed,
// so distinct addresses may share a pool — harmless (a woken waiter re-checks
// its own predicate and re-parks). Separate from g_atomic_lock_pool above.
constexpr unsigned kWaitPoolCount = 256;     // power of two
struct alignas(64) WaitPool {                // one cacheline per pool (no false-share)
    uint64_t ver;                            // bumped by notify; parked-on by wait
    uint32_t waiters;                        // gate: skip addr_wake when zero
};
WaitPool g_wait_pools[kWaitPoolCount];       // .bss, zero-init

inline WaitPool *WaitPoolFor(const volatile void *addr)
{
    uintptr_t a = reinterpret_cast<uintptr_t>(addr);
    return &g_wait_pools[(a >> 4) & (kWaitPoolCount - 1)];
}

constexpr uint32_t kWaitBackstopMs = 100;    // defense-in-depth; never load-bearing

// Predicate-proxy compare: did the bits the caller actually waits on change?
// `mask` selects only those bits, so an unrelated field packed in the same word
// (barrier's count beside its phase, shared_mutex's reader count beside the write
// bit) churning under multi-strand contention does NOT spuriously end the park.
// Callers watching the whole value pass mask = ~0. Widths the kernel cannot mask
// (16-byte / odd-size atomics — always whole-value predicates) use a full memcmp.
inline bool WaitWordChanged(const volatile void *addr, const void *observed,
                            unsigned width, unsigned long long mask)
{
    switch (width) {
    case 1:
        return ((uint64_t)(*(const volatile uint8_t *)addr ^
                           *(const uint8_t *)observed) & mask) != 0;
    case 2:
        return ((uint64_t)(*(const volatile uint16_t *)addr ^
                           *(const uint16_t *)observed) & mask) != 0;
    case 4:
        return ((uint64_t)(*(const volatile uint32_t *)addr ^
                           *(const uint32_t *)observed) & mask) != 0;
    case 8:
        return ((*(const volatile uint64_t *)addr ^
                 *(const uint64_t *)observed) & mask) != 0;
    default:
        return __builtin_memcmp((const void *)addr, observed, width) != 0;
    }
}

} // namespace

extern "C" {

void __atomic_load(size_t size, const volatile void *mem, void *ret, int)
{
    PoolGuard guard(mem);
    memcpy(ret, const_cast<const void *>(mem), size);
}

void __atomic_store(size_t size, volatile void *mem, void *val, int)
{
    PoolGuard guard(mem);
    memcpy(const_cast<void *>(mem), val, size);
}

void __atomic_exchange(size_t size, volatile void *mem, void *val, void *ret,
                       int)
{
    PoolGuard guard(mem);
    memcpy(ret, const_cast<const void *>(mem), size);
    memcpy(const_cast<void *>(mem), val, size);
}

bool __atomic_compare_exchange(size_t size, volatile void *mem, void *expected,
                               void *desired, int, int)
{
    PoolGuard guard(mem);
    if (memcmp(const_cast<const void *>(mem), expected, size) == 0) {
        memcpy(const_cast<void *>(mem), desired, size);
        return true;
    }
    memcpy(expected, const_cast<const void *>(mem), size);
    return false;
}

bool __atomic_is_lock_free(size_t size, const volatile void *mem)
{
    if (size != 1 && size != 2 && size != 4 && size != 8 && size != 16)
        return false;
    uintptr_t a = reinterpret_cast<uintptr_t>(mem);
    return mem == nullptr || (a & (size - 1)) == 0;
}

// ── atomic wait/notify machinery ────────────────────────────────────────

void __boxcxx_atomic_wait_cycle(const volatile void *addr, const void *observed,
                                unsigned width, unsigned long long mask,
                                unsigned *spin_state)
{
    // Phase 1 — bounded adaptive spin: dodge a syscall for very short waits.
    // (Same WAITPKG/PAUSE policy family as before; now a *prelude* to a real park.)
    if (cpu_has_waitpkg()) {
        // Monitor the data word's cacheline; a notifier's store ends UMWAIT.
        // umwait self-bounds at the cap — allow a couple of rounds, then park.
        if (*spin_state < 2u) {
            (*spin_state)++;
            umonitor(const_cast<volatile void *>(addr));
            uint64_t deadline = cpu_rdtsc() + cpu_ms_to_tsc(50);
            (void)umwait(0, deadline);
            return;                          // caller re-checks its predicate
        }
    } else {
        constexpr unsigned kSpinBudget = 1u << 14;
        if ((*spin_state)++ < kSpinBudget) {
            __asm__ volatile("pause");
            return;                          // caller re-checks its predicate
        }
    }

    // Phase 2 — real kernel park on the version-pool.
    *spin_state = 0;                         // post-wake: spin-recheck before re-parking
    WaitPool *p = WaitPoolFor(addr);
    __atomic_fetch_add(&p->waiters, 1u, __ATOMIC_SEQ_CST);
    uint64_t v = __atomic_load_n(&p->ver, __ATOMIC_SEQ_CST);   // snapshot BEFORE re-check
    // Predicate proxy: if the bits the caller waits on already changed, its
    // predicate may now hold — bail without parking. `mask` keeps an unrelated
    // co-packed field (barrier count beside phase, reader count beside the write
    // bit) from spuriously ending the park under multi-strand contention.
    if (WaitWordChanged(addr, observed, width, mask)) {
        __atomic_fetch_sub(&p->waiters, 1u, __ATOMIC_SEQ_CST);
        return;
    }
    // Park until ver != v (a notify bumps it) or the backstop fires. The kernel's
    // park-time pre-check compares *ver==v atomically; combined with snapshot-
    // before-recheck this closes the notify-races-park window with no lost wakeup.
    addr_park(&p->ver, v, kWaitBackstopMs);
    __atomic_fetch_sub(&p->waiters, 1u, __ATOMIC_SEQ_CST);
}

void __boxcxx_atomic_notify(const volatile void *addr, bool all)
{
    (void)all;   // address-hashed pool: both one and all wake the whole pool
                 // (conformant — notify_one may unblock more than one waiter)
    WaitPool *p = WaitPoolFor(addr);
    __atomic_fetch_add(&p->ver, 1u, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&p->waiters, __ATOMIC_SEQ_CST) != 0)
        addr_wake(&p->ver, 0);               // count=0 = wake all parked on this ver
}

} // extern "C"
