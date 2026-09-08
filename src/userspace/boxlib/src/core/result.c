#include "box/core/result.h"
#include "box/core/notify.h"
#include "box/system.h"
#include "box/cpu.h"
#include "box/memory.h"   /* malloc / free — per-strand ferry stash heap backing (Ф26e) */
#include "box/debug.h"    /* kdbg_print — a deadline-free wait must be able to accuse */
#include "box/turnin.h"   /* box_turn_in — a deadline-free wait must also be able to SLEEP */

bool result_available(void) {
    ResultRing* rr = result_ring();
    __sync_synchronize();
    return rr->hdr.head != rr->hdr.tail;
}

/* True when the slot at the ring's head is published and unconsumed — a record
 * this strand can pop right now. result_available() only says a producer has
 * CLAIMED past head; the claim is released later, and a sleep that ends on the
 * claim can be re-armed before the release and never look again. This is the
 * fact box_turn_in ends on. Same header sanity as result_pop, no side effects. */
bool result_published_at_head(void) {
    ResultRing* rr = result_ring();
    if (!rr) return false;
    uint32_t cap    = __atomic_load_n(&rr->hdr.slot_count_max, __ATOMIC_RELAXED);
    uint64_t base   = __atomic_load_n(&rr->hdr.slots_base,     __ATOMIC_RELAXED);
    uint32_t stride = __atomic_load_n(&rr->hdr.slot_size,      __ATOMIC_RELAXED);
    if (cap == 0 || base == 0 || stride == 0)        return false;
    if (base < 0x100000000ULL)                       return false;
    if (stride != sizeof(ResultSlot))                return false;

    uint64_t tail = __atomic_load_n(&rr->hdr.tail, __ATOMIC_ACQUIRE);
    uint64_t pos  = __atomic_load_n(&rr->hdr.head, __ATOMIC_RELAXED);
    if (pos == tail) return false;

    ResultSlot *slot  = (ResultSlot *)(uintptr_t)(base + (pos % cap) * stride);
    uint64_t expected = 2u * (pos / (uint64_t)cap) + 1u;
    return __atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE) == expected;
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

/* Result stashes are per-STRAND, not per-process (P5a). Each strand consumes
 * only its OWN ResultRing, so its ipc/non-ipc stashes must be private too —
 * sharing one process-wide stash across concurrent strands would interleave
 * and misroute replies (the same data race the per-strand rings fix).
 *
 *   - Main strand: a large static FIFO (8192 entries). The original 64-slot
 *     cap dropped events under multi-core stress; 8192 covers any realistic
 *     burst for the cabin's primary thread.
 *   - Spawned strand: a 256-entry FIFO overlaid on its StrandInfo TLS block
 *     (kernel zero-init'd). Smaller because a spawned strand makes near-serial
 *     syscalls; drop-oldest semantics keep it self-healing under overflow.
 *
 * stash_view_t abstracts over the two backings so the push/shift logic is
 * written once. Both are single-threaded per strand (a strand drains its own
 * ring on its own core), so no locking is needed. */
#define STASH_CAP 8192

typedef struct {
    Result   buf[STASH_CAP];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
} StashRing;

static StashRing ipc_stash;
static StashRing non_ipc_stash;

/* Per-strand stash overlaid on StrandInfo.{ipc,non_ipc}_stash bytes. */
typedef struct {
    Result   buf[STRAND_STASH_CAP];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    uint32_t _pad;
} StrandStashRing;

STATIC_ASSERT(sizeof(Result) == STRAND_STASH_ENTRY_SZ,
              "Result stash entry size must match strand_info.h ABI");
STATIC_ASSERT(sizeof(StrandStashRing) <= STRAND_STASH_BYTES,
              "StrandStashRing must fit the StrandInfo stash reservation");

/* Mutable view of whichever stash backs the calling strand. */
typedef struct {
    Result   *buf;
    uint32_t  cap;
    uint32_t *head;
    uint32_t *tail;
    uint32_t *count;
} stash_view_t;

static stash_view_t ipc_stash_view(void) {
    StrandInfo *si = strand_info_or_null();
    if (si) {
        StrandStashRing *r = (StrandStashRing *)si->ipc_stash;
        stash_view_t v = { r->buf, STRAND_STASH_CAP, &r->head, &r->tail, &r->count };
        return v;
    }
    stash_view_t v = { ipc_stash.buf, STASH_CAP, &ipc_stash.head, &ipc_stash.tail, &ipc_stash.count };
    return v;
}

