#ifndef ATOMICS_H
#define ATOMICS_H

#include "ktypes.h"

// Atomic type aliases for clearer semantics
typedef volatile uint32_t atomic_u32_t;
typedef volatile uint64_t atomic_u64_t;
typedef volatile uint8_t atomic_u8_t;

// Initialization helpers
#define atomic_init_u32(ptr, val) atomic_store_u32(ptr, val)
#define atomic_init_u64(ptr, val) atomic_store_u64(ptr, val)
#define atomic_init_u8(ptr, val) atomic_store_u8(ptr, val)

#define COMPILER_BARRIER() __asm__ __volatile__("" ::: "memory")

// CRITICAL FIX: Use GCC atomic builtins for true atomic 64-bit operations
static inline uint64_t atomic_load_u64(const volatile uint64_t* ptr) {
    return __atomic_load_n((uint64_t*)ptr, __ATOMIC_SEQ_CST);
}

static inline void atomic_store_u64(volatile uint64_t* ptr, uint64_t val) {
    __atomic_store_n((uint64_t*)ptr, val, __ATOMIC_SEQ_CST);
}

static inline uint32_t atomic_load_u32(const volatile uint32_t* ptr) {
    return __atomic_load_n((uint32_t*)ptr, __ATOMIC_SEQ_CST);
}

static inline void atomic_store_u32(volatile uint32_t* ptr, uint32_t val) {
    __atomic_store_n((uint32_t*)ptr, val, __ATOMIC_SEQ_CST);
}

static inline bool atomic_cas_u64(volatile uint64_t* ptr, uint64_t expected, uint64_t desired) {
    uint64_t prev;
    __asm__ __volatile__(
        "lock cmpxchgq %2, %1"
        : "=a"(prev), "+m"(*ptr)
        : "r"(desired), "0"(expected)
        : "memory"
    );
    return prev == expected;
}

static inline bool atomic_cas_u32(volatile uint32_t* ptr, uint32_t expected, uint32_t desired) {
    uint32_t prev;
    __asm__ __volatile__(
        "lock cmpxchgl %2, %1"
        : "=a"(prev), "+m"(*ptr)
        : "r"(desired), "0"(expected)
        : "memory"
    );
    return prev == expected;
}

static inline uint64_t atomic_fetch_add_u64(volatile uint64_t* ptr, uint64_t val) {
    uint64_t result;
    __asm__ __volatile__(
        "lock xaddq %0, %1"
        : "=r"(result), "+m"(*ptr)
        : "0"(val)
        : "memory"
    );
    return result;
}

static inline uint32_t atomic_fetch_add_u32(volatile uint32_t* ptr, uint32_t val) {
    uint32_t result;
    __asm__ __volatile__(
        "lock xaddl %0, %1"
        : "=r"(result), "+m"(*ptr)
        : "0"(val)
        : "memory"
    );
    return result;
}

static inline uint32_t atomic_fetch_sub_u32(volatile uint32_t* ptr, uint32_t val) {
    return atomic_fetch_add_u32(ptr, ~val + 1);
}

static inline uint64_t atomic_fetch_sub_u64(volatile uint64_t* ptr, uint64_t val) {
    return atomic_fetch_add_u64(ptr, ~val + 1);
}

static inline uint8_t atomic_fetch_add_u8(volatile uint8_t* ptr, uint8_t val) {
    uint8_t result;
    __asm__ __volatile__(
        "lock xaddb %0, %1"
        : "=r"(result), "+m"(*ptr)
        : "0"(val)
        : "memory"
    );
    return result;
}

static inline uint8_t atomic_fetch_sub_u8(volatile uint8_t* ptr, uint8_t val) {
    return atomic_fetch_add_u8(ptr, (uint8_t)(~val + 1));
}

static inline void cpu_pause(void) {
    __asm__ __volatile__("pause" ::: "memory");
}

static inline void mfence(void) {
    __asm__ __volatile__("mfence" ::: "memory");
}

static inline void lfence(void) {
    __asm__ __volatile__("lfence" ::: "memory");
}

static inline void sfence(void) {
    __asm__ __volatile__("sfence" ::: "memory");
}

static inline uint64_t rdtsc(void) {
    uint32_t low, high;
    __asm__ __volatile__("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32) | low;
}

// CRITICAL FIX: Use GCC atomic builtins for true atomic 8-bit operations
static inline void atomic_store_u8(volatile uint8_t* ptr, uint8_t val) {
    __atomic_store_n((uint8_t*)ptr, val, __ATOMIC_SEQ_CST);
}

static inline uint8_t atomic_load_u8(const volatile uint8_t* ptr) {
    return __atomic_load_n((uint8_t*)ptr, __ATOMIC_SEQ_CST);
}

#endif // ATOMICS_H
