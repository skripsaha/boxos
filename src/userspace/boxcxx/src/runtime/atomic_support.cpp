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
 *   3. __boxcxx_atomic_wait_cycle — one wait cycle for atomic wait/
 *      notify: UMONITOR/UMWAIT on the value's cacheline when WAITPKG
 *      exists (same policy as Brook/Touch: C0.2, capped TSC deadline so
 *      a wake missed in the check→UMONITOR window drains within
 *      bounded time), PAUSE-budget + yield() otherwise.
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

void __boxcxx_atomic_wait_cycle(const volatile void *addr,
                                unsigned *spin_state)
{
    // Same policy family as Brook/Touch (see brook.c wait rationale):
    // C0.2 for deeper savings; capped deadline so a store landing in the
    // caller-check → UMONITOR window costs at most one cap period.
    constexpr uint64_t kUmwaitCapMs = 50;

    if (cpu_has_waitpkg()) {
        umonitor(const_cast<volatile void *>(addr));
        uint64_t deadline_tsc = cpu_rdtsc() + cpu_ms_to_tsc(kUmwaitCapMs);
        (void)umwait(0, deadline_tsc);
        return;
    }

    constexpr unsigned kSpinBudget = 1u << 14;
    if ((*spin_state)++ < kSpinBudget) {
        __asm__ volatile("pause");
    } else {
        *spin_state = 0;
        yield();
    }
}

} // extern "C"