static stash_view_t non_ipc_stash_view(void) {
    StrandInfo *si = strand_info_or_null();
    if (si) {
        StrandStashRing *r = (StrandStashRing *)si->non_ipc_stash;
        stash_view_t v = { r->buf, STRAND_STASH_CAP, &r->head, &r->tail, &r->count };
        return v;
    }
    stash_view_t v = { non_ipc_stash.buf, STASH_CAP,
                       &non_ipc_stash.head, &non_ipc_stash.tail, &non_ipc_stash.count };
    return v;
}

static void stash_push_view(stash_view_t v, const Result *entry) {
    if (*v.count >= v.cap) {
        /* Drop oldest to make room. Preserves FIFO order; loses the oldest
         * unconsumed entry, which is preferable to dropping the new one
         * that may be the actual reply the caller is waiting for. */
        *v.head = (*v.head + 1) % v.cap;
        (*v.count)--;
    }
    v.buf[*v.tail] = *entry;
    *v.tail = (*v.tail + 1) % v.cap;
    (*v.count)++;
}

static bool stash_shift_view(stash_view_t v, Result *out) {
    if (*v.count == 0) return false;
    *out = v.buf[*v.head];
    *v.head = (*v.head + 1) % v.cap;
    (*v.count)--;
    return true;
}

static void ipc_stash_push(Result* entry)        { stash_push_view(ipc_stash_view(), entry); }
static bool ipc_stash_shift(Result* out)         { return stash_shift_view(ipc_stash_view(), out); }
static void non_ipc_stash_push(Result* entry)    { stash_push_view(non_ipc_stash_view(), entry); }
static bool non_ipc_stash_shift(Result* out)     { return stash_shift_view(non_ipc_stash_view(), out); }

/* Ф26e — ferry (async file I/O, box::ferry) completion stash. FULLY ISOLATED:
 * a KCTX_STORAGE record is routed here by every other ResultRing consumer
 * (result_pop_non_ipc / result_pop_ipc / result_wait_any / orphan-drain) and
 * handed out ONLY by result_pop_ferry, so no synchronous fread, result_any, or
 * IPC receive can ever steal a ferry completion. Main strand → a large static
 * ring; spawned strand → a heap ring lazily malloc'd and cached in
 * StrandInfo.ferry_stash_ptr (mirrors touch_stash_ptr — opt-in, so a strand
 * that never issues a ferry op pays nothing). */
static StashRing ferry_stash;

static stash_view_t ferry_stash_view(void) {
    StrandInfo *si = strand_info_or_null();
    if (si) {
        StrandStashRing *r = (StrandStashRing *)(uintptr_t)si->ferry_stash_ptr;
        if (!r) {
            r = (StrandStashRing *)malloc(sizeof(StrandStashRing));
            if (r) {
                r->head = r->tail = r->count = 0;
                si->ferry_stash_ptr = (uint64_t)(uintptr_t)r;
            }
        }
        if (r) {
            stash_view_t v = { r->buf, STRAND_STASH_CAP, &r->head, &r->tail, &r->count };
            return v;
        }
        /* malloc failed (extreme OOM on a spawned strand) — a NULL view makes
         * push/shift no-ops; the completion is dropped like any stash overflow. */
        stash_view_t v = { NULL, 0, NULL, NULL, NULL };
        return v;
    }
    stash_view_t v = { ferry_stash.buf, STASH_CAP,
                       &ferry_stash.head, &ferry_stash.tail, &ferry_stash.count };
    return v;
}

static void ferry_stash_push(Result* entry) {
    stash_view_t v = ferry_stash_view();
    if (!v.buf) return;                       /* spawned-strand OOM — self-healing drop */
    stash_push_view(v, entry);
}
static bool ferry_stash_shift(Result* out) {
    stash_view_t v = ferry_stash_view();
    if (!v.buf) return false;
    return stash_shift_view(v, out);
}

/* Non-allocating count probe for the ferry blocking-wait loop. */
uint32_t result_ferry_stash_count(void) {
    StrandInfo *si = strand_info_or_null();
    if (si) {
        StrandStashRing *r = (StrandStashRing *)(uintptr_t)si->ferry_stash_ptr;
        return r ? r->count : 0;
    }
    return ferry_stash.count;
}

