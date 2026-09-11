
#include "kring.h"
#include "chit.h"
#include "klib.h"
#include "vmm.h"
#include "pmm.h"
#include "process.h"
#include "nightwatch.h"
#include "result.h"
#include "kresult.h"
#include "atomics.h"
#include "error.h"
#include "boxos_magic.h"
#include "amp.h"
#include "lapic.h"
#include "irqchip.h"

void KRingPocketInitAt(PocketRing *hdr, uint64_t slots_base, uint32_t slot_count_max)
{
    if (!hdr) return;
    if (slots_base & (VMM_PAGE_SIZE - 1))
        panic("ring slots_base 0x%lx not page-aligned - straddle invariant broken",
              (unsigned long)slots_base);
    memset(hdr, 0, sizeof(*hdr));
    hdr->hdr.head           = 0;
    hdr->hdr.tail           = 0;
    hdr->hdr.slots_base     = slots_base;
    hdr->hdr.slot_size      = POCKET_SLOT_SIZE;
    hdr->hdr.slot_count_max = slot_count_max;
    hdr->hdr.magic          = POCKET_RING_MAGIC;
}

void KRingResultInitAt(ResultRing *hdr, uint64_t slots_base, uint32_t slot_count_max)
{
    if (!hdr) return;
    if (slots_base & (VMM_PAGE_SIZE - 1))
        panic("ring slots_base 0x%lx not page-aligned - straddle invariant broken",
              (unsigned long)slots_base);
    memset(hdr, 0, sizeof(*hdr));
    hdr->hdr.head           = 0;
    hdr->hdr.tail           = 0;
    hdr->hdr.slots_base     = slots_base;
    hdr->hdr.slot_size      = RESULT_SLOT_SIZE;
    hdr->hdr.slot_count_max = slot_count_max;
    hdr->hdr.magic          = RESULT_RING_MAGIC;
}

void KRingPocketInit(PocketRing *hdr)
{
    KRingPocketInitAt(hdr, CABIN_POCKET_SLOTS_BASE, (uint32_t)POCKET_RING_SLOT_MAX);
}

void KRingResultInit(ResultRing *hdr)
{
    KRingResultInitAt(hdr, CABIN_RESULT_SLOTS_BASE, (uint32_t)RESULT_RING_SLOT_MAX);
}


static PocketRing *kring_pocket_hdr(process_t *proc)
{
    if (!proc || !proc->cabin || !proc->pocket_ring_phys) return NULL;
    return (PocketRing *)vmm_phys_to_virt(proc->pocket_ring_phys);
}

bool KPocketIsEmpty(process_t *proc)
{
    PocketRing *r = kring_pocket_hdr(proc);
    if (!r) return true;
    uint64_t head = __atomic_load_n(&r->hdr.head, __ATOMIC_RELAXED);
    uint64_t tail = __atomic_load_n(&r->hdr.tail, __ATOMIC_ACQUIRE);
    return head == tail;
}

uint32_t KPocketCount(process_t *proc)
{
    PocketRing *r = kring_pocket_hdr(proc);
    if (!r) return 0;
    uint64_t head = __atomic_load_n(&r->hdr.head, __ATOMIC_RELAXED);
    uint64_t tail = __atomic_load_n(&r->hdr.tail, __ATOMIC_ACQUIRE);
    uint64_t n = tail - head;
    return n > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)n;
}

Pocket *KPocketPeek(process_t *proc, uint64_t *pos_out)
{
    PocketRing *r = kring_pocket_hdr(proc);
    if (!r) return NULL;
    uint64_t head = __atomic_load_n(&r->hdr.head, __ATOMIC_RELAXED);
    uint64_t tail = __atomic_load_n(&r->hdr.tail, __ATOMIC_ACQUIRE);
    if (head == tail) return NULL;
    if (pos_out) *pos_out = head;

    uintptr_t uvaddr = pocket_ring_slot_uvaddr(r, head);
    return (Pocket *)vmm_translate_user_addr(proc->cabin->vmm, uvaddr, sizeof(Pocket));
}

