#include "box/core/result.h"
#include "box/core/notify.h"
#include "box/system.h"
#include "box/cpu.h"

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

/* Diagnostic counters — see result_pop_stats(). */
static volatile uint32_t g_rp_calls;
static volatile uint32_t g_rp_empty;       /* head == tail */
static volatile uint32_t g_rp_seq_mismatch; /* head != tail but slot.seq != expected */
static volatile uint32_t g_rp_success;
static volatile uint64_t g_rp_last_seq_seen;
static volatile uint64_t g_rp_last_expected;
static volatile uint64_t g_rp_last_pos;
static volatile uint64_t g_rp_last_tail;

void result_pop_stats(uint64_t out[8]) {
    out[0] = __atomic_load_n(&g_rp_calls,         __ATOMIC_RELAXED);
    out[1] = __atomic_load_n(&g_rp_empty,         __ATOMIC_RELAXED);
    out[2] = __atomic_load_n(&g_rp_seq_mismatch,  __ATOMIC_RELAXED);
    out[3] = __atomic_load_n(&g_rp_success,       __ATOMIC_RELAXED);
    out[4] = __atomic_load_n(&g_rp_last_seq_seen, __ATOMIC_RELAXED);
    out[5] = __atomic_load_n(&g_rp_last_expected, __ATOMIC_RELAXED);
    out[6] = __atomic_load_n(&g_rp_last_pos,      __ATOMIC_RELAXED);
    out[7] = __atomic_load_n(&g_rp_last_tail,     __ATOMIC_RELAXED);
}

bool result_pop(Result* out) {
    ResultRing* rr = result_ring();
    if (!rr || !out) return false;
    __atomic_add_fetch(&g_rp_calls, 1, __ATOMIC_RELAXED);

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
    if (pos == tail) {
        __atomic_add_fetch(&g_rp_empty, 1, __ATOMIC_RELAXED);
        return false;
    }

    /* Vyukov consumer: slot is ready for our round when seq == 2*round + 1.
     * If the producer holding `pos` hasn't released its seq yet, slot.seq
     * is still 2*round (the prior round's "free" marker) — we treat it as
     * empty and the caller will retry on its next poll. */
    ResultSlot *slot   = (ResultSlot *)(uintptr_t)(base + (pos % cap) * stride);
    uint64_t expected  = 2u * (pos / (uint64_t)cap) + 1u;
    uint64_t seq       = __atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE);
    if (seq != expected) {
        __atomic_store_n(&g_rp_last_seq_seen, seq, __ATOMIC_RELAXED);
        __atomic_store_n(&g_rp_last_expected, expected, __ATOMIC_RELAXED);
        __atomic_store_n(&g_rp_last_pos, pos, __ATOMIC_RELAXED);
        __atomic_store_n(&g_rp_last_tail, tail, __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_rp_seq_mismatch, 1, __ATOMIC_RELAXED);
        return false;
    }
    __atomic_add_fetch(&g_rp_success, 1, __ATOMIC_RELAXED);

    *out = slot->r;

    /* Release the slot for the producer's next round at this index.
     * Producer for round R+1 expects seq == 2*(R+1) before it may write. */
    __atomic_store_n(&slot->seq, expected + 1u, __ATOMIC_RELEASE);
    /* Advance head — kernel's ACQUIRE-load of head will pair with this
     * RELEASE so the released seq is visible before fullness probes. */
    __atomic_store_n(&rr->hdr.head, pos + 1u, __ATOMIC_RELEASE);
    return true;
}

/* Stash is a static-sized circular FIFO. The original 64-slot cap dropped
 * touches under multi-core stress (a 4000-event burst overflowed and lost
 * events that never made it back to the consumer). 8192 covers any realistic
 * burst without dynamic allocation, and drop-oldest semantics ensure stale
 * entries from prior calls don't accumulate to corrupt future replies. */
#define STASH_CAP 8192

typedef struct {
    Result   buf[STASH_CAP];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
} StashRing;

static StashRing ipc_stash;
static StashRing non_ipc_stash;

static void stash_push(StashRing *s, const Result *entry) {
    if (s->count >= STASH_CAP) {
        /* Drop oldest to make room. Preserves FIFO order; loses the oldest
         * unconsumed entry, which is preferable to dropping the new one
         * that may be the actual reply the caller is waiting for. */
        s->head = (s->head + 1) % STASH_CAP;
        s->count--;
    }
    s->buf[s->tail] = *entry;
    s->tail = (s->tail + 1) % STASH_CAP;
    s->count++;
}

static bool stash_shift(StashRing *s, Result *out) {
    if (s->count == 0) return false;
    *out = s->buf[s->head];
    s->head = (s->head + 1) % STASH_CAP;
    s->count--;
    return true;
}

static void ipc_stash_push(Result* entry)        { stash_push(&ipc_stash, entry); }
static bool ipc_stash_shift(Result* out)         { return stash_shift(&ipc_stash, out); }
static void non_ipc_stash_push(Result* entry)    { stash_push(&non_ipc_stash, entry); }
static bool non_ipc_stash_shift(Result* out)     { return stash_shift(&non_ipc_stash, out); }

