#include "box/core/result.h"
#include "box/core/notify.h"
#include "box/system.h"
#include "box/cpu.h"
#include "box/memory.h"   /* malloc / free — a spawned strand's stashes live on the heap */
#include "box/string.h"
#include "box/core/stash.h"
#include "box/debug.h"    /* kdbg_print — a deadline-free wait must be able to accuse */
#include "box/turnin.h"   /* box_turn_in — a deadline-free wait must also be able to SLEEP */

bool result_available(void) {
    ResultRing* rr = result_ring();
    __sync_synchronize();
    return rr->hdr.head != rr->hdr.tail;
}

/* The head of the ring: the slot there, its position, the ring's tail and the
 * seq that marks the slot released for this round (the Vyukov consumer gate:
 * seq == 2*round + 1). False when the header is not one this boxlib can trust
 * — slots_base / slot_size / slot_count_max are loaded at runtime on purpose:
 * treated as const-after-init, the checks were elided against a corrupted or
 * uninitialised header (the decks-elf RIP=0x11b8c crash, 2026-04-29) — or
 * when the ring is empty, which the caller tells by *pos == *tail. Whether the
 * slot is released is the caller's own load of its seq. */
static bool head_slot(ResultRing *rr, ResultSlot **slot, uint64_t *pos,
                      uint64_t *tail, uint64_t *expected)
{
    uint32_t cap    = __atomic_load_n(&rr->hdr.slot_count_max, __ATOMIC_RELAXED);
    uint64_t base   = __atomic_load_n(&rr->hdr.slots_base,     __ATOMIC_RELAXED);
    uint32_t stride = __atomic_load_n(&rr->hdr.slot_size,      __ATOMIC_RELAXED);
    if (cap == 0 || base == 0 || stride == 0)        return false;
    if (base < 0x100000000ULL)                       return false;
    /* ABI guard: the kernel must publish slots in 32-byte ResultSlot units. */
    if (stride != sizeof(ResultSlot))                return false;

    /* ACQUIRE on tail pairs with the kernel's ACQ_REL fetch_add so we never
     * see a stale seq from a slot the producer is about to write. */
    *tail = __atomic_load_n(&rr->hdr.tail, __ATOMIC_ACQUIRE);
    *pos  = __atomic_load_n(&rr->hdr.head, __ATOMIC_RELAXED);
    if (*pos == *tail) return false;

    *slot     = (ResultSlot *)(uintptr_t)(base + (*pos % cap) * stride);
    *expected = 2u * (*pos / (uint64_t)cap) + 1u;
    return true;
}

/* True when the slot at the ring's head is published and unconsumed — a record
 * this strand can pop right now. result_available() only says a producer has
 * CLAIMED past head; the claim is released later, and a sleep that ends on the
 * claim can be re-armed before the release and never look again. This is the
 * fact box_turn_in ends on. No side effects. */
bool result_published_at_head(void) {
    ResultRing* rr = result_ring();
    if (!rr) return false;
    ResultSlot *slot; uint64_t pos, tail, expected;
    if (!head_slot(rr, &slot, &pos, &tail, &expected)) return false;
    return __atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE) == expected;
}

/* The record at the head of the ring, left where it is: what result_pop would
 * hand over next, so a consumer can secure a place for it BEFORE taking it. */
bool result_peek(Result* out) {
    ResultRing* rr = result_ring();
    if (!rr || !out) return false;
    ResultSlot *slot; uint64_t pos, tail, expected;
    if (!head_slot(rr, &slot, &pos, &tail, &expected)) return false;
    if (__atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE) != expected) return false;
    *out = slot->r;
    return true;
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

    ResultSlot *slot; uint64_t pos = 0, tail = 1, expected;
    if (!head_slot(rr, &slot, &pos, &tail, &expected)) {
        if (pos == tail) __atomic_add_fetch(&g_rp_empty, 1, __ATOMIC_RELAXED);
        return false;
    }

    /* If the producer holding `pos` hasn't released its seq yet, slot.seq is
     * still 2*round (the prior round's "free" marker) — we treat it as empty
     * and the caller will retry on its next poll. */
    uint64_t seq = __atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE);
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
 * only its OWN ResultRing, so its stashes must be private too — sharing one
 * process-wide stash across concurrent strands would interleave and misroute
 * replies (the same data race the per-strand rings fix).
 *
 * Three stashes, by whom a record is for: the IPC receiver, the ferry station
 * (box::ferry, KCTX_STORAGE — never handed to a synchronous wait) and the
 * caller of a synchronous submit. Each grows by the chunk and never drops
 * (box/core/stash.h). The main strand keeps the three statics below; a
 * spawned strand keeps three on the heap, cached in its StrandInfo. */