bool KPocketPopAt(process_t *proc, uint64_t pos)
{
    PocketRing *r = kring_pocket_hdr(proc);
    if (!r) return false;
    uint64_t expected = pos;
    return __atomic_compare_exchange_n(&r->hdr.head, &expected, pos + 1,
                                       false,
                                       __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
}


static ResultRing *kring_result_hdr(process_t *proc)
{
    if (!proc || !proc->cabin || !proc->result_ring_phys) return NULL;
    return (ResultRing *)vmm_phys_to_virt(proc->result_ring_phys);
}

static ResultSlot *kring_translate_slot(process_t *target, uintptr_t uvaddr)
{
    return (ResultSlot *)vmm_translate_user_addr(target->cabin->vmm, uvaddr,
                                                  sizeof(ResultSlot));
}

bool KResultRingHasPendingReply(process_t *proc)
{
    ResultRing *rr = kring_result_hdr(proc);
    if (!rr) return false;
    uint32_t cap = rr->hdr.slot_count_max;
    if (cap == 0) return false;

    uint64_t head = __atomic_load_n(&rr->hdr.head, __ATOMIC_ACQUIRE);
    uint64_t tail = __atomic_load_n(&rr->hdr.tail, __ATOMIC_ACQUIRE);
    if (head == tail) return false;
    uint64_t scan_end = tail;
    if (scan_end - head > cap) scan_end = head + cap;

    for (uint64_t pos = head; pos < scan_end; pos++) {
        uintptr_t   uva  = result_ring_slot_uvaddr(rr, pos);
        ResultSlot *slot = kring_translate_slot(proc, uva);
        if (!slot) continue;
        uint64_t round = pos / cap;
        if (__atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE) != 2u * round + 1u)
            continue;
        if (KCTX_COOKIE24(slot->r.context) != 0) return true;
    }
    return false;
}

bool KResultRingHasUnreadAtHead(process_t *proc)
{
    ResultRing *rr = kring_result_hdr(proc);
    if (!rr) return false;
    uint32_t cap = rr->hdr.slot_count_max;
    if (cap == 0) return false;

    uint64_t head = __atomic_load_n(&rr->hdr.head, __ATOMIC_ACQUIRE);
    uint64_t tail = __atomic_load_n(&rr->hdr.tail, __ATOMIC_ACQUIRE);
    if (head == tail) return false;

    ResultSlot *slot = kring_translate_slot(proc, result_ring_slot_uvaddr(rr, head));
    if (!slot) return false;
    return __atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE) == 2u * (head / cap) + 1u;
}

static inline void kring_wake_remote(process_t *target)
{
    if (!target) return;
    if (g_amp.total_cores <= 1) return;
    uint8_t core = target->home_core;
    if (core >= g_amp.total_cores) return;
    if (core == amp_get_core_index()) return;
    lapic_send_ipi(g_amp.cores[core].lapic_id, IPI_WAKE_VECTOR);
}

static volatile uint64_t g_krp_full;
static volatile uint64_t g_krp_seq_broken;
static volatile uint64_t g_krp_refused;
static volatile uint64_t g_krp_token_astray;

static void krp_refused(const process_t *target, const Result *r, const char *why)
{
    uint64_t n     = __atomic_fetch_add(&g_krp_refused, 1, __ATOMIC_RELAXED);
    uint32_t token = KCTX_COOKIE24(r->context);
    if (token != 0) {
        kprintf("[KRP] ERROR: dropped an ANSWER for pid %u (token %u): %s. The "
                "strand waiting on that answer will not be woken by it\n",
                (unsigned int)target->pid, (unsigned int)token, why);
    } else if ((n & 0x3Fu) == 0) {
        kprintf("[KRP] WARN: refused a message for pid %u: %s\n",
                (unsigned int)target->pid, why);
    }
}