bool result_pop_non_ipc(Result* out) {
    if (!out) return false;
    /* Skip ERR_WOULD_BLOCK entries — these are transient kernel acks for
     * async-parking ops (touch_await, kb_readline) and never valid replies
     * to a synchronous manifest submission. If we returned them as the
     * reply for a different submission the caller would see a stale 9 and
     * misinterpret it as its own error. */
    while (non_ipc_stash_shift(out)) {
        if (out->error_code != 9 /* ERR_WOULD_BLOCK */) return true;
    }

    Result entry;
    while (result_pop(&entry)) {
        if (entry.sender_pid != 0) {
            ipc_stash_push(&entry);
            continue;
        }
        /* Post-2026-06-01 TouchRing migration: KCTX_TOUCH no longer
         * arrives via ResultRing. The dispatch below is a defensive
         * fallback that should never fire on a synchronised kernel +
         * boxlib build — but we keep the filter in case a future
         * kernel publisher (or a stale third-party tool) still publishes
         * a Touch through this channel. The entry is discarded; real
         * Touches are delivered via touch_pop / touch_wait. */
        if (entry.context == KCTX_TOUCH) {
            continue;
        }
        if (entry.error_code == 9 /* ERR_WOULD_BLOCK */) continue;
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
        /* Sender_pid != 0 → IPC payload (route from another process). */
        if (entry.sender_pid != 0) {
            *out = entry;
            return true;
        }
        /* Post-2026-06-01: Touch events come via TouchRing, not
         * ResultRing. The defensive filter here drops any leftover
         * KCTX_TOUCH-tagged entry that might still arrive from a
         * mismatched kernel/boxlib build. */
        if (entry.context == KCTX_TOUCH) {
            continue;
        }
        /* Skip transient async-park acks (see result_pop_non_ipc). */
        if (entry.error_code == 9 /* ERR_WOULD_BLOCK */) continue;
        non_ipc_stash_push(&entry);
    }
    return false;
}

uint32_t result_ipc_stash_count(void) {
    return ipc_stash.count;
}

uint32_t result_non_ipc_stash_count(void) {
    return non_ipc_stash.count;
}

/* result_pop_touch removed 2026-06-01: Touch events migrated to a
 * dedicated per-cabin TouchRing. Use touch_pop / touch_wait from
 * box/touch.h instead. The function name lives on in the declaration
 * for one release as a stub so build-time linker errors guide
 * out-of-tree consumers to the new API. */
bool result_pop_touch(Result* out) {
    (void)out;
    return false;
}

/* Drop orphan manifest replies — replies whose original submitter timed out
 * and is no longer waiting for them. Without this, the very next MfCall1's
 * result_wait would dequeue the orphan and treat it as ITS reply, leaking
 * the prior call's error_code (e.g. 302 / 902) into a brand-new operation.
 *
 * Filter rules (must mirror result_pop_non_ipc):
 *   - sender_pid != 0   → IPC / touch event (preserve in ipc_stash)
 *   - context==KCTX_TOUCH → LEVEL touch with kernel source_pid=0 (preserve)
 *   - error_code==9 (WOULD_BLOCK) → async-park ack (discard, not a real reply)
 *   - everything else   → orphan manifest reply (DISCARD)
 *
 * Safe to call ONLY before submitting a fresh synchronous Manifest. Async
 * paths (touch_await, kb_readline) must NOT use this helper because their
 * pending replies look identical to orphans. */
void result_drain_orphan_replies(void) {
    /* Drop everything sitting in the local non_ipc_stash too — those are by
     * definition entries the caller skipped past. */
    Result discard;
    while (non_ipc_stash_shift(&discard)) { /* drop */ }

    Result entry;
    while (result_pop(&entry)) {
        if (entry.sender_pid != 0) {
            ipc_stash_push(&entry);
            continue;
        }
        /* Post-2026-06-01: KCTX_TOUCH no longer arrives here. Any
         * leftover entry tagged that way is treated as an orphan and
         * discarded along with regular manifest-reply orphans. */
        /* Manifest-reply slot with sender_pid==0 — orphan. Drop. */
    }
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
    /* Per Intel SDM Vol 2B, UMONITOR arms a hardware monitor over the
     * cache line containing the supplied address (size from CPUID.05H:
     * EAX[15:0], typically 64B). We want to wake when the producer
     * (kernel KResultPush) advances `tail` — so monitor exactly that
     * field, not the head/tail header at offset+4 (which used to work
     * by 64B-cache-line accident: head, tail and the rest of the
     * 64-byte ResultRingHeader all share one line).
     *
     * ResultRingHeader is __packed, so taking &rr->hdr.tail directly
     * would draw -Waddress-of-packed-member. Computing the address via
     * offsetof yields the same value, the page-aligned base + 8-byte
     * offset is naturally 8-aligned, and the warning is silenced
     * legitimately rather than via diagnostic-pragma noise. */
    volatile uint64_t *tail_addr = (volatile uint64_t *)
        ((uintptr_t)rr + OFFSETOF(ResultRing, hdr.tail));

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
    /* Discard stale stash entries — manifest_submit always wants a FRESH
     * reply for THIS submission, never an old stash entry. Stale-reply
     * mismatch (got reply for op N when waiting for op N+1) breaks
     * touch_claim/release sequencing on multi-core stress. */
    {
        Result discard;
        while (non_ipc_stash_shift(&discard)) { /* drain */ }
    }
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