static Stash g_ipc_stash;
static Stash g_non_ipc_stash;
static Stash g_ferry_stash;

/* The calling strand's stash of one kind. The main strand's static, or the
 * spawned strand's heap one cached in its StrandInfo slot (the kernel
 * zero-inits the slot, so 0 is "not yet"). With `create` false this only
 * looks. NULL when there is none — or none could be made, the few bytes the
 * heap did not have. A chunk is one ring's worth: the ring's own count, the
 * kernel's number, read from the header. */
static Stash *stash_of(uint64_t *slot, Stash *main_stash, bool create)
{
    Stash *s = main_stash;
    if (slot) {
        s = (Stash *)(uintptr_t)*slot;
        if (!s) {
            if (!create) return NULL;
            s = malloc(sizeof(Stash));
            if (!s) return NULL;
            memset(s, 0, sizeof(*s));
            *slot = (uint64_t)(uintptr_t)s;
        }
    }
    if (s->entry_size == 0) {
        ResultRing *rr = result_ring();
        uint32_t cap = rr ? __atomic_load_n(&rr->hdr.slot_count_max, __ATOMIC_RELAXED) : 0;
        stash_init(s, sizeof(Result), cap);
    }
    return s;
}

static Stash *ipc_stash_self(bool create) {
    StrandInfo *si = strand_info_or_null();
    return stash_of(si ? &si->ipc_stash_ptr : NULL, &g_ipc_stash, create);
}
static Stash *non_ipc_stash_self(bool create) {
    StrandInfo *si = strand_info_or_null();
    return stash_of(si ? &si->non_ipc_stash_ptr : NULL, &g_non_ipc_stash, create);
}
static Stash *ferry_stash_self(bool create) {
    StrandInfo *si = strand_info_or_null();
    return stash_of(si ? &si->ferry_stash_ptr : NULL, &g_ferry_stash, create);
}

/* Whom a record is for. */
enum {
    FOR_CALLER = 1u << 0,   /* a plain kernel reply — the synchronous submit's */
    FOR_IPC    = 1u << 1,   /* another process wrote it — the IPC receiver's  */
    FOR_FERRY  = 1u << 2,   /* a storage completion — the ferry station's     */
    FOR_NOBODY = 1u << 3,   /* it only moved the cursor                        */
};

static uint32_t route_of(const Result *e)
{
    if (e->sender_pid != 0)                       return FOR_IPC;
    if (KCTX_KIND(e->context) == KCTX_STORAGE)    return FOR_FERRY;
    /* The async-park ack and Brook's bell carry ERR_WOULD_BLOCK and nothing
     * else; a Touch has not come this way since 2026-06-01 (TouchRing). */
    if (KCTX_KIND(e->context) == KCTX_TOUCH)      return FOR_NOBODY;
    if (e->error_code == 9 /* ERR_WOULD_BLOCK */) return FOR_NOBODY;
    return FOR_CALLER;
}

static Stash *stash_for(uint32_t route)
{
    switch (route) {
    case FOR_IPC:    return ipc_stash_self(true);
    case FOR_FERRY:  return ferry_stash_self(true);
    case FOR_CALLER: return non_ipc_stash_self(true);
    default:         return NULL;
    }
}

static const char *name_of(uint32_t route)
{
    switch (route) {
    case FOR_IPC:    return "an IPC message";
    case FOR_FERRY:  return "a ferry completion";
    default:         return "a kernel reply";
    }
}

/* The heap has no room to keep a record that is not for the caller at hand.
 * Said once per process, and said WITHOUT waiting for an answer: the record
 * that could not be kept sits at the head of this very ring, and a reply
 * awaited here would stand behind it for ever. */
