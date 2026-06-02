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

void KTouchRingInit(TouchRing *hdr)
{
    if (!hdr) return;
    memset(hdr, 0, sizeof(*hdr));
    hdr->hdr.head           = 0;
    hdr->hdr.tail           = 0;
    hdr->hdr.slots_base     = CABIN_TOUCH_SLOTS_BASE;
    hdr->hdr.slot_size      = TOUCH_SLOT_SIZE;
    hdr->hdr.slot_count_max = (uint32_t)TOUCH_RING_SLOT_MAX;
    hdr->hdr.magic          = TOUCH_RING_MAGIC;
}

/* ------------------------------------------------------------------------
 * Header / slot translation helpers
 * ------------------------------------------------------------------------ */

static TouchRing *ktr_hdr(process_t *proc)
{
    if (!proc || !proc->touch_ring_phys) return NULL;
    return (TouchRing *)vmm_phys_to_virt(proc->touch_ring_phys);
}

static TouchSlot *ktr_translate_slot(process_t *target, uintptr_t uvaddr)
{
    return (TouchSlot *)vmm_translate_user_addr(target->cabin, uvaddr,
                                                sizeof(TouchSlot));
}

/* ------------------------------------------------------------------------
 * Per-return-path counters (snapshot via KTouchPushStats).
 * ------------------------------------------------------------------------ */

static volatile uint64_t g_ktr_reject;
static volatile uint64_t g_ktr_pre_full;
static volatile uint64_t g_ktr_map_fail;
static volatile uint64_t g_ktr_translate_fail;
static volatile uint64_t g_ktr_spin_limit;
static volatile uint64_t g_ktr_overflow;
static volatile uint64_t g_ktr_success;

void KTouchPushStats(uint64_t out[7])
{
    out[0] = __atomic_load_n(&g_ktr_reject,         __ATOMIC_RELAXED);
    out[1] = __atomic_load_n(&g_ktr_pre_full,       __ATOMIC_RELAXED);
    out[2] = __atomic_load_n(&g_ktr_map_fail,       __ATOMIC_RELAXED);
    out[3] = __atomic_load_n(&g_ktr_translate_fail, __ATOMIC_RELAXED);
    out[4] = __atomic_load_n(&g_ktr_spin_limit,     __ATOMIC_RELAXED);
    out[5] = __atomic_load_n(&g_ktr_overflow,       __ATOMIC_RELAXED);
    out[6] = __atomic_load_n(&g_ktr_success,        __ATOMIC_RELAXED);
}