bool KResultPush(process_t *target, const Result *r)
{
    nightwatch_core_busy(amp_get_core_index());

    if (!target || !r) return false;

    ResultRing *rr = kring_result_hdr(target);
    if (!rr) { krp_refused(target, r, "it has no reply ring"); return false; }

    uint32_t cap = rr->hdr.slot_count_max;
    if (cap == 0) { krp_refused(target, r, "its reply ring has no slots"); return false; }

    uint32_t limit = cap;
    if (KCTX_COOKIE24(r->context) == 0) {
        PocketRing *pr = kring_pocket_hdr(target);
        uint32_t reserve = pr ? pr->hdr.slot_count_max : (cap / 2u);
        limit = (cap > reserve) ? (cap - reserve) : (cap / 2u);
        if (limit == 0) limit = 1u;
    }

    ResultSlot *slot    = NULL;
    uintptr_t   ensured = 0;
    uint64_t    pos     = 0;

    for (;;) {
        uint64_t head = __atomic_load_n(&rr->hdr.head, __ATOMIC_ACQUIRE);
        pos           = __atomic_load_n(&rr->hdr.tail, __ATOMIC_RELAXED);
        if (pos - head >= limit) {
            uint64_t n = __atomic_fetch_add(&g_krp_full, 1, __ATOMIC_RELAXED);
            if (KCTX_COOKIE24(r->context) != 0 && (n & 0x3Fu) == 0) {
                kprintf("[KRP] ERROR: dropped an ANSWER for pid %u (token %u) — "
                        "reply ring full at %u of %u slots. The strand waiting "
                        "on that answer will not be woken by it\n",
                        (unsigned int)target->pid,
                        (unsigned int)KCTX_COOKIE24(r->context),
                        (unsigned int)(pos - head), (unsigned int)cap);
            }
            return false;
        }

        uintptr_t uvaddr = result_ring_slot_uvaddr(rr, pos);
        uintptr_t page   = uvaddr & ~(uintptr_t)(VMM_PAGE_SIZE - 1);
        if (page != ensured) {
            if (vmm_ensure_user_page(target->cabin->vmm, uvaddr,
                                     true) != 0) {
                krp_refused(target, r, "the slot's page could not be mapped");
                return false;
            }
            ensured = page;
        }

        slot = kring_translate_slot(target, uvaddr);
        if (!slot) { krp_refused(target, r, "the slot does not translate"); return false; }

        if (__atomic_compare_exchange_n(&rr->hdr.tail, &pos, pos + 1,
                                        true, __ATOMIC_ACQ_REL,
                                        __ATOMIC_RELAXED)) {
            break;
        }
    }

    uint64_t round    = pos / cap;
    uint64_t expected = 2u * round;

    if (__atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE) != expected) {
        uint64_t n = __atomic_fetch_add(&g_krp_seq_broken, 1, __ATOMIC_RELAXED);
        if ((n & 0x3FFu) == 0) {
            kprintf("[KRP] WARN: pid %u reported a head its consumer has not "
                    "reached (slot %lu of round %lu was not released); its own "
                    "reply stream is what gets overwritten\n",
                    (unsigned int)target->pid, (unsigned long)(pos % cap),
                    (unsigned long)round);
        }
    }

    slot->r = *r;

    {
        uint32_t token    = KCTX_COOKIE24(r->context);
        uint64_t awaiting = __atomic_load_n(&rr->hdr.awaiting, __ATOMIC_ACQUIRE);
        if (token != 0 && awaiting != 0 && awaiting != (uint64_t)token) {
            __atomic_fetch_add(&g_krp_token_astray, 1, __ATOMIC_RELAXED);
            kprintf("[KRP] DEFECT: pid %u waits on token 0x%06x, but the answer "
                    "being published for it carries token 0x%06x — its wait will "
                    "drop this reply as an orphan\n",
                    (unsigned int)target->pid, (unsigned int)awaiting, (unsigned int)token);
        }
    }

    __atomic_store_n(&slot->seq, expected + 1u, __ATOMIC_RELEASE);

    ChitKeep(target, KCTX_COOKIE24(r->context));

    if (KCTX_COOKIE24(r->context) != 0) {
        process_set_state(target, PROC_WORKING);
    } else if (process_get_state(target) == PROC_WAITING) {
        process_set_state(target, PROC_WORKING);
    }
    kring_wake_remote(target);
    return true;
}