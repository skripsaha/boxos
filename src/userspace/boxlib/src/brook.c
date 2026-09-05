/*
 * boxlib brook.c — userspace side of the SPSC streaming primitive.
 *
 * Hot path runs lock-free entirely in this process (atomic head/tail
 * updates + memcpy of the frame slot). Block-on-full / block-on-empty
 * loops via pause / UMWAIT / yield on the shared BrookHeader cursor —
 * the cache line invalidation on the peer's release-store IS the wake.
 * No wake-up syscall is needed; the cursor itself is the synchronization
 * point. (Same model Touch uses for its event channel — see
 * touch_wait_umwait / touch_wait_pause in this directory.)
 *
 * Peer death is detected by reading the kernel-managed *_alive flag in
 * the shared header. The history bit (*_ever_attached) lets a reader
 * that opens before any writer wait (vs returning EOF immediately).
 *
 * Timeouts are tracked in userspace via clock_uptime_ms — the kernel
 * never gets involved in the wait, so the API can offer arbitrary
 * deadlines without growing per-process state.
 */

#include "box/brook.h"
#include "box/types.h"
#include "box/string.h"
#include "box/error.h"
#include "box/memory.h"
#include "box/clock.h"
#include "box/system.h"
#include "box/cpu.h"
#include "box/core/manifest.h"
#include "box/core/pocket.h"
#include "box/turnin.h"   /* box_turn_in — a sleep on a stream must cost no core */
#include "box/core/strand_self.h"  /* strand_self — whose bell to hang */
#include "boxos_decks.h"  /* SYSTEM_OP_BROOK_* opcodes — single source */

/* Shape limits — mirror kernel brook.h. Caller-side validation gives
 * better error reporting before crossing the syscall boundary. */
#define BROOK_FRAME_SIZE_MIN     8u
#define BROOK_FRAME_SIZE_MAX     (64u * 1024u)
#define BROOK_FRAME_COUNT_MIN    2u
#define BROOK_FRAME_COUNT_MAX    16384u
#define BROOK_MAX_TOTAL_SIZE     (1ULL * 1024 * 1024 * 1024)

/* Sticky terminal sentinel — mirror kernel BROOK_ALIVE_FROZEN. */
#define BROOK_ALIVE_FROZEN       0xFFFFFFFFu

/* Spin tuning — how many pause() iterations to burn before yielding to
 * the scheduler. PAUSE is a single µop on most micro-arches so a few
 * hundred is well under one PIT tick (4 ms @ 250 Hz) and lets a peer
 * that's actively pushing/popping complete without the round-trip
 * cost of yield(). Beyond that we yield to let other processes run. */
#define BROOK_SPIN_BUDGET        2048u

/* ─────────────────────────────────────────────────────────────────────
 * BrookHeader — must mirror kernel layout EXACTLY. Validated by the
 * sizeof / offsetof asserts below; if the kernel struct ever changes
 * the build fails here loudly.
 *
 * CL0 = writer state (watched by reader for both data-available AND
 * peer-death). CL1 = reader state (watched by writer). See kernel
 * brook.h for the full UMWAIT-correctness rationale.
 * ───────────────────────────────────────────────────────────────────── */
typedef struct {
    /* Cacheline 0 — writer state, watched by reader */
    volatile uint64_t tail;
    volatile uint32_t writer_alive;
    volatile uint32_t writer_ever_attached;
    uint32_t          frame_size;
    uint32_t          frame_count;
    uint64_t          magic;
    volatile uint32_t writer_bell;       /* a sleeping writer's pid; 0 = awake */
    uint8_t           _pad_line0[28];

    /* Cacheline 1 — reader state, watched by writer */
    volatile uint64_t head;
    volatile uint32_t reader_alive;
    volatile uint32_t reader_ever_attached;
    volatile uint32_t reader_bell;       /* a sleeping reader's pid; 0 = awake */
    uint8_t           _pad_line1[44];
} BrookHeaderUser;

STATIC_ASSERT(sizeof(BrookHeaderUser) == 128,
              "BrookHeaderUser must match kernel BrookHeader (128 B)");
STATIC_ASSERT(OFFSETOF(BrookHeaderUser, tail) == 0,
              "BrookHeaderUser.tail offset must be 0 (CL0)");
STATIC_ASSERT(OFFSETOF(BrookHeaderUser, head) == 64,
              "BrookHeaderUser.head offset must be 64 (CL1)");
