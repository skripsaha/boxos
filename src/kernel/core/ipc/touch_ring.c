
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


static TouchRing *ktr_hdr(process_t *proc)
{
    if (!proc || !proc->cabin || !proc->touch_ring_phys) return NULL;
    return (TouchRing *)vmm_phys_to_virt(proc->touch_ring_phys);
}

static TouchSlot *ktr_translate_slot(process_t *target, uintptr_t uvaddr)
{
    return (TouchSlot *)vmm_translate_user_addr(target->cabin->vmm, uvaddr,
                                                sizeof(TouchSlot));
}

bool KTouchRingHasUnreadAtHead(process_t *proc)
{
    TouchRing *rr = ktr_hdr(proc);
    if (!rr) return false;
    uint32_t cap = rr->hdr.slot_count_max;
    if (cap == 0) return false;

    uint64_t head = __atomic_load_n(&rr->hdr.head, __ATOMIC_ACQUIRE);
    uint64_t tail = __atomic_load_n(&rr->hdr.tail, __ATOMIC_ACQUIRE);
    if (head == tail) return false;

    TouchSlot *slot = ktr_translate_slot(proc, touch_ring_slot_uvaddr(rr, head));
    if (!slot) return false;
    return __atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE) == 2u * (head / cap) + 1u;
}

static inline void ktr_wake_remote(process_t *target)
{
    if (!target) return;
    if (g_amp.total_cores <= 1) return;
    uint8_t core = target->home_core;
    if (core >= g_amp.total_cores) return;
    if (core == amp_get_core_index()) return;
    lapic_send_ipi(g_amp.cores[core].lapic_id, IPI_WAKE_VECTOR);
}


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

    if (payload_len > BOXOS_TOUCH_PAYLOAD_MAX) {
        payload_len = BOXOS_TOUCH_PAYLOAD_MAX;
    }

    TouchSlot *slot     = NULL;
    uintptr_t  ensured  = 0;
    uint32_t   turns    = 0;
    uint64_t   pos      = 0;

    for (;;) {
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
                                     true) != 0) {
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
                                        true, __ATOMIC_ACQ_REL,
                                        __ATOMIC_RELAXED)) {
            break;
        }
        if (++turns == 1) atomic_fetch_add_u64(&g_ktr_contended, 1);
    }

    uint64_t round    = pos / cap;
    uint64_t expected = 2u * round;

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

    __atomic_store_n(&slot->seq, expected + 1u, __ATOMIC_RELEASE);
    atomic_fetch_add_u64(&g_ktr_success, 1);

    if (process_get_state(target) == PROC_WAITING) {
        process_set_state(target, PROC_WORKING);
    }
    ktr_wake_remote(target);
    return true;
}