bool result_pop_non_ipc(Result* out) {
    if (!out) return false;
    /* Skip ERR_WOULD_BLOCK entries — these are transient kernel acks for
     * async-parking ops (touch_await, readline) and never valid replies
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
        /* Ф26e: a ferry (KCTX_STORAGE) completion is NEVER a synchronous reply.
         * Route it to the isolated ferry stash so this sync/result_any consumer
         * can never steal it, then keep scanning for our real reply. */
        if (KCTX_KIND(entry.context) == KCTX_STORAGE) {
            ferry_stash_push(&entry);
            continue;
        }
        /* Post-2026-06-01 TouchRing migration: KCTX_TOUCH no longer
         * arrives via ResultRing. The dispatch below is a defensive
         * fallback that should never fire on a synchronised kernel +
         * boxlib build — but we keep the filter in case a future
         * kernel publisher (or a stale third-party tool) still publishes
         * a Touch through this channel. The entry is discarded; real
         * Touches are delivered via touch_pop / touch_wait. */
        if (KCTX_KIND(entry.context) == KCTX_TOUCH) {
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
        /* Ф26e: isolate ferry completions (see result_pop_non_ipc). */
        if (KCTX_KIND(entry.context) == KCTX_STORAGE) {
            ferry_stash_push(&entry);
            continue;
        }
        /* Post-2026-06-01: Touch events come via TouchRing, not
         * ResultRing. The defensive filter here drops any leftover
         * KCTX_TOUCH-tagged entry that might still arrive from a
         * mismatched kernel/boxlib build. */
        if (KCTX_KIND(entry.context) == KCTX_TOUCH) {
            continue;
        }
        /* Skip transient async-park acks (see result_pop_non_ipc). */
        if (entry.error_code == 9 /* ERR_WOULD_BLOCK */) continue;
        non_ipc_stash_push(&entry);
    }
    return false;
}

/* Ф26e — result_pop_ferry: the box::ferry station's non-blocking drain.
 * Mirror of result_pop_ipc, storage axis. Returns the next KCTX_STORAGE
 * completion from the ferry stash or the ring; routes every non-ferry record
 * it passes to that record's own consumer (IPC → ipc_stash, plain kernel reply
 * → non_ipc_stash) so nothing a sibling awaiter is owed is ever stranded. */
bool result_pop_ferry(Result* out) {
    if (!out) return false;
    if (ferry_stash_shift(out)) return true;

    Result entry;
    while (result_pop(&entry)) {
        if (KCTX_KIND(entry.context) == KCTX_STORAGE) {     /* our ferry completion */
            *out = entry;
            return true;
        }
        if (entry.sender_pid != 0) {             /* IPC → receiver */
            ipc_stash_push(&entry);
            continue;
        }
        if (KCTX_KIND(entry.context) == KCTX_TOUCH) continue;
        if (entry.error_code == 9 /* ERR_WOULD_BLOCK */) continue;
        non_ipc_stash_push(&entry);              /* KCTX_GUIDE → sync / result_any */
    }
    return false;
}

/* Ф26e — result_restash: route ONE non-ferry record back to the stash its real
 * consumer drains. Used by result_wait_ferry after its consume-any block pops a
 * record the ferry station does not own. */
void result_restash(const Result* r) {
    if (!r) return;
    Result e = *r;
    if (KCTX_KIND(e.context) == KCTX_STORAGE) { ferry_stash_push(&e); return; }   /* defensive */
    if (KCTX_KIND(e.context) == KCTX_TOUCH)   return;                             /* migrated out — drop */
    if (e.error_code == 9 /* ERR_WOULD_BLOCK */) return;              /* async-park ack — drop */
    if (e.sender_pid != 0) { ipc_stash_push(&e); return; }            /* IPC → receiver */
    non_ipc_stash_push(&e);                                            /* plain kernel reply */
}

uint32_t result_ipc_stash_count(void) {
    return *ipc_stash_view().count;
}