STATIC_ASSERT(OFFSETOF(BrookHeaderUser, writer_alive) < 64,
              "writer_alive must share CL0 with tail");
STATIC_ASSERT(OFFSETOF(BrookHeaderUser, reader_alive) >= 64,
              "reader_alive must share CL1 with head");
STATIC_ASSERT(OFFSETOF(BrookHeaderUser, reader_bell) >= 64,
              "reader_bell must share CL1 with head — the line the writer already reads");
STATIC_ASSERT(OFFSETOF(BrookHeaderUser, writer_bell) < 64,
              "writer_bell must share CL0 with tail — the line the reader already reads");

/* ─────────────────────────────────────────────────────────────────────
 * Brook — opaque user handle. Allocated via malloc on open, freed on
 * release.
 * ───────────────────────────────────────────────────────────────────── */
struct Brook {
    BrookHeaderUser *hdr;        /* mapped header page */
    uint8_t         *slots;      /* mapped slot region */
    uint64_t         va_header;  /* original VA — used for syscalls */
    uint32_t         frame_size; /* immutable after open */
    uint32_t         frame_count;/* immutable after open, power-of-2 */
    uint32_t         role;       /* BROOK_WRITER or BROOK_READER */
    uint32_t         streaming;  /* BROOK_STREAM was set at open: skip EOF/TERM */
    uint32_t         self_pid;   /* whose bell to hang — resolved once at open */
};

/* ─────────────────────────────────────────────────────────────────────
 * Atomic helpers — GCC builtins.
 * ───────────────────────────────────────────────────────────────────── */