static bool g_stash_no_room_said;

static void stash_no_room(uint32_t route)
{
    if (g_stash_no_room_said) return;
    g_stash_no_room_said = true;
    char line[160];
    size_t n = 0;
    const char *parts[3] = { "[stash] DEFECT: no memory to keep ", name_of(route),
                             " - it stays in the ring, and this strand cannot reach what is behind it" };
    for (int i = 0; i < 3; i++) {
        size_t l = strlen(parts[i]);
        if (n + l >= sizeof(line)) l = sizeof(line) - 1 - n;
        memcpy(line + n, parts[i], l);
        n += l;
    }
    line[n] = '\0';
    kdbg_nowait(line);
}

/* The ring, one record at a time, each routed where it belongs. A record for
 * `want` is handed back (1). One for `drop` is popped and forgotten, as is
 * one for nobody. Any other is kept in its stash — after room for it is
 * secured, so it never leaves the ring for nowhere: with no room it stays
 * (-1, said aloud). An empty ring, or one whose head the kernel has not
 * released yet, is 0. With `one_step` the first record kept for somebody
 * else ends the call (2), so a caller that takes turns with its siblings can
 * give them theirs. */
static int ring_take(uint32_t want, uint32_t drop, bool one_step, Result *out)
{
    Result e;
    while (result_peek(&e)) {
        uint32_t r = route_of(&e);
        if (r & want) {
            if (!result_pop(&e)) return 0;
            *out = e;
            return 1;
        }
        if (r & (drop | FOR_NOBODY)) {
            if (!result_pop(&e)) return 0;
            continue;
        }
        Stash *st = stash_for(r);
        if (!st || !stash_reserve(st)) { stash_no_room(r); return -1; }
        if (!result_pop(&e)) return 0;
        stash_put(st, &e);
        if (one_step) return 2;
    }
    return 0;
}

bool result_pop_non_ipc(Result* out) {
    if (!out) return false;
    if (stash_take(non_ipc_stash_self(false), out)) return true;
    return ring_take(FOR_CALLER, 0, false, out) == 1;
}

bool result_pop_ipc(Result* out) {
    if (!out) return false;
    if (stash_take(ipc_stash_self(false), out)) return true;
    return ring_take(FOR_IPC, 0, false, out) == 1;
}

/* Ф26e — result_pop_ferry: the box::ferry station's non-blocking drain. The
 * next KCTX_STORAGE completion from the ferry stash or the ring; every other
 * record met on the way goes to its own consumer, so nothing a sibling awaiter
 * is owed is ever stranded. */
bool result_pop_ferry(Result* out) {
    if (!out) return false;
    if (stash_take(ferry_stash_self(false), out)) return true;
    return ring_take(FOR_FERRY, 0, false, out) == 1;
}

/* Any message — an IPC one or a kernel reply — without blocking; a ferry
 * completion met on the way goes to its station. For IPC servers and
 * box::result_any's poll, which used to pop the bare ring for this. */
bool result_pop_any(Result* out) {
    if (!out) return false;
    if (result_pop_ipc(out))     return true;
    if (result_pop_non_ipc(out)) return true;
    return ring_take(FOR_IPC | FOR_CALLER, 0, false, out) == 1;
}

/* A record already taken out of the ring, back to the stash of its consumer —
 * print.c holds IPC messages met while it waits for the display's grant and
 * hands them back afterwards. The one place a record can be lost: it is out of
 * the ring, and when the heap has no room for it there is nowhere else. Said
 * aloud, as any drop must be. */
void result_restash(const Result* r) {
    if (!r) return;
    uint32_t route = route_of(r);
    if (route == FOR_NOBODY) return;
    Stash *st = stash_for(route);
    if (!st || !stash_reserve(st)) { stash_no_room(route); return; }
    stash_put(st, r);
}

uint32_t result_ipc_stash_count(void)     { return stash_count(ipc_stash_self(false)); }
uint32_t result_non_ipc_stash_count(void) { return stash_count(non_ipc_stash_self(false)); }
uint32_t result_ferry_stash_count(void)   { return stash_count(ferry_stash_self(false)); }