uint32_t result_non_ipc_stash_count(void) {
    return *non_ipc_stash_view().count;
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
 * paths (touch_await, readline) must NOT use this helper because their
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
        /* Ф26e: a ferry completion is NOT an orphan — it belongs to a still-live
         * box::ferry awaiter. Preserve it in the isolated ferry stash instead of
         * dropping it (this drain runs first in every synchronous submit). */
        if (KCTX_KIND(entry.context) == KCTX_STORAGE) {
            ferry_stash_push(&entry);
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

static bool result_wait_raw(Result* out, uint32_t timeout_ms) {
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

/* Replies dropped because their cloakroom token answered an EARLIER,
 * abandoned submit — each one is a mis-pairing that would previously have
 * been adopted as the current call's answer. Diagnostic only. */
static uint64_t g_reply_orphans_dropped;

uint64_t result_orphans_dropped(void)
{
    return __atomic_load_n(&g_reply_orphans_dropped, __ATOMIC_RELAXED);
}

static bool result_wait_inner(Result* out, uint32_t expect_cookie, uint32_t timeout_ms) {
    if (!out) return false;

    /* The cloakroom rule: a coat is handed over by token, never "the next
     * one off the rack". Every synchronous submit stamps a 24-bit token
     * into its Pocket; the kernel echoes it in the reply's context high
     * bits; anything of a reply kind carrying a DIFFERENT token is the
     * provable orphan of an earlier call — drop it where it stands and
     * keep waiting for our own. Dropping is safe precisely because a strand
     * is inside at most ONE synchronous submit at a time: a foreign token
     * therefore belongs to a call this strand has already abandoned, never
     * to an outer frame waiting behind us. Before this, pairing was implicit
     * by ring order and one late reply shifted every later wait onto the
     * wrong answer (debug.c's S2 302/902 cascade; the 16c stack-smash
     * family). */
    uint64_t deadline_tsc = 0;
    if (timeout_ms > 0) deadline_tsc = rdtsc() + cpu_ms_to_tsc(timeout_ms);

    for (;;) {
        uint32_t remaining_ms = timeout_ms;
        if (timeout_ms > 0) {
            uint64_t now = rdtsc();
            if (now >= deadline_tsc) return false;
            remaining_ms = (uint32_t)cpu_tsc_to_ms(deadline_tsc - now);
            if (remaining_ms == 0) remaining_ms = 1;
        }
        if (!result_wait_raw(out, remaining_ms)) return false;
        if (KCTX_COOKIE24(out->context) == expect_cookie) return true;
        __atomic_add_fetch(&g_reply_orphans_dropped, 1u, __ATOMIC_RELAXED);
    }
}

/* Say what is being waited for, for as long as it is being waited for.
 *
 * The kernel cannot tell a strand doing work from a strand waiting on an
 * answer — parked or spinning, neither state says WHAT for. That gap is why a
 * wedge could hold this machine still with Nightwatch armed and never a word.
 * So the waiter publishes its own cloakroom token into the ring header it
 * already shares with the kernel, and clears it on every exit.
 *
 * The kernel reads it in Nightwatch's verdict, which walks every process — so
 * this speaks for a strand that is fast asleep just as well as for one burning
 * a core. And it is only ever a HINT: the verdict convicts on facts (the
 * pocket ring empty, no K-Core serving it, no chit left for the token by the
 * handler that put the answer off — the kernel's own half of this token, see
 * the kernel's chit.h — and the same a full look later), never on this token
 * or on a clock. It has to be that way, because a submit may be owed an answer
 * for hours and still be perfectly healthy — process.gone waits out a whole
 * child's life.
 *
 * Two stores per synchronous submit, in a wrapper rather than edits at each
 * return: this way there is no exit path that can forget, now or later. */
bool result_wait(Result* out, uint32_t expect_cookie, uint32_t timeout_ms) {
    ResultRing* rr = result_ring();
    if (rr) __atomic_store_n(&rr->hdr.awaiting, (uint64_t)expect_cookie,
                             __ATOMIC_RELEASE);
    bool ok = result_wait_inner(out, expect_cookie, timeout_ms);
    if (rr) __atomic_store_n(&rr->hdr.awaiting, 0u, __ATOMIC_RELEASE);
    return ok;
}

/* Block until ANY result arrives — no IPC/non-IPC filtering.
 * For IPC servers (display daemon) that receive both kernel results
 * (from their own VGA/keyboard calls) and IPC messages.
 * Drains both stashes first, then pops from the ring. */
bool result_wait_any(Result* out, uint32_t timeout_ms) {
    if (!out) return false;
    if (result_pop_ipc(out))     return true;
    if (result_pop_non_ipc(out)) return true;
    /* Ф26e: the bare-ring pops below can surface a ferry (KCTX_STORAGE) record —
     * route it to the isolated ferry stash and return false. The false hands
     * control back to the executor's poll sweep so the box::ferry sibling that
     * owns this completion can collect it; without it a lone storage completion
     * would never break this block and the ferry would hang. result_pop_ipc /
     * result_pop_non_ipc above already divert storage, so this is the only
     * remaining raw path. */
    {
        Result e;
        if (result_pop(&e)) {
            if (KCTX_KIND(e.context) == KCTX_STORAGE) { ferry_stash_push(&e); return false; }
            /* Records whose only job was to move the cursor — the async-park
             * ack, and Brook's bell — are not messages. Every other consumer
             * in this file has always dropped them; this raw pop is the one
             * that did not, and an IPC server handed one would read it as
             * traffic that nobody sent. */
            if (e.error_code == 9 /* ERR_WOULD_BLOCK */) return false;
            *out = e;
            return true;
        }
    }

    uint64_t deadline = 0;
    if (timeout_ms > 0) {
        deadline = rdtsc() + cpu_ms_to_tsc(timeout_ms);
    }

    while (1) {
        __sync_synchronize();

        if (result_available()) {
            Result e;
            if (result_pop(&e)) {
                if (KCTX_KIND(e.context) == KCTX_STORAGE) { ferry_stash_push(&e); return false; }
                if (e.error_code == 9 /* ERR_WOULD_BLOCK */) continue;   /* cursor only */
                *out = e;
                return true;
            }
        }

        if (timeout_ms > 0 && rdtsc() >= deadline) return false;

        __asm__ volatile("pause");
    }
}

/* IPC-filtered blocking wait. Event-driven where WAITPKG exists: UMONITOR the
 * ResultRing tail and UMWAIT until a sender's KResultPush advances it. Without
 * WAITPKG, fall back to a yield loop — a COOPERATIVE yield (not PAUSE), because
 * the message producer may be a sibling strand on the same App-Core that must
 * be scheduled for the message to ever arrive (PAUSE-spinning would livelock a
 * single-App-Core cabin). Mirrors result_wait()'s WAITPKG/fallback split. */
void yield(void);   /* boxlib (yield.c) — declared here to avoid pulling sync.h */

static bool result_wait_ipc_umwait(Result* out, uint32_t timeout_ms) {
    ResultRing* rr = result_ring();
    volatile uint64_t *tail_addr = (volatile uint64_t *)
        ((uintptr_t)rr + OFFSETOF(ResultRing, hdr.tail));
    while (1) {
        __sync_synchronize();
        if (result_available() || result_ipc_stash_count() > 0) {
            if (result_pop_ipc(out)) return true;
        }
        umonitor((volatile void*)tail_addr);
        __sync_synchronize();
        if (!result_available() && result_ipc_stash_count() == 0) {
            uint64_t deadline_tsc;
            if (timeout_ms == 0) {
                deadline_tsc = 0xFFFFFFFFFFFFFFFFULL;
            } else {
                deadline_tsc = rdtsc() + cpu_ms_to_tsc(timeout_ms);
            }
            int wake_reason = umwait(0, deadline_tsc);
            if (wake_reason == 1 && timeout_ms > 0) {
                __sync_synchronize();
                if (!result_available() && result_ipc_stash_count() == 0)
                    return false;
            }
        }
    }
}

static bool result_wait_ipc_yield(Result* out, uint32_t timeout_ms) {
    uint64_t deadline = 0;
    if (timeout_ms > 0) deadline = rdtsc() + cpu_ms_to_tsc(timeout_ms);
    while (1) {
        __sync_synchronize();
        if (result_available() || result_ipc_stash_count() > 0) {
            if (result_pop_ipc(out)) return true;
        }
        if (timeout_ms > 0 && rdtsc() >= deadline) return false;
        yield();
    }
}

/* The unbounded wait — and the only one that sleeps.
 *
 * A wait with no deadline is a wait that may last a week: the shell's readline
 * sits here from one prompt to the next keystroke. Every one of those seconds
 * used to be spent asking the kernel for its core back, over and over, a
 * hundred thousand times a second per idle strand. Turning in costs one submit
 * and gives the core away entirely.
 *
 * BOUNDED waits deliberately do NOT come through here. Parking a wait that has
 * a deadline needs a timer to end it, and arming one per wait was measured to
 * break sixteen-core runs — a child's exit racing its own kill, process:died
 * lost for three seconds. A bounded wait is also, by construction, short: the
 * strand that set the deadline expects to be back soon. It keeps its spin.
 *
 * The mark is taken BEFORE the look, every round: what arrives after the mark
 * either turns up in the look (and we return it) or refuses the sleep. */
static bool result_wait_ipc_turnin(Result* out) {
    for (;;) {
        TurnInMark mark = box_mark();
        __sync_synchronize();
        if (result_available() || result_ipc_stash_count() > 0) {
            if (result_pop_ipc(out)) return true;
        }
        box_turn_in(mark);
    }
}

bool result_wait_ipc(Result* out, uint32_t timeout_ms) {
    if (!out) return false;
    if (result_pop_ipc(out)) return true;
    if (timeout_ms == 0)   return result_wait_ipc_turnin(out);
    if (cpu_has_waitpkg()) return result_wait_ipc_umwait(out, timeout_ms);
    return result_wait_ipc_yield(out, timeout_ms);
}

/* ===========================================================================
 * Ф26e — result_wait_ferry: the box::ferry station's consume-any BLOCK.
 *
 * Mirrors result_wait_ipc but pops RAW (result_pop) so it can SEE the isolated
 * KCTX_STORAGE records, and returns after ONE record of ANY kind so the
 * executor's post-block poll sweep runs and every sibling awaiter is serviced
 * (the accept_any contract). A ferry record is handed to the caller; a
 * non-ferry record is routed to its own stash and the call returns false (a
 * sibling has work → let the sweep run); a genuinely empty ring blocks on the
 * ResultRing tail (UMWAIT / cooperative yield) until a KResultPush advances it.
 * =========================================================================== */
static bool result_wait_ferry_umwait(Result* out, uint32_t timeout_ms) {
    ResultRing* rr = result_ring();
    volatile uint64_t *tail_addr = (volatile uint64_t *)
        ((uintptr_t)rr + OFFSETOF(ResultRing, hdr.tail));
    while (1) {
        __sync_synchronize();
        if (ferry_stash_shift(out)) return true;
        if (result_available()) {
            Result e;
            if (result_pop(&e)) {
                if (KCTX_KIND(e.context) == KCTX_STORAGE) { *out = e; return true; }
                result_restash(&e);
                return false;   /* handed a sibling its record — yield to poll sweep */
            }
            /* seq not yet released by the producer — fall through to UMWAIT
             * rather than busy-return. */
        }
        umonitor((volatile void*)tail_addr);
        __sync_synchronize();
        if (!result_available() && result_ferry_stash_count() == 0) {
            uint64_t deadline_tsc = (timeout_ms == 0)
                ? 0xFFFFFFFFFFFFFFFFULL
                : rdtsc() + cpu_ms_to_tsc(timeout_ms);
            int wake_reason = umwait(0, deadline_tsc);
            if (wake_reason == 1 && timeout_ms > 0) {
                __sync_synchronize();
                if (!result_available() && result_ferry_stash_count() == 0) return false;
            }
        }
    }
}

static bool result_wait_ferry_yield(Result* out, uint32_t timeout_ms) {
    uint64_t deadline = 0;
    if (timeout_ms > 0) deadline = rdtsc() + cpu_ms_to_tsc(timeout_ms);
    while (1) {
        __sync_synchronize();
        if (ferry_stash_shift(out)) return true;
        if (result_available()) {
            Result e;
            if (result_pop(&e)) {
                if (KCTX_KIND(e.context) == KCTX_STORAGE) { *out = e; return true; }
                result_restash(&e);
                return false;
            }
        }
        if (timeout_ms > 0 && rdtsc() >= deadline) return false;
        yield();
    }
}

bool result_wait_ferry(Result* out, uint32_t timeout_ms) {
    if (!out) return false;
    if (ferry_stash_shift(out)) return true;
    if (cpu_has_waitpkg()) return result_wait_ferry_umwait(out, timeout_ms);
    return result_wait_ferry_yield(out, timeout_ms);
}