static inline uint64_t brook_load_acquire_u64(volatile uint64_t *p)
{ return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static inline uint64_t brook_load_relaxed_u64(volatile uint64_t *p)
{ return __atomic_load_n(p, __ATOMIC_RELAXED); }
static inline void brook_store_release_u64(volatile uint64_t *p, uint64_t v)
{ __atomic_store_n(p, v, __ATOMIC_RELEASE); }
static inline uint32_t brook_load_acquire_u32(volatile uint32_t *p)
{ return __atomic_load_n(p, __ATOMIC_ACQUIRE); }

static inline void brook_cpu_pause(void)
{
    __asm__ volatile("pause" ::: "memory");
}

static inline bool brook_cas_u32(volatile uint32_t *p, uint32_t expect, uint32_t want)
{
    return __atomic_compare_exchange_n(p, &expect, want, false,
                                       __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
}

/* Take a bell if one is hung out, and ring it.
 *
 * Called by whichever side has just made the other side's wait end — the
 * writer after publishing a frame, the reader after freeing a slot — and ONLY
 * there, because a bell means "I looked and there was nothing", so it is the
 * cursor moving that makes it worth ringing.
 *
 * The CAS is what keeps the cost at one ring per sleep no matter how many
 * pushes or pops follow: the first to take the bell rings, everyone after
 * finds 0 and pays a compare. The sleeper hangs it out again next time it
 * goes down.
 *
 * A bell says nothing; it only says come. It is its own kernel op rather than
 * an empty IPC message, and deliberately: an empty message was tried, and
 * hiding it required every receive path in the system to learn that a
 * contentless message is not a message — a change to what IPC means, made to
 * serve one subsystem's doorbell. system.bell pushes a record wearing the
 * shape every consumer has always discarded, so it can never be mistaken for
 * a message or for somebody's answer, and Brook never has to learn anybody's
 * wire protocol.
 *
 * Best-effort by construction: a ring to a pid that has gone away is answered
 * OK and does nothing, and a failure here must never turn a successful push or
 * pop into an error. The bell has already been taken down, so this does not
 * retry it — if the peer really is alive and merely missed this one, it hangs
 * its bell out again on its next trip down. */
static void brook_ring(volatile uint32_t *bell)
{
    uint32_t who = brook_load_acquire_u32(bell);
    if (who == 0) return;
    if (!brook_cas_u32(bell, who, 0)) return;   /* somebody else is ringing */

    uint8_t params[4];
    memcpy(params, &who, sizeof(uint32_t));
    (void)MfCall1(DECK_SYSTEM, SYSTEM_OP_BELL,
                  params, sizeof(params), NULL, 0, NULL, 0, NULL,
                  0 /* no deadline — reply guaranteed */, NULL);
}

/* This side's own bell — the one it hangs out — and the peer's, which it
 * rings. A writer sleeps on writer_bell and rings reader_bell; a reader is the
 * mirror image. */
static inline volatile uint32_t *brook_own_bell(BrookHeaderUser *h, uint32_t role)
{
    return (role == BROOK_WRITER) ? &h->writer_bell : &h->reader_bell;
}

static inline volatile uint32_t *brook_peer_bell(BrookHeaderUser *h, uint32_t role)
{
    return (role == BROOK_WRITER) ? &h->reader_bell : &h->writer_bell;
}

/* Has the wait ended? Each is its side's own question — "is there a frame
 * yet", "is there room yet" — asked one last time with the bell already
 * hanging, and it must also answer TRUE for a peer that has DEPARTED. A stream
 * whose other end went away between the caller's look and this one would
 * otherwise be slept on for ever: the departure is a store to `*_alive` that
 * no sleeping strand is going to read.
 *
 * Neither decides anything. The caller's loop re-runs and does the real work —
 * the copy, or the freeze-CAS that turns a departure into EOF. */
static bool brook_pop_ready(Brook *b)
{
    BrookHeaderUser *h = b->hdr;
    uint64_t head = brook_load_relaxed_u64(&h->head);   /* ours; single reader */
    uint64_t tail = brook_load_acquire_u64(&h->tail);
    if (head != tail) return true;
    if (b->streaming) return false;
    return brook_load_acquire_u32(&h->writer_alive) == 0 &&
           brook_load_acquire_u32(&h->writer_ever_attached) != 0;
}

static bool brook_push_ready(Brook *b)
{
    BrookHeaderUser *h = b->hdr;
    uint64_t tail = brook_load_relaxed_u64(&h->tail);   /* ours; single writer */
    uint64_t head = brook_load_acquire_u64(&h->head);
    if ((tail - head) < b->frame_count) return true;
    if (b->streaming) return false;
    return brook_load_acquire_u32(&h->reader_alive) == 0 &&
           brook_load_acquire_u32(&h->reader_ever_attached) != 0;
}

/* Go to sleep on this stream, and be reachable while asleep.
 *
 * The order is the whole proof, and it is the same one the console daemon
 * uses: hang the bell FIRST, then take the delivery mark, then look ONE more
 * time. Anything the peer does from here on either finds the bell out (and
 * rings, which is itself an arrival on this strand's Result ring), or moves
 * the cursor this last look reads. There is no third way and no gap between
 * the two.
 *
 * `recheck` is the caller's own question — "is there a frame yet", "is there
 * room yet" — asked again with the bell already hanging. It also has to
 * answer true for a peer that has DEPARTED, or a stream whose other end went
 * away between the caller's look and this one would be slept on for ever.
 *
 * Returns with the bell taken back in. The caller loops and looks again: this
 * promises nothing about how long it slept, only that it did not spin. */
static void brook_sleep_until(Brook *b, bool (*recheck)(Brook *))
{
    BrookHeaderUser   *h    = b->hdr;
    volatile uint32_t *mine = brook_own_bell(h, b->role);

    /* SEQ_CST, and the seq_cst is the point: it is a full barrier, so this
     * store cannot sit in the store buffer while the look below already runs.
     * The peer's half is fenced by its own cursor publish (a locked exchange),
     * so one of the two always sees the other. */
    __atomic_store_n(mine, b->self_pid, __ATOMIC_SEQ_CST);

    TurnInMark mark = box_mark();
    if (!recheck(b)) box_turn_in(mark);

    __atomic_store_n(mine, 0u, __ATOMIC_RELEASE);
}

/* ─────────────────────────────────────────────────────────────────────
 * Syscall wrappers — only OPEN / RELEASE / INFO touch the kernel.
 * ───────────────────────────────────────────────────────────────────── */
static int brook_sys_open(const char *tag,
                          uint32_t frame_size, uint32_t frame_count,
                          uint32_t flags,
                          uint64_t *out_va_header,
                          uint64_t *out_va_slots,
                          uint32_t *out_frame_size,
                          uint32_t *out_frame_count)
{
    uint8_t params[12];
    memcpy(params,     &frame_size,  sizeof(uint32_t));
    memcpy(params + 4, &frame_count, sizeof(uint32_t));
    memcpy(params + 8, &flags,       sizeof(uint32_t));

    uint8_t out[24] = {0};
    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_BROOK_OPEN,
                     params, sizeof(params),
                     tag, (uint32_t)(strlen(tag) + 1),
                     out, sizeof(out), 0,
                     0 /* no deadline — reply guaranteed */, 0);
    if (rc != 0) return rc;
    memcpy(out_va_header,    out,      sizeof(uint64_t));
    memcpy(out_va_slots,     out + 8,  sizeof(uint64_t));
    memcpy(out_frame_size,   out + 16, sizeof(uint32_t));
    memcpy(out_frame_count,  out + 20, sizeof(uint32_t));
    return 0;
}

static int brook_sys_release(uint64_t va_header)
{
    uint8_t params[8];
    memcpy(params, &va_header, sizeof(uint64_t));
    return MfCall1(DECK_SYSTEM, SYSTEM_OP_BROOK_RELEASE,
                   params, sizeof(params), 0, 0, 0, 0, 0, 0 /* no deadline — reply guaranteed */, 0);
}

/* ─────────────────────────────────────────────────────────────────────
 * Validation helpers.
 * ───────────────────────────────────────────────────────────────────── */
static bool brook_is_pow2_u32(uint32_t v)
{ return v != 0 && (v & (v - 1)) == 0; }

static int brook_validate_create_shape(uint32_t fs, uint32_t fc)
{
    if (fs < BROOK_FRAME_SIZE_MIN  || fs > BROOK_FRAME_SIZE_MAX)  return -ERR_INVALID_ARGS;
    if (fc < BROOK_FRAME_COUNT_MIN || fc > BROOK_FRAME_COUNT_MAX) return -ERR_INVALID_ARGS;
    if (!brook_is_pow2_u32(fc))                                   return -ERR_INVALID_ARGS;
    if ((uint64_t)fs * fc > BROOK_MAX_TOTAL_SIZE)                 return -ERR_INVALID_ARGS;
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────
 * brook_open / brook_release
 * ───────────────────────────────────────────────────────────────────── */
Brook *brook_open(const char *tag, uint32_t frame_size, uint32_t frame_count,
                  uint32_t flags)
{
    if (!tag || tag[0] == '\0') return 0;

    uint32_t role = flags & (BROOK_WRITER | BROOK_READER);
    if (role != BROOK_WRITER && role != BROOK_READER) return 0;

    if (flags & BROOK_CREATE) {
        if (brook_validate_create_shape(frame_size, frame_count) != 0) return 0;
    }

    uint64_t va_header = 0, va_slots = 0;
    uint32_t out_fs = 0, out_fc = 0;
    int rc = brook_sys_open(tag, frame_size, frame_count, flags,
                            &va_header, &va_slots, &out_fs, &out_fc);
    if (rc != 0) return 0;

    Brook *b = (Brook *)malloc(sizeof(Brook));
    if (!b) {
        brook_sys_release(va_header);
        return 0;
    }
    memset(b, 0, sizeof(*b));
    b->hdr         = (BrookHeaderUser *)(uintptr_t)va_header;
    b->slots       = (uint8_t *)(uintptr_t)va_slots;
    b->va_header   = va_header;
    b->frame_size  = out_fs;
    b->frame_count = out_fc;
    b->role        = role;
    b->streaming   = (flags & BROOK_STREAM) ? 1u : 0u;
    /* Resolved once, here, and never again: a bell has to name a STRAND, and
     * the strand that opens a Brook is the one that will sleep on it. Asking
     * for it on the sleeping path instead would put a lookup between hanging
     * the bell and the last look, which is the one place nothing may go. */
    b->self_pid    = strand_self();
    return b;
}

/* CAS the peer's alive flag 0 → FROZEN. Returns true if WE made the
 * EOF decision (or it was already FROZEN — sticky), false if the peer
 * just attached (alive=1) and we should NOT EOF.
 *
 * Memory ordering: ACQ_REL on success so a subsequent reader observes
 * our FROZEN write and ANY pending state writes from the peer that
 * preceded its alive=0 store are visible. ACQUIRE on failure to see
 * the peer's freshly-published alive=1 + slot writes. */
static inline bool brook_cas_freeze_peer(volatile uint32_t *peer_alive)
{
    uint32_t expected = 0u;
    if (__atomic_compare_exchange_n(peer_alive, &expected,
                                    BROOK_ALIVE_FROZEN,
                                    false,
                                    __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE)) {
        return true;
    }
    /* Expected was either FROZEN (sticky — peer already terminal in a
     * prior loop iter, treat as success) or 1 (peer just attached;
     * caller should not EOF). */
    return expected == BROOK_ALIVE_FROZEN;
}

int brook_release(Brook *b)
{
    if (!b) return -ERR_INVALID_ARGS;
    /* Take the bell in on the way out. A side that leaves with its pid still
     * hanging leaves its peer a cord to pull for somebody who is not there —
     * one wasted message per lane, and a stale pid on a page the kernel keeps
     * for as long as the writer holds it. Nothing depends on this being done;
     * it is done because leaving it undone is untidy in a way that becomes
     * confusing evidence later. */
    if (b->hdr)
        __atomic_store_n(brook_own_bell(b->hdr, b->role), 0u, __ATOMIC_RELEASE);
    int rc = brook_sys_release(b->va_header);
    free(b);
    return rc;
}

/* ─────────────────────────────────────────────────────────────────────
 * Wait policy.
 *
 * Two paths, chosen at runtime:
 *
 *  - UMWAIT path (gated on cpu_has_waitpkg, present on Intel Tremont
 *    Atoms and Tiger Lake / Sapphire Rapids cores and newer): arm a
 *    UMONITOR on the peer's cacheline (containing both the peer's
 *    cursor AND the peer's alive flag — see the BrookHeader layout
 *    rationale), then UMWAIT with a TSC deadline. The CPU enters a
 *    low-power C0-sub state until the cacheline changes (peer push/
 *    release/kernel-death wake), an interrupt fires, or the TSC
 *    deadline expires. Recovers µs-range wake latency at near-zero
 *    CPU draw.
 *
 *  - Pause+yield fallback: keep the cache line hot for BROOK_SPIN_BUDGET
 *    pauses (≈ a few µs on real silicon), then yield the scheduler so
 *    other processes get CPU. The peer's release-store will reach us
 *    on the next loop iteration. Granularity ≈ scheduler tick (4 ms).
 *
 * Both paths are SAFE — they may wake spuriously; the caller's loop
 * always re-checks the cursor + alive flag after wake.
 *
 * `watch_addr` MUST point into the peer's cacheline so a peer-state
 * change (cursor advance OR kernel alive=0 write) wakes the monitor.
 * `deadline_ms == 0` means "no caller-imposed deadline"; UMWAIT path
 * still caps the kernel sleep at a moderate value so we periodically
 * re-poll the alive flag even on pathological cache-coherence quirks.
 * Returns nothing — caller's loop re-checks state on return.
 * ───────────────────────────────────────────────────────────────────── */
#define BROOK_UMWAIT_DEADLINE_CAP_MS    50u   /* periodic re-poll ceiling */

static void brook_wait_cycle(volatile void *watch_addr,
                             uint32_t *spin_counter,
                             uint64_t deadline_ms)
{
    if (cpu_has_waitpkg()) {
        /* UMONITOR (Intel SDM Vol 2A — opcode F3 0F AE /6) arms the
         * hardware monitor on the cacheline of `watch_addr`. The
         * cacheline granularity comes from CPUID.05H (typically 64 B,
         * 32 B on Atom Tremont/Goldmont, 128 B on some server SKUs).
         * Any subsequent store to that line — peer cursor advance OR
         * the kernel's alive=0 flip on peer-death (both co-located on
         * the same cacheline by the BrookHeader layout, see kernel
         * brook.h) — wakes UMWAIT immediately. SDM UMONITOR:
         * "Only write-back memory is guaranteed to correctly trigger
         * the monitoring hardware" — our header page is mapped via
         * VMM_FLAGS_USER_RW (no PCD/PWT/PAT bits) → WB. */
        umonitor(watch_addr);

        /* Compute TSC deadline. Always cap at
         * BROOK_UMWAIT_DEADLINE_CAP_MS so a missed wake on real
         * silicon (monitor latency, scheduler interaction, IA32_
         * UMWAIT_CONTROL OS-imposed early exit) drains into a fresh
         * loop iteration within bounded time. The caller tracks the
         * user-facing deadline separately and returns ERR_TIMEOUT on
         * the next loop pass.
         *
         * The kernel programs IA32_UMWAIT_CONTROL (MSR 0xE1) to ~2 ms
         * worth of TSC quanta — see kernel cpuid.c
         * cpu_umwait_control_init. That cap supersedes our budget_ms
         * here when smaller; CF=1 on early exit is harmless for
         * Brook because the caller's loop re-checks state. */
        uint64_t budget_ms = BROOK_UMWAIT_DEADLINE_CAP_MS;
        if (deadline_ms != 0) {
            uint64_t now_ms = clock_uptime_ms();
            if (now_ms >= deadline_ms) return;       /* caller checks */
            uint64_t remaining = deadline_ms - now_ms;
            if (remaining < budget_ms) budget_ms = remaining;
        }
        uint64_t deadline_tsc = cpu_rdtsc() + cpu_ms_to_tsc(budget_ms);

        /* UMWAIT (Intel SDM Vol 2A — opcode F2 0F AE /6). State=0 ⇒
         * C0.2 (slower wake, larger power saving) — matches Touch's
         * choice for consistency across IPC primitives. Return value
         * (CF) indicates whether OS time limit elapsed (1) or another
         * wake event fired (0, e.g. monitor write, NMI, interrupt).
         * The caller's loop re-checks head/tail/alive on return so we
         * don't need to act on CF directly; both outcomes are safe. */
        (void)umwait(0, deadline_tsc);
        return;
    }

    /* Pause + yield fallback. PAUSE (Intel SDM Vol 2B PAUSE) is the
     * SDM-recommended spin hint — saves power and reduces inter-core
     * pipeline-flush penalties on a contended cacheline. */
    if ((*spin_counter)++ < BROOK_SPIN_BUDGET) {
        brook_cpu_pause();
    } else {
        *spin_counter = 0;
        yield();
    }
}

/* ─────────────────────────────────────────────────────────────────────
 * brook_push core. `mode` selects block/timeout/try; deadline_ms is
 * the absolute deadline (clock_uptime_ms-relative) for TIMEOUT mode.
 * ───────────────────────────────────────────────────────────────────── */
typedef enum {
    BROOK_MODE_TRY     = 0,
    BROOK_MODE_BLOCK   = 1,
    BROOK_MODE_TIMEOUT = 2,
} BrookWaitMode;

static int brook_push_core(Brook *b, const void *frame,
                           BrookWaitMode mode, uint64_t deadline_ms)
{
    if (!b || !frame)             return -ERR_INVALID_ARGS;
    if (b->role != BROOK_WRITER)  return -ERR_INVALID_ARGS;

    BrookHeaderUser *h = b->hdr;
    uint32_t cap = b->frame_count;
    uint32_t cap_mask = cap - 1;            /* power-of-2 guarantee */
    uint32_t fs = b->frame_size;

    uint32_t spin = 0;
    for (;;) {
        uint64_t tail = brook_load_relaxed_u64(&h->tail);
        uint64_t head = brook_load_acquire_u64(&h->head);

        if ((tail - head) < cap) {
            /* Free slot — write payload, then release tail. Release on
             * tail publishes the memcpy to the reader. */
            uint8_t *slot = b->slots + ((uint32_t)(tail & cap_mask)) * fs;
            memcpy(slot, frame, fs);

            /* Publish with a LOCKed exchange rather than a release store.
             *
             * The value written is identical; what the locked form buys is the
             * StoreLoad barrier the bell check below cannot do without. x86
             * lets a load pass an earlier store to a different location (SDM 3A
             * 9.2.3.4), so a plain release store here would let this core read
             * `bell` while its own `tail` was still in the store buffer — and
             * the sleeping reader, whose own half of the handshake IS fenced,
             * would read the old tail. Neither side sees the other; the frame
             * sits in the ring and the reader sleeps on it. Dekker's, in a
             * console.
             *
             * One locked instruction replaces store+mfence, so the whole price
             * of the bell on the hot path is the difference between a store and
             * an exchange on a line this core already owns — against a push
             * that already reads the reader's cacheline for `head`. */
            __atomic_exchange_n(&h->tail, tail + 1, __ATOMIC_SEQ_CST);

            brook_ring(&h->reader_bell);
            return 0;
        }

        /* Ring full — peer-death detection. Single-session mode: attempt
         * a CAS on reader_alive 0→FROZEN (atomic w.r.t. a concurrent
         * reader attach). If the CAS commits, the session is terminal;
         * subsequent reader attaches via the kernel fail with
         * ERR_INVALID_STATE, eliminating the re-attach race.
         *
         * Streaming mode (b->streaming): no terminal decision — keep
         * waiting through reader-leave/re-attach cycles. */
        if (!b->streaming &&
            brook_load_acquire_u32(&h->reader_alive) == 0 &&
            brook_load_acquire_u32(&h->reader_ever_attached) != 0) {
            if (brook_cas_freeze_peer(&h->reader_alive)) {
                return -ERR_PROCESS_TERMINATED;
            }
            /* CAS lost: reader just re-attached. Continue loop — the
             * next iter sees alive=1 and will eventually push when the
             * reader drains a slot. */
            continue;
        }

        if (mode == BROOK_MODE_TRY) return -ERR_WOULD_BLOCK;

        if (mode == BROOK_MODE_TIMEOUT) {
            uint64_t now = clock_uptime_ms();
            if (now >= deadline_ms) return -ERR_TIMEOUT;

            /* A BOUNDED wait keeps its spin. Ending it needs a clock, and a
             * sleep with no deadline has none — the caller who set a deadline
             * expects to be back soon, and is the one who must watch it.
             * Writer monitors CL1 (reader state): a reader-side pop advances
             * head, and a kernel-side reader-death writes reader_alive, both
             * on the same cacheline, both wake us. */
            brook_wait_cycle(&h->head, &spin, deadline_ms);
            continue;
        }

        /* BLOCK. Nothing to watch for and no deadline to watch it with, so
         * stop watching: hang the bell and go to sleep. A reader that frees a
         * slot rings it; so does the kernel if the reader departs. */
        brook_sleep_until(b, brook_push_ready);
    }
}

static int brook_pop_core(Brook *b, void *frame,
                          BrookWaitMode mode, uint64_t deadline_ms)
{
    if (!b || !frame)              return -ERR_INVALID_ARGS;
    if (b->role != BROOK_READER)   return -ERR_INVALID_ARGS;

    BrookHeaderUser *h = b->hdr;
    uint32_t cap = b->frame_count;
    uint32_t cap_mask = cap - 1;
    uint32_t fs = b->frame_size;

    uint32_t spin = 0;
    for (;;) {
        uint64_t head = brook_load_relaxed_u64(&h->head);
        uint64_t tail = brook_load_acquire_u64(&h->tail);

        if (head != tail) {
            const uint8_t *slot = b->slots + ((uint32_t)(head & cap_mask)) * fs;
            memcpy(frame, slot, fs);

            /* Publish with a LOCKed exchange rather than a release store —
             * the writer's half of the same argument at the push above. The
             * value written is identical; what the locked form buys is the
             * StoreLoad barrier the bell check below cannot do without, since
             * x86 lets a load pass an earlier store to a different location
             * (SDM 3A 9.2.3.4). Without it this core could read `writer_bell`
             * while its own `head` was still in the store buffer, and a
             * sleeping writer — whose own half IS fenced — would read the old
             * head. Neither sees the other and the stream stops with room in
             * it. */
            __atomic_exchange_n(&h->head, head + 1, __ATOMIC_SEQ_CST);

            brook_ring(&h->writer_bell);
            return 0;
        }

        /* Empty ring + writer transition. Single-session mode (default):
         * if writer was ever attached and is now gone, CAS the
         * writer_alive flag from 0 to FROZEN. Winning the CAS gives us
         * a definitive EOF (subsequent writer attaches will fail in the
         * kernel, eliminating the re-attach race). Losing the CAS means
         * a new writer attached during this very loop iteration —
         * continue waiting.
         *
         * If writer never attached (ever_attached=0), keep waiting — a
         * reader opening before any writer is a valid producer-
         * launches-after pattern.
         *
         * Streaming mode (b->streaming): skip terminal decision —
         * block through writer-leave/re-attach cycles indefinitely. */
        if (!b->streaming &&
            brook_load_acquire_u32(&h->writer_alive) == 0 &&
            brook_load_acquire_u32(&h->writer_ever_attached) != 0) {
            if (brook_cas_freeze_peer(&h->writer_alive)) {
                return -ERR_STREAM_CLOSED;
            }
            continue;
        }

        if (mode == BROOK_MODE_TRY) return -ERR_WOULD_BLOCK;

        if (mode == BROOK_MODE_TIMEOUT) {
            uint64_t now = clock_uptime_ms();
            if (now >= deadline_ms) return -ERR_TIMEOUT;

            /* Bounded: keep the spin, for the writer's reasons (see there).
             * Reader monitors CL0 (writer state): a writer-side push advances
             * tail, and a kernel-side writer-death writes writer_alive, both
             * on the same cacheline. */
            brook_wait_cycle(&h->tail, &spin, deadline_ms);
            continue;
        }

        /* BLOCK — hang the bell and sleep. This is the wait that used to hold
         * a core for as long as a stream was quiet, which on a console lane or
         * a Current is most of the time a machine is on. */
        brook_sleep_until(b, brook_pop_ready);
    }
}

/* ─────────────────────────────────────────────────────────────────────
 * Public push/pop variants.
 * ───────────────────────────────────────────────────────────────────── */
int brook_push(Brook *b, const void *frame)
{
    return brook_push_core(b, frame, BROOK_MODE_BLOCK, 0);
}

int brook_try_push(Brook *b, const void *frame)
{
    return brook_push_core(b, frame, BROOK_MODE_TRY, 0);
}

int brook_push_timeout(Brook *b, const void *frame, uint32_t timeout_ms)
{
    if (timeout_ms == 0) return brook_push(b, frame);
    uint64_t deadline = clock_uptime_ms() + timeout_ms;
    return brook_push_core(b, frame, BROOK_MODE_TIMEOUT, deadline);
}

int brook_pop(Brook *b, void *frame)
{
    return brook_pop_core(b, frame, BROOK_MODE_BLOCK, 0);
}

int brook_try_pop(Brook *b, void *frame)
{
    return brook_pop_core(b, frame, BROOK_MODE_TRY, 0);
}

int brook_pop_timeout(Brook *b, void *frame, uint32_t timeout_ms)
{
    if (timeout_ms == 0) return brook_pop(b, frame);
    uint64_t deadline = clock_uptime_ms() + timeout_ms;
    return brook_pop_core(b, frame, BROOK_MODE_TIMEOUT, deadline);
}

/* ─────────────────────────────────────────────────────────────────────
 * Query helpers — zero syscalls.
 * ───────────────────────────────────────────────────────────────────── */
/* ─────────────────────────────────────────────────────────────────────
 * The bell, hung by hand. See box/brook.h for the discipline.
 *
 * brook_pop and brook_push hang their own bells and need nobody to do it for
 * them. These exist for a reader that does NOT block inside Brook — the
 * console daemon, which try_pops many lanes and then sleeps once for all of
 * them, so the hanging and the sleeping happen in different places and neither
 * belongs to any one stream.
 *
 * Whose bell depends on the role: a writer hangs on CL0 where a reader already
 * reads `tail`, a reader on CL1 where a writer already reads `head`.
 * ───────────────────────────────────────────────────────────────────── */
int brook_bell_hang(Brook *b, uint32_t strand_pid)
{
    if (!b || !b->hdr)    return -ERR_INVALID_ARGS;
    if (strand_pid == 0)  return -ERR_INVALID_ARGS;

    /* SEQ_CST, and the seq_cst is the whole point: it is a full barrier, so
     * this store cannot sit in the store buffer while the caller's next act —
     * its one last look — already executes. Pair it with the peer's locked
     * cursor publish and one of the two always sees the other. */
    __atomic_store_n(brook_own_bell(b->hdr, b->role), strand_pid, __ATOMIC_SEQ_CST);
    return 0;
}

int brook_bell_take(Brook *b)
{
    if (!b || !b->hdr) return -ERR_INVALID_ARGS;

    /* Awake again. Taking the bell in needs no ordering of its own — a peer
     * that reads a stale pid merely rings a strand that is already up, which
     * costs one empty message and wakes nobody twice. */
    __atomic_store_n(brook_own_bell(b->hdr, b->role), 0u, __ATOMIC_RELEASE);
    return 0;
}

uint32_t brook_frame_size(const Brook *b)
{
    return b ? b->frame_size : 0;
}

uint32_t brook_frame_count(const Brook *b)
{
    return b ? b->frame_count : 0;
}

uint64_t brook_handle_header_va(const Brook *b)
{
    return b ? b->va_header : 0;
}

uint32_t brook_available(const Brook *b)
{
    if (!b) return 0;
    uint64_t tail = brook_load_acquire_u64(&b->hdr->tail);
    uint64_t head = brook_load_relaxed_u64(&b->hdr->head);
    uint64_t n = tail - head;
    return n > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)n;
}

uint32_t brook_free(const Brook *b)
{
    if (!b) return 0;
    uint64_t head = brook_load_acquire_u64(&b->hdr->head);
    uint64_t tail = brook_load_relaxed_u64(&b->hdr->tail);
    uint64_t used = tail - head;
    uint32_t cap = b->frame_count;
    return used >= cap ? 0 : (uint32_t)(cap - used);
}

bool brook_writer_ever_attached(const Brook *b)
{
    if (!b) return false;
    return brook_load_acquire_u32(&b->hdr->writer_ever_attached) != 0;
}
