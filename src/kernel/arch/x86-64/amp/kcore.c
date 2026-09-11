#include "kcore.h"
#include "boardroom.h"
#include "xhci_hub.h"
#include "hardware_deck.h"
#include "xhci_msd.h"
#include "xhci_enumeration.h"
#include "process.h"
#include "guide.h"
#include "lapic.h"
#include "irqchip.h"
#include "atomics.h"
#include "klib.h"
#include "vmm.h"
#include "pmm.h"
#include "perf_trace.h"
#include "amp.h"
#include "error.h"
#include "kring.h"
#include "nightwatch.h"
#include "baton.h"
#include "tagfs.h"

KCorePocketQueue *g_kcore_queues = NULL;

void kcore_init(void)
{
    uint32_t n = g_amp.total_cores;
    size_t queue_bytes = sizeof(KCorePocketQueue) * n;
    size_t queue_pages = (queue_bytes + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    void  *queue_phys  = pmm_alloc_zero(queue_pages);
    g_kcore_queues = queue_phys ? (KCorePocketQueue *)vmm_phys_to_virt((uintptr_t)queue_phys)
                                : NULL;
    if (!g_kcore_queues) {
        kprintf("[KCORE] FATAL: cannot allocate queue array (%u cores, %zu pages)\n",
                n, queue_pages);
        while (1) { __asm__ volatile("cli; hlt"); }
    }
    for (uint32_t i = 0; i < n; i++) {
        g_kcore_queues[i].kcore_idx = (uint8_t)i;
    }
    debug_printf("[KCORE] %u K-Core queue(s) initialized (%u bytes)\n",
                 g_amp.k_count, (uint32_t)(sizeof(KCorePocketQueue) * n));
}

uint32_t kcore_queue_depth(uint8_t core_idx)
{
    if (!g_kcore_queues || core_idx >= g_amp.total_cores) return 0;
    return atomic_load_u32(&g_kcore_queues[core_idx].count);
}

bool kcore_is_serving(const struct process_t* proc)
{
    if (!g_kcore_queues || !proc) return false;
    for (uint8_t c = 0; c < g_amp.total_cores; c++) {
        if (!g_amp.cores[c].is_kcore) continue;
        if (__atomic_load_n(&g_kcore_queues[c].serving, __ATOMIC_ACQUIRE) == proc)
            return true;
    }
    return false;
}

static error_t kcore_queue_push(KCorePocketQueue* q, struct process_t* proc)
{
    if (!q || !proc)
        return ERR_INVALID_ARGUMENT;

    uint32_t old_tail, new_tail;
    uint32_t spins = 0;

    for (;;) {
        old_tail = atomic_load_u32(&q->tail);
        new_tail = (old_tail + 1) & KCORE_QUEUE_MASK;

        if (new_tail == atomic_load_u32(&q->head)) {
            return ERR_KCORE_QUEUE_FULL;
        }

        if (atomic_cas_u32(&q->tail, old_tail, new_tail)) {
            break;
        }

        uint32_t backoff = 1u << (spins < 4 ? spins : 4);
        for (uint32_t b = 0; b < backoff; b++) {
            cpu_pause();
        }
        spins++;
    }

    atomic_fetch_add_u32(&q->count, 1);
    __atomic_store_n(&q->slots[old_tail], proc, __ATOMIC_RELEASE);
    mfence();
    return OK;
}

static struct process_t* kcore_queue_pop(KCorePocketQueue* q)
{
    if (!q)
        return NULL;

    uint32_t h = q->head;
    struct process_t* proc = __atomic_load_n(&q->slots[h], __ATOMIC_ACQUIRE);
    if (!proc) return NULL;

    q->slots[h] = NULL;
    mfence();
    q->head = (h + 1) & KCORE_QUEUE_MASK;
    atomic_fetch_sub_u32(&q->count, 1);
    return proc;
}

static uint8_t kcore_find_least_loaded(void)
{
    uint8_t best_core = g_amp.bsp_index;
    uint32_t best_depth = UINT32_MAX;

    for (uint8_t i = 0; i < g_amp.total_cores; i++) {
        CoreDescriptor *c = &g_amp.cores[i];
        if (!c->is_kcore) continue;
        if (!amp_core_online(c)) continue;
        uint32_t depth = kcore_queue_depth(i);
        if (depth < best_depth) {
            best_depth = depth;
            best_core = i;
        }
    }
    return best_core;
}

error_t kcore_submit(struct process_t* proc)
{
    if (!proc)
        return ERR_INVALID_ARGUMENT;

    uint8_t target = kcore_find_least_loaded();
    uint8_t my_core = amp_get_core_index();

    error_t result = kcore_queue_push(&g_kcore_queues[target], proc);
    if (result == OK) {
        goto submitted;
    }

    for (uint8_t i = 0; i < g_amp.total_cores; i++) {
        if (!g_amp.cores[i].is_kcore || i == target) continue;
        result = kcore_queue_push(&g_kcore_queues[i], proc);
        if (result == OK) {
            target = i;
            goto submitted;
        }
    }

    kprintf("[KCORE] DEFECT: no queue slot for PID %u — dedup/capacity invariant broken\n",
            proc->pid);
    return ERR_KCORE_SUBMIT_FAILED;

submitted:
    const char* my_type = amp_is_kcore() ? "K" : (my_core == g_amp.bsp_index ? "BSP" : "A");
    debug_printf("[%s%u] KCORE submit PID %u -> K%u (depth=%u)\n",
                 my_type, my_core, proc->pid, target, kcore_queue_depth(target));

    lapic_send_ipi(g_amp.cores[target].lapic_id, IPI_WAKE_VECTOR);
    return OK;
}

static void kcore_process_entry(struct process_t* proc)
{
    if (!proc || proc->magic != PROCESS_MAGIC) {
        return;
    }

    process_ref_inc(proc);

    uint8_t core_idx = amp_get_core_index();
    KCorePocketQueue* q = &g_kcore_queues[core_idx];
    nightwatch_core_busy(core_idx);
    __atomic_store_n(&q->serving, proc, __ATOMIC_RELEASE);
    debug_printf("[K%u] Processing PID %u\n", core_idx, proc->pid);

    guide_process_one(proc);

    __atomic_store_n(&q->serving, NULL, __ATOMIC_RELEASE);
    mfence();
    atomic_store_u8(&proc->kcore_pending, 0);
    mfence();

    if (!KPocketIsEmpty(proc)) {
        if (atomic_cas_u8(&proc->kcore_pending, 0, 1)) {
            kcore_submit(proc);
        }
    }

    process_ref_dec(proc);
}

void kcore_run_loop(void)
{
    uint8_t my_idx = amp_get_core_index();
    KCorePocketQueue* q = &g_kcore_queues[my_idx];
    uint32_t loop_count = 0;

    kprintf("[KCORE] Core %u guide loop started\n", (uint32_t)my_idx);

    __asm__ volatile("sti");

    for (;;) {
        struct process_t* proc;
        while ((proc = kcore_queue_pop(q)) != NULL) {
            kcore_process_entry(proc);
        }

        BatonPump(my_idx);

        xhci_hub_service_if_pending();

        xhci_slot_service_if_pending();

        xhci_recover_if_needed();

        HardwareDeckUsbRecoverProof();

        xhci_msd_watchdog();

        BoardroomAttendIfPending();

        TagFSServiceIfPending();

        if ((++loop_count % 10) == 0) {
            process_reap_strands();
            process_cleanup_deferred();
        }

        nightwatch_core_idle(my_idx);

        __asm__ volatile("cli");
        if (kcore_queue_depth(my_idx) != 0 || BatonPending(my_idx)) {
            __asm__ volatile("sti");
        } else {
            __asm__ volatile("sti; hlt");
        }
    }
}