/* A spawned strand's stashes back to the heap, at its exit. The main strand's
 * are static and die with the cabin. */
void result_stash_free_self(void) {
    StrandInfo *si = strand_info_or_null();
    if (!si) return;
    uint64_t *slots[3] = { &si->ipc_stash_ptr, &si->non_ipc_stash_ptr, &si->ferry_stash_ptr };
    for (int i = 0; i < 3; i++) {
        Stash *s = (Stash *)(uintptr_t)*slots[i];
        if (!s) continue;
        stash_free(s);
        free(s);
        *slots[i] = 0;
    }
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
 * Everything in the caller's own stash is by definition skipped past — gone
 * too. IPC messages and ferry completions met in the ring are kept for
 * their consumers (a ferry completion belongs to a still-live awaiter).
 *
 * Safe to call ONLY before submitting a fresh synchronous Manifest. Async
 * paths (touch_await, readline) must NOT use this helper because their
 * pending replies look identical to orphans. */
void result_drain_orphan_replies(void) {
    stash_clear(non_ipc_stash_self(false));
    Result e;
    (void)ring_take(0, FOR_CALLER, false, &e);
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
    stash_clear(non_ipc_stash_self(false));
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
    /* Ф26e: a ferry (KCTX_STORAGE) record met here goes to the ferry stash and
     * the call returns false. The false hands control back to the executor's
     * poll sweep so the box::ferry sibling that owns this completion can
     * collect it; without it a lone storage completion would never break this
     * block and the ferry would hang. The records whose only job was to move
     * the cursor — the async-park ack, and Brook's bell — are nobody's and are
     * dropped here as everywhere else: an IPC server handed one would read it
     * as traffic that nobody sent. */
    {
        int r = ring_take(FOR_IPC | FOR_CALLER, 0, true, out);
        if (r == 1) return true;
        if (r != 0) return false;
    }

    uint64_t deadline = 0;
    if (timeout_ms > 0) {
        deadline = rdtsc() + cpu_ms_to_tsc(timeout_ms);
    }

    while (1) {
        __sync_synchronize();

        if (result_available()) {
            int r = ring_take(FOR_IPC | FOR_CALLER, 0, true, out);
            if (r == 1) return true;
            if (r != 0) return false;
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
 * Mirrors result_wait_ipc but takes the ring one record at a time so it can
 * SEE the isolated KCTX_STORAGE records, and returns after ONE record of ANY
 * kind so the executor's post-block poll sweep runs and every sibling awaiter
 * is serviced (the accept_any contract). A ferry record is handed to the
 * caller; a non-ferry record is kept in its own stash and the call returns
 * false (a sibling has work → let the sweep run); a genuinely empty ring
 * blocks on the ResultRing tail (UMWAIT / cooperative yield) until a
 * KResultPush advances it.
 * =========================================================================== */
static bool result_wait_ferry_umwait(Result* out, uint32_t timeout_ms) {
    ResultRing* rr = result_ring();
    volatile uint64_t *tail_addr = (volatile uint64_t *)
        ((uintptr_t)rr + OFFSETOF(ResultRing, hdr.tail));
    while (1) {
        __sync_synchronize();
        if (stash_take(ferry_stash_self(false), out)) return true;
        if (result_available()) {
            int r = ring_take(FOR_FERRY, 0, true, out);
            if (r == 1) return true;
            if (r != 0) return false;   /* handed a sibling its record — yield to poll sweep */
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
        if (stash_take(ferry_stash_self(false), out)) return true;
        if (result_available()) {
            int r = ring_take(FOR_FERRY, 0, true, out);
            if (r == 1) return true;
            if (r != 0) return false;
        }
        if (timeout_ms > 0 && rdtsc() >= deadline) return false;
        yield();
    }
}

bool result_wait_ferry(Result* out, uint32_t timeout_ms) {
    if (!out) return false;
    if (stash_take(ferry_stash_self(false), out)) return true;
    if (cpu_has_waitpkg()) return result_wait_ferry_umwait(out, timeout_ms);
    return result_wait_ferry_yield(out, timeout_ms);
}

