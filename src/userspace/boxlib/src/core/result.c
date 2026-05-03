#include "box/core/result.h"
#include "box/core/notify.h"
#include "box/system.h"
#include "box/cpu.h"
#include "../arch/x86_64/cpu_wait.h"

bool result_available(void) {
    ResultRing* rr = result_ring();
    __sync_synchronize();
    return rr->hdr.head != rr->hdr.tail;
}

uint32_t result_count(void) {
    ResultRing* rr = result_ring();
    __sync_synchronize();
    uint64_t n = rr->hdr.tail - rr->hdr.head;
    return n > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)n;
}

bool result_pop(Result* out) {
    ResultRing* rr = result_ring();
    if (!rr || !out) return false;

    /* Force runtime loads — same rationale as pocket_ring_push: the
     * compiler treats slots_base / slot_size / slot_count_max as
     * const-after-init and was eliding the safety checks against a
     * corrupted or uninitialised header (see decks-elf RIP=0x11b8c
     * crash, 2026-04-29). */
    uint32_t cap     = __atomic_load_n(&rr->hdr.slot_count_max, __ATOMIC_RELAXED);
    uint64_t base    = __atomic_load_n(&rr->hdr.slots_base,     __ATOMIC_RELAXED);
    uint32_t stride  = __atomic_load_n(&rr->hdr.slot_size,      __ATOMIC_RELAXED);
    if (cap == 0 || base == 0 || stride == 0)        return false;
    if (base < 0x100000000ULL)                       return false;
    /* ABI guard: the kernel must publish slots in 32-byte ResultSlot units. */
    if (stride != sizeof(ResultSlot))                return false;

    /* Cheap drain check — if no producer has reserved beyond our cursor,
     * no work to do. Without this the seq read below would still correctly
     * report "not ready", but the empty-ring case is hot and skipping the
     * slot translate pays for the extra atomic load. ACQUIRE on tail
     * pairs with the kernel's ACQ_REL fetch_add so we never see a stale
     * seq from a slot the producer is about to write. */
    uint64_t tail = __atomic_load_n(&rr->hdr.tail, __ATOMIC_ACQUIRE);
    uint64_t pos  = __atomic_load_n(&rr->hdr.head, __ATOMIC_RELAXED);
    if (pos == tail) return false;

    /* Vyukov consumer: slot is ready for our round when seq == 2*round + 1.
     * If the producer holding `pos` hasn't released its seq yet, slot.seq
     * is still 2*round (the prior round's "free" marker) — we treat it as
     * empty and the caller will retry on its next poll. */
    ResultSlot *slot   = (ResultSlot *)(uintptr_t)(base + (pos % cap) * stride);
    uint64_t expected  = 2u * (pos / (uint64_t)cap) + 1u;
    uint64_t seq       = __atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE);
    if (seq != expected) return false;

    *out = slot->r;

    /* Release the slot for the producer's next round at this index.
     * Producer for round R+1 expects seq == 2*(R+1) before it may write. */
    __atomic_store_n(&slot->seq, expected + 1u, __ATOMIC_RELEASE);
    /* Advance head — kernel's ACQUIRE-load of head will pair with this
     * RELEASE so the released seq is visible before fullness probes. */
    __atomic_store_n(&rr->hdr.head, pos + 1u, __ATOMIC_RELEASE);
    return true;
}

#define IPC_STASH_SIZE 64
static Result ipc_stash_buf[IPC_STASH_SIZE];
static uint32_t ipc_stash_cnt = 0;

static void ipc_stash_push(Result* entry) {
    if (ipc_stash_cnt >= IPC_STASH_SIZE) {
        /* Stash full: drop oldest to make room */
        for (uint32_t i = 1; i < ipc_stash_cnt; i++) {
            ipc_stash_buf[i - 1] = ipc_stash_buf[i];
        }
        ipc_stash_cnt--;
    }
    ipc_stash_buf[ipc_stash_cnt++] = *entry;
}

static bool ipc_stash_shift(Result* out) {
    if (ipc_stash_cnt == 0) return false;
    *out = ipc_stash_buf[0];
    for (uint32_t i = 1; i < ipc_stash_cnt; i++) {
        ipc_stash_buf[i - 1] = ipc_stash_buf[i];
    }
    ipc_stash_cnt--;
    return true;
}

#define NON_IPC_STASH_SIZE 64
static Result non_ipc_stash_buf[NON_IPC_STASH_SIZE];
static uint32_t non_ipc_stash_cnt = 0;