/* ------------------------------------------------------------------------
 * KTouchPush — MPSC producer
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

    /* Defensive clamp: API contract says caller should keep payload at
     * or below BOXOS_TOUCH_PAYLOAD_MAX, but truncating here keeps the
     * slot copy bounded even if a caller drifts. */
    if (payload_len > BOXOS_TOUCH_PAYLOAD_MAX) {
        payload_len = BOXOS_TOUCH_PAYLOAD_MAX;
    }

    /* (1) Cheap fullness pre-check. ACQUIRE on head so we see the
     *     consumer's most recent advance. */
    uint64_t head      = __atomic_load_n(&rr->hdr.head, __ATOMIC_ACQUIRE);
    uint64_t tail_snap = __atomic_load_n(&rr->hdr.tail, __ATOMIC_RELAXED);
    if ((tail_snap - head) >= cap) {
        atomic_fetch_add_u64(&g_ktr_pre_full, 1);
        return false;
    }

    /* (2) Pre-map BOTH the snapshot's slot page AND the next 4 KiB page.
     *
     *     Rationale identical to KResultPush: concurrent producers can
     *     push our reserved `pos` (after fetch_add at step (3)) onto
     *     the next page. Pre-mapping both before the reservation moves
     *     any allocation failure BEFORE the state change, so a failure
     *     leaves the ring consistent and we just return false.
     *
     *     With 128 B TouchSlot stride and 4 KiB pages there are 32
     *     slots per page; concurrent producers cannot overshoot
     *     `uvaddr_pre + 4096` within one fetch_add window for any
     *     realistic SMP (max ~64 K-Cores). */
    uintptr_t uvaddr_pre     = touch_ring_slot_uvaddr(rr, tail_snap);
    uintptr_t one_page_ahead = uvaddr_pre + 4096u;
    if (vmm_ensure_user_page(target->cabin, uvaddr_pre, /*writable=*/true) != 0) {
        atomic_fetch_add_u64(&g_ktr_map_fail, 1);
        return false;
    }
    (void)vmm_ensure_user_page(target->cabin, one_page_ahead, /*writable=*/true);

    /* (3) Atomic reservation — MPSC linearisation point. Use ACQ_REL so
     *     all writes to the slot that follow are ordered AFTER this
     *     fetch (release semantic on store), and we see prior consumer
     *     advances (acquire semantic on load). */
    uint64_t pos = __atomic_fetch_add(&rr->hdr.tail, 1, __ATOMIC_ACQ_REL);

    /* (4) Re-check fullness against our reserved pos. Concurrent
     *     reservations may have pushed us past capacity; publish a
     *     synthetic overflow marker (payload_len=0) so the consumer
     *     advances seq instead of waiting forever on a never-arriving
     *     payload. */
    bool overflow = (pos - head) >= cap;

    /* (5) Cross-page case — same handling as KResultPush. */
    uintptr_t uvaddr = touch_ring_slot_uvaddr(rr, pos);
    if (uvaddr != uvaddr_pre && uvaddr != one_page_ahead) {
        if (vmm_ensure_user_page(target->cabin, uvaddr, /*writable=*/true) != 0) {
            atomic_fetch_add_u64(&g_ktr_map_fail, 1);
            kprintf("[KTR] WARN: cross-page map failed at pos=%lu pid=%u — "
                    "publishing synthetic overflow slot\n",
                    (unsigned long)pos, (unsigned int)target->pid);
            overflow = true;
        }
    }

    TouchSlot *slot = ktr_translate_slot(target, uvaddr);
    if (!slot) {
        /* Stranded-slot window — same residual rare case as KResultPush
         * (vmm_ensure_user_page succeeded but translate failed). The
         * consumer treats the stuck seq as "empty and waiting", which
         * blocks this single slot until process exit. Real risk only
         * during concurrent process_destroy; logged for diagnosis. */
        atomic_fetch_add_u64(&g_ktr_translate_fail, 1);
        kprintf("[KTR] WARN: translate failed pos=%lu pid=%u — slot stuck\n",
                (unsigned long)pos, (unsigned int)target->pid);
        return false;
    }

    /* (6) Vyukov gate — bounded spin with destroying-probe. Identical
     *     contract to KResultPush: NEVER abandon a reserved slot, but
     *     ALWAYS exit eventually so a frozen/crashed consumer can't
     *     wedge every producer K-Core. */
    uint64_t round    = pos / cap;
    uint64_t expected = 2u * round;

    enum { KTR_SPIN_BUDGET = 1u << 20 };
    enum { KTR_DESTROYING_PROBE_MASK = 0xFFFFu };
    bool consumer_lost = false;
    for (uint64_t spins = 0; ; spins++) {
        uint64_t seq = __atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE);
        if (seq == expected) break;
        if (spins >= KTR_SPIN_BUDGET) {
            atomic_fetch_add_u64(&g_ktr_spin_limit, 1);
            kprintf("[KTR] WARN: spin budget exhausted pid=%u pos=%lu seq=%lu expected=%lu — publishing synthetic ERR\n",
                    (unsigned int)target->pid, (unsigned long)pos,
                    (unsigned long)seq, (unsigned long)expected);
            consumer_lost = true;
            break;
        }
        if ((spins & KTR_DESTROYING_PROBE_MASK) == 0 &&
            __atomic_load_n(&target->destroying, __ATOMIC_ACQUIRE)) {
            atomic_fetch_add_u64(&g_ktr_spin_limit, 1);
            consumer_lost = true;
            break;
        }
        cpu_pause();
    }

    /* (7) Write the slot. On overflow/consumer-lost we still write a
     *     valid slot so the seq advance at step (8) gives the consumer
     *     something benign to drain. Use payload_len=0 + tag_id=0 as
     *     the overflow marker; consumers can check payload_len/flags
     *     to filter or simply ignore (the slot still increments their
     *     position correctly). */
    if (overflow || consumer_lost) {
        slot->tag_id        = 0;
        slot->flags         = 0;
        slot->source_pid    = 0;
        slot->payload_len   = 0;
        slot->_reserved     = 0;
        slot->timestamp_tsc = rdtsc();
        /* payload[] contents irrelevant on overflow; leave as-is. */
        atomic_fetch_add_u64(&g_ktr_overflow, 1);
    } else {
        slot->tag_id        = tag_id;
        slot->flags         = flags;
        slot->source_pid    = source_pid;
        slot->payload_len   = payload_len;
        slot->_reserved     = 0;
        slot->timestamp_tsc = rdtsc();
        if (payload_len > 0 && payload) {
            memcpy(slot->payload, payload, payload_len);
        }
        atomic_fetch_add_u64(&g_ktr_success, 1);
    }

    /* (8) Publish — release-store the slot's seq so the consumer's
     *     ACQUIRE-load sees all of: tag_id / flags / source_pid /
     *     payload_len / timestamp_tsc / payload[0..payload_len). */
    __atomic_store_n(&slot->seq, expected + 1u, __ATOMIC_RELEASE);

    /* (9) Wake target if it was sleeping. Skip on consumer_lost — cabin
     *     is destroying or unresponsive, one more wake won't help. */
    if (!consumer_lost && process_get_state(target) == PROC_WAITING) {
        process_set_state(target, PROC_WORKING);
    }
    return !overflow && !consumer_lost;
}
