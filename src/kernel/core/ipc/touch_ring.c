/*
 * touch_ring.c — TouchRing MPSC producer implementation.
 *
 * Owns the kernel side of the kernel→userspace Touch event channel.
 * Mirrors KResultPush's Vyukov-MPSC algorithm with two key differences:
 *
 *   1. Slot is 128 B and self-contained — metadata header + inline
 *      payload + seq gate. No `data_addr` indirection, no separate
 *      payload allocator. Producer writes both the metadata and the
 *      payload bytes BEFORE releasing seq, and the consumer must read
 *      both BEFORE releasing seq on its side. That ties payload
 *      lifetime to the slot's Vyukov round and eliminates the
 *      buf_heap_next leak the multiplexed design suffered from.
 *
 *   2. There is no concept of `sender_pid != 0` IPC-routing or KCTX
 *      fan-out for TouchRing — every entry is a kernel-published
 *      Touch with a tag_id, payload, and (optionally) the source pid
 *      that triggered the publish (0 = pure-kernel publisher).
 *
 * Cabin teardown / process_destroy
 * --------------------------------
 * KTouchPush's caller must hold a process reference on `target` for the
 * duration of the call (same contract as KResultPush). Inside, we still
 * probe `target->destroying` periodically during the Vyukov gate spin so
 * a teardown that started after the caller's ref_inc cannot trap us in
 * an unbounded wait.
 */

#include "touch_ring.h"
#include "klib.h"
#include "vmm.h"
#include "pmm.h"
#include "process.h"
#include "atomics.h"
#include "boxos_magic.h"
#include "amp.h"
#include "lapic.h"
#include "irqchip.h"

void KTouchRingInitAt(TouchRing *hdr, uint64_t slots_base, uint32_t slot_count_max)
{
    if (!hdr) return;
    /* Straddle invariant: the per-strand Hammock slots_base (runtime VA, not
     * the page-aligned cabin_layout.h constant) must be page-aligned or a
     * 128-byte TouchSlot translation could cross a page boundary. */
    if (slots_base & (VMM_PAGE_SIZE - 1))
        panic("ring slots_base 0x%lx not page-aligned - straddle invariant broken",
              (unsigned long)slots_base);
    memset(hdr, 0, sizeof(*hdr));
    hdr->hdr.head           = 0;
    hdr->hdr.tail           = 0;
    hdr->hdr.slots_base     = slots_base;
    hdr->hdr.slot_size      = TOUCH_SLOT_SIZE;
    hdr->hdr.slot_count_max = slot_count_max;
    hdr->hdr.magic          = TOUCH_RING_MAGIC;
}

void KTouchRingInit(TouchRing *hdr)
{
    KTouchRingInitAt(hdr, CABIN_TOUCH_SLOTS_BASE, (uint32_t)TOUCH_RING_SLOT_MAX);
}

/* ------------------------------------------------------------------------
 * Header / slot translation helpers
 * ------------------------------------------------------------------------ */

static TouchRing *ktr_hdr(process_t *proc)
{
    /* P5a: per-strand TouchRing (proc->touch_ring_phys); main strand aliases
     * the cabin ring, spawned strand uses its own Hammock-carved ring. Cabin
     * guard stays for the downstream proc->cabin->vmm deref. */
    if (!proc || !proc->cabin || !proc->touch_ring_phys) return NULL;
    return (TouchRing *)vmm_phys_to_virt(proc->touch_ring_phys);
}

static TouchSlot *ktr_translate_slot(process_t *target, uintptr_t uvaddr)
{
    return (TouchSlot *)vmm_translate_user_addr(target->cabin->vmm, uvaddr,
                                                sizeof(TouchSlot));
}

/* ------------------------------------------------------------------------
 * Cross-core wake helper — mirrors kring_wake_remote and touch_wake_remote.
 *
 * Real-HW rationale (identical to KResultPush M3 wake): the home_core
 * of `target` may sit in HLT / MWAIT / UMWAIT idle when we publish a
 * Touch slot. Without an explicit IPI, the consumer only resumes on the
 * next LAPIC timer tick (≤ 1 ms at 1 kHz; longer on deep C-states).
 * TouchRestDeliver in touch.c also calls touch_wake_remote at its outer
 * retry layer — that stays as the fallback. The inline wake here covers
 * the common single-attempt fast path where the outer wake would only
 * arrive AFTER the retry loop completes (~ms later on a contested ring).
 *
 * Defensive guards: skip on null/single-core/out-of-range/self — same
 * shape as kring_wake_remote in kring.c. ------------------------- */
static inline void ktr_wake_remote(process_t *target)
{
    if (!target) return;
    if (g_amp.total_cores <= 1) return;
    uint8_t core = target->home_core;
    if (core >= g_amp.total_cores) return;
    if (core == amp_get_core_index()) return;
    lapic_send_ipi(g_amp.cores[core].lapic_id, IPI_WAKE_VECTOR);
}

