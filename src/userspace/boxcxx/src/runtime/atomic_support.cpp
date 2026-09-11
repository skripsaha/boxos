
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

}


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
                                  u128 desired, bool , int, int)
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

}


namespace {

constexpr unsigned kLockPoolSize = 64;

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

constexpr unsigned kWaitPoolCount = 256;
struct alignas(64) WaitPool {
    uint64_t ver;
    uint32_t waiters;
};
WaitPool g_wait_pools[kWaitPoolCount];

inline WaitPool *WaitPoolFor(const volatile void *addr)
{
    uintptr_t a = reinterpret_cast<uintptr_t>(addr);
    return &g_wait_pools[(a >> 4) & (kWaitPoolCount - 1)];
}

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

}

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


void __boxcxx_atomic_wait_cycle(const volatile void *addr, const void *observed,
                                unsigned width, unsigned long long mask,
                                unsigned *spin_state)
{
    if (cpu_has_waitpkg()) {
        if (*spin_state < 2u) {
            (*spin_state)++;
            umonitor(const_cast<volatile void *>(addr));
            uint64_t deadline = cpu_rdtsc() + cpu_ms_to_tsc(50);
            (void)umwait(0, deadline);
            return;
        }
    } else {
        constexpr unsigned kSpinBudget = 1u << 14;
        if ((*spin_state)++ < kSpinBudget) {
            __asm__ volatile("pause");
            return;
        }
    }

    *spin_state = 0;
    WaitPool *p = WaitPoolFor(addr);
    __atomic_fetch_add(&p->waiters, 1u, __ATOMIC_SEQ_CST);
    uint64_t v = __atomic_load_n(&p->ver, __ATOMIC_SEQ_CST);
    if (WaitWordChanged(addr, observed, width, mask)) {
        __atomic_fetch_sub(&p->waiters, 1u, __ATOMIC_SEQ_CST);
        return;
    }
    addr_park(&p->ver, v, 0);
    __atomic_fetch_sub(&p->waiters, 1u, __ATOMIC_SEQ_CST);
}

bool __boxcxx_atomic_wait_until(const volatile void *addr, const void *observed,
                                unsigned width, unsigned long long mask,
                                unsigned timeout_ms, unsigned *spin_state)
{
    if (timeout_ms == 0) return true;

    if (cpu_has_waitpkg()) {
        if (*spin_state < 2u) {
            (*spin_state)++;
            umonitor(const_cast<volatile void *>(addr));
            unsigned spin_ms  = timeout_ms < 50u ? timeout_ms : 50u;
            uint64_t deadline = cpu_rdtsc() + cpu_ms_to_tsc(spin_ms);
            (void)umwait(0, deadline);
            return false;
        }
    } else {
        constexpr unsigned kSpinBudget = 1u << 14;
        if ((*spin_state)++ < kSpinBudget) {
            __asm__ volatile("pause");
            return false;
        }
    }

    *spin_state = 0;
    WaitPool *p = WaitPoolFor(addr);
    __atomic_fetch_add(&p->waiters, 1u, __ATOMIC_SEQ_CST);
    uint64_t v = __atomic_load_n(&p->ver, __ATOMIC_SEQ_CST);
    if (WaitWordChanged(addr, observed, width, mask)) {
        __atomic_fetch_sub(&p->waiters, 1u, __ATOMIC_SEQ_CST);
        return false;
    }
    error_t rc = addr_park(&p->ver, v, timeout_ms);
    __atomic_fetch_sub(&p->waiters, 1u, __ATOMIC_SEQ_CST);
    return rc == ERR_TIMEOUT;
}

void __boxcxx_atomic_notify(const volatile void *addr, bool all)
{
    (void)all;
    WaitPool *p = WaitPoolFor(addr);
    __atomic_fetch_add(&p->ver, 1u, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&p->waiters, __ATOMIC_SEQ_CST) != 0)
        addr_wake(&p->ver, 0);
}

}