static void non_ipc_stash_push(Result* entry) {
    if (non_ipc_stash_cnt >= NON_IPC_STASH_SIZE) {
        for (uint32_t i = 1; i < non_ipc_stash_cnt; i++) {
            non_ipc_stash_buf[i - 1] = non_ipc_stash_buf[i];
        }
        non_ipc_stash_cnt--;
    }
    non_ipc_stash_buf[non_ipc_stash_cnt++] = *entry;
}

static bool non_ipc_stash_shift(Result* out) {
    if (non_ipc_stash_cnt == 0) return false;
    *out = non_ipc_stash_buf[0];
    for (uint32_t i = 1; i < non_ipc_stash_cnt; i++) {
        non_ipc_stash_buf[i - 1] = non_ipc_stash_buf[i];
    }
    non_ipc_stash_cnt--;
    return true;
}

bool result_pop_non_ipc(Result* out) {
    if (!out) return false;
    if (non_ipc_stash_shift(out)) return true;

    Result entry;
    while (result_pop(&entry)) {
        if (entry.sender_pid != 0) {
            ipc_stash_push(&entry);
            continue;
        }
        *out = entry;
        return true;
    }
    return false;
}

bool result_pop_ipc(Result* out) {
    if (!out) return false;
    if (ipc_stash_shift(out)) return true;

    Result entry;
    while (result_pop(&entry)) {
        if (entry.sender_pid != 0) {
            *out = entry;
            return true;
        }
        non_ipc_stash_push(&entry);
    }
    return false;
}

uint32_t result_ipc_stash_count(void) {
    return ipc_stash_cnt;
}

/* ===========================================================================
 * Blocking waits — was result_wait.c (merged Stage 2).
 * UMWAIT-based fast path on CPUs with WAITPKG; pause-spin fallback otherwise.
 * =========================================================================== */
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static bool result_wait_umwait(Result* out, uint32_t timeout_ms) {
    ResultRing* rr = result_ring();
    volatile uint32_t* tail_addr;
    { char* base = (char*)rr; tail_addr = (volatile uint32_t*)(base + 4); }

    while (1) {
        __sync_synchronize();

        if (result_available()) {
            if (result_pop_non_ipc(out)) return true;
        }

        umonitor((volatile void*)tail_addr);
        __sync_synchronize();

        if (!result_available()) {
            uint64_t deadline_tsc;
            if (timeout_ms == 0) {
                deadline_tsc = 0xFFFFFFFFFFFFFFFFULL;
            } else {
                uint64_t tsc_now   = rdtsc();
                uint64_t tsc_delta = cpu_ms_to_tsc(timeout_ms);
                deadline_tsc = tsc_now + tsc_delta;
            }

            int wake_reason = umwait(0, deadline_tsc);

            if (wake_reason == 1 && timeout_ms > 0) {
                __sync_synchronize();
                if (!result_available()) return false;
            }
        }
    }
}

static bool result_wait_yield(Result* out, uint32_t timeout_ms) {
    uint64_t deadline = 0;
    if (timeout_ms > 0) {
        deadline = rdtsc() + cpu_ms_to_tsc(timeout_ms);
    }

    while (1) {
        __sync_synchronize();

        if (result_available()) {
            if (result_pop_non_ipc(out)) return true;
        }

        if (timeout_ms > 0 && rdtsc() >= deadline) return false;

        /* Pause keeps multi-core friendly: K-Core writes Result directly;
         * we just spin until it appears. No kernel re-entry needed. */
        __asm__ volatile("pause");
    }
}

bool result_wait(Result* out, uint32_t timeout_ms) {
    if (!out) return false;
    if (result_pop_non_ipc(out)) return true;

    if (cpu_has_waitpkg()) return result_wait_umwait(out, timeout_ms);
    return result_wait_yield(out, timeout_ms);
}

/* Block until ANY result arrives — no IPC/non-IPC filtering.
 * For IPC servers (display daemon) that receive both kernel results
 * (from their own VGA/keyboard calls) and IPC messages.
 * Drains both stashes first, then pops from the ring. */
bool result_wait_any(Result* out, uint32_t timeout_ms) {
    if (!out) return false;
    if (result_pop_ipc(out))     return true;
    if (result_pop_non_ipc(out)) return true;
    if (result_pop(out))         return true;

    uint64_t deadline = 0;
    if (timeout_ms > 0) {
        deadline = rdtsc() + cpu_ms_to_tsc(timeout_ms);
    }

    while (1) {
        __sync_synchronize();

        if (result_available()) {
            if (result_pop(out)) return true;
        }

        if (timeout_ms > 0 && rdtsc() >= deadline) return false;

        __asm__ volatile("pause");
    }
}