/* ------------------------------------------------------------------------
 * Per-return-path counters (snapshot via KTouchPushStats).
 * ------------------------------------------------------------------------ */

static volatile uint64_t g_ktr_reject;
static volatile uint64_t g_ktr_full;
static volatile uint64_t g_ktr_map_fail;
static volatile uint64_t g_ktr_translate_fail;
static volatile uint64_t g_ktr_contended;
static volatile uint64_t g_ktr_seq_broken;
static volatile uint64_t g_ktr_success;

void KTouchPushStats(uint64_t out[7])
{
    out[0] = __atomic_load_n(&g_ktr_reject,         __ATOMIC_RELAXED);
    out[1] = __atomic_load_n(&g_ktr_full,           __ATOMIC_RELAXED);
    out[2] = __atomic_load_n(&g_ktr_map_fail,       __ATOMIC_RELAXED);
    out[3] = __atomic_load_n(&g_ktr_translate_fail, __ATOMIC_RELAXED);
    out[4] = __atomic_load_n(&g_ktr_contended,      __ATOMIC_RELAXED);
    out[5] = __atomic_load_n(&g_ktr_seq_broken,     __ATOMIC_RELAXED);
    out[6] = __atomic_load_n(&g_ktr_success,        __ATOMIC_RELAXED);
}

/* ------------------------------------------------------------------------
 * KTouchPush — MPSC producer
 *
 * THE RESERVATION IS THE WHOLE DESIGN, AND IT WAITS FOR NOTHING.
 *
 * A producer used to claim its position with an unconditional fetch_add and
 * then repair the claim afterwards: re-check whether the position had gone
 * past the consumer, and wait — up to a guessed budget of 16384 PAUSEs — for
 * the slot's Vyukov gate to open. Past the guess it declared the consumer lost
 * and published a synthetic empty slot in place of the caller's event. Every
 * part of that was a consequence of the first line: a claim taken before it
 * was known to be valid can only be repaired by waiting, and a wait needs a
 * number nobody can derive. On a busy 16-core TCG host that number is a lie in
 * both directions — long enough to stall a K-Core for half a millisecond, and
 * short enough to throw away a live event because a vCPU was descheduled.
 *
 * The claim is now taken only when it is provably good, by CAS on the tail:
 *
 *   THE CONSUMER RELEASES A SLOT'S seq BEFORE IT ADVANCES head.
 *   (touch_ring.c's userspace mirror: `slot->seq = expected+1` RELEASE, then
 *    `hdr.head = pos+1` RELEASE — in that order, always.)
 *
 * So every position in [head, head + cap) names a slot whose previous round
 * the consumer has already finished with. A producer that only ever claims a
 * position inside that window claims a slot that is ALREADY free, and there is
 * nothing left to wait for: no gate spin, no budget, no destroying-probe woven
 * through it, no synthetic slot, no "consumer lost".
 *
 * Every way this can fail — full ring, no memory for the page, no translation
 * — now happens BEFORE the claim, so a claimed position is always filled and
 * the consumer can never meet a hole that stalls it forever. The old code's
 * one residual "slot stuck until process exit" window is gone with it.
 *
 * A full ring is answered honestly, at once, with false. That is not a loss:
 * TouchRestDeliver holds the event on the target's Owed queue (touch.c) and
 * hands it over at the strand's next syscall.
 *
 * `head` lives in a page the guest can write. A guest that reports a head it
 * has not reached can make the kernel reuse a slot it is still reading — and
 * corrupt its OWN event stream, in its OWN page, which is the only thing it
 * can reach. It is counted and named (g_ktr_seq_broken) rather than defended
 * against with a wait: the previous code's answer to the same lie was to stall
 * for half a millisecond and then corrupt the stream anyway.
 * ------------------------------------------------------------------------ */

bool KTouchPush(process_t *target,
                uint16_t tag_id, uint16_t flags, uint32_t source_pid,
                const void *payload, uint32_t payload_len)
{
    if (!target) {
        atomic_fetch_add_u64(&g_ktr_reject, 1);
        return false;
    }

    TouchRing *rr = ktr_hdr(target);
    if (!rr) {
        atomic_fetch_add_u64(&g_ktr_reject, 1);
        return false;
    }

    uint32_t cap = rr->hdr.slot_count_max;
    if (cap == 0) {
        atomic_fetch_add_u64(&g_ktr_reject, 1);
        return false;
    }

    /* Defensive clamp: the API contract says callers keep payloads at or below
     * BOXOS_TOUCH_PAYLOAD_MAX; truncating here keeps the slot copy bounded even
     * if a caller drifts. */
    if (payload_len > BOXOS_TOUCH_PAYLOAD_MAX) {
        payload_len = BOXOS_TOUCH_PAYLOAD_MAX;
    }

    /* ── Claim a position, or refuse. Nothing here waits on the consumer. ──
     *
     * The loop retries only when ANOTHER PRODUCER won the tail, which means
     * the ring made progress — so it is lock-free, not a spin on someone
     * else's liveness. `pos` is refreshed by the failing compare-exchange
     * itself, so each turn considers the position that actually came free.
     *
     * The page is mapped before the claim, and remembered across turns: a
     * retry usually lands on the same 4 KiB page (32 slots), so the common
     * contended path costs one compare, not a page walk. */
    TouchSlot *slot     = NULL;
    uintptr_t  ensured  = 0;   /* page whose mapping this call has secured */
    uint32_t   turns    = 0;
    uint64_t   pos      = 0;

    for (;;) {
        /* head BEFORE tail, and never the other way round.
         *
         * Both cursors only ever grow and tail >= head is the ring's own
         * invariant — but that is a statement about the ring at one instant,
         * not about two loads taken at two. Read tail first and the consumer
         * can advance head past it in between; the snapshot then has head >
         * pos, `pos - head` wraps to a colossal unsigned number, and the ring
         * declares itself full when it is in fact empty. Reading head FIRST
         * makes the order do the work: tail is sampled later, so it cannot be
         * behind the head we already have.
         *
         * MEASURED, not reasoned into place afterwards: with the loads the
         * other way round this refused an ANSWER once in a two-hour matrix,
         * reporting "full at 4294967295 of 32768 slots", and the strand
         * waiting on that answer never woke. */
        uint64_t head = __atomic_load_n(&rr->hdr.head, __ATOMIC_ACQUIRE);
        pos           = __atomic_load_n(&rr->hdr.tail, __ATOMIC_RELAXED);
        if (pos - head >= cap) {
            atomic_fetch_add_u64(&g_ktr_full, 1);
            return false;
        }

        uintptr_t uvaddr = touch_ring_slot_uvaddr(rr, pos);
        uintptr_t page   = uvaddr & ~(uintptr_t)(VMM_PAGE_SIZE - 1);
        if (page != ensured) {
            if (vmm_ensure_user_page(target->cabin->vmm, uvaddr,
                                     /*writable=*/true) != 0) {
                atomic_fetch_add_u64(&g_ktr_map_fail, 1);
                return false;
            }
            ensured = page;
        }

        slot = ktr_translate_slot(target, uvaddr);
        if (!slot) {
            atomic_fetch_add_u64(&g_ktr_translate_fail, 1);
            return false;
        }

        if (__atomic_compare_exchange_n(&rr->hdr.tail, &pos, pos + 1,
                                        /*weak=*/true, __ATOMIC_ACQ_REL,
                                        __ATOMIC_RELAXED)) {
            break;   /* `pos` is ours, and `slot` is the slot it names */
        }
        if (++turns == 1) atomic_fetch_add_u64(&g_ktr_contended, 1);
    }

    uint64_t round    = pos / cap;
    uint64_t expected = 2u * round;

    /* The claim's own guarantee, read back once. It costs one ACQUIRE load and
     * it is the only thing that can tell a broken consumer from a healthy one
     * — so it is read, counted and said out loud, never waited on. */
    if (__atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE) != expected) {
        uint64_t n = atomic_fetch_add_u64(&g_ktr_seq_broken, 1);
        if ((n & 0x3FFu) == 0) {
            kprintf("[KTR] WARN: pid %u reported a head its consumer has not "
                    "reached (slot %lu of round %lu was not released); its own "
                    "event stream is what gets overwritten\n",
                    (unsigned)target->pid, (unsigned long)(pos % cap),
                    (unsigned long)round);
        }
    }

    slot->tag_id        = tag_id;
    slot->flags         = flags;
    slot->source_pid    = source_pid;
    slot->payload_len   = payload_len;
    slot->_reserved     = 0;
    slot->timestamp_tsc = rdtsc();
    if (payload_len > 0 && payload) {
        memcpy(slot->payload, payload, payload_len);
    }

    /* Publish — the RELEASE store makes every field above visible to the
     * consumer's ACQUIRE load of the same word. */
    __atomic_store_n(&slot->seq, expected + 1u, __ATOMIC_RELEASE);
    atomic_fetch_add_u64(&g_ktr_success, 1);

    /* Wake. The home_core may be HLT/MWAIT-idle and would otherwise not learn
     * of the new slot until the next LAPIC tick; the IPI is the doorbell. */
    if (process_get_state(target) == PROC_WAITING) {
        process_set_state(target, PROC_WORKING);
    }
    ktr_wake_remote(target);
    return true;
}
