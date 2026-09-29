/* kheap_charge.c — whom the kernel heap serves, and how much of it one class
 * of cabins may make it hold.
 *
 * ‼ A HEAP THAT GROWS WITH THE MACHINE MUST NOT BE GROWN BY ONE CABIN FOR IT.
 *
 * A heap of a fixed size was, by accident, the kernel's only defence against a
 * cabin that makes it hold things — tags interned by the thousand, files by
 * the million, names that never die: that cabin ran the heap out, and with it
 * everybody. Growing the heap to the size of the machine takes the accident
 * away, so the rule is said instead, once, here:
 *
 *     A CLASS OF CABINS MAY NOT HOLD MORE OF THE KERNEL HEAP THAN THE MACHINE
 *     HAS LEFT FREE ONCE IT IS SERVED.
 *
 * "Left free" is what anybody could still be served from: the page
 * allocator's free pages and what the heap's ordinary regions hold free — not
 * the reserve, which is nobody's to spend. So no class ever takes more than
 * half of what it could reach, whatever the machine is, and a class that
 * tries is refused out loud with its numbers, once per episode: the refusals
 * after the first are counted, and the count is said when the class is served
 * again. The kernel's own account is not a class and meets only the machine.
 *
 * Who is asking is a per-core scope (kheap.h). An interrupt taken inside a
 * scope is not the scope's business, and is charged to the kernel.
 */
#include "kheap_internal.h"
#include "slab.h"
#include "pmm.h"
#include "amp.h"
#include "idt.h"

typedef struct {
    KHeapScope scope;
} __attribute__((aligned(64))) KHeapCoreScope;

static KHeapCoreScope g_scopes[MAX_CORES];

/* Per class, a line of its own: what it holds outside the slab (pool blocks
 * as carved, large pieces as pages), its refusals ever, and those of the
 * episode standing now — 0 when it is being served. */
typedef struct {
    volatile uint64_t bytes;
    volatile uint64_t refused;
    volatile uint64_t episode;
} __attribute__((aligned(64))) KHeapChargeLine;

static KHeapChargeLine g_charges[KHEAP_CHARGE_COUNT];

static const char *const g_charge_names[KHEAP_CHARGE_COUNT] = {
    [KHEAP_CHARGE_KERNEL]  = "the kernel",
    [KHEAP_CHARGE_GOD]     = "god-class cabins",
    [KHEAP_CHARGE_SYSTEM]  = "system-class cabins",
    [KHEAP_CHARGE_UTILITY] = "utility-class cabins",
    [KHEAP_CHARGE_APP]     = "app-class cabins",
    [KHEAP_CHARGE_OTHER]   = "cabins with no authority key",
};

static inline KHeapCharge ChargeClamp(KHeapCharge charge)
{
    return (unsigned)charge < KHEAP_CHARGE_COUNT ? charge : KHEAP_CHARGE_KERNEL;
}

KHeapScope KHeapScopeEnter(KHeapCharge charge, uint32_t pid)
{
    KHeapScope before = { KHEAP_CHARGE_KERNEL, 0, 0 };
    uint8_t    core   = amp_get_core_index();
    if (core >= MAX_CORES)
        return before;

    before = g_scopes[core].scope;
    g_scopes[core].scope = (KHeapScope){ (uint8_t)ChargeClamp(charge), 0, pid };
    asm volatile("" ::: "memory");
    return before;
}

void KHeapScopeLeave(KHeapScope before)
{
    uint8_t core = amp_get_core_index();
    if (core >= MAX_CORES)
        return;
    asm volatile("" ::: "memory");
    g_scopes[core].scope = before;
}

void KHeapReserveOpen(void)
{
    uint8_t core = amp_get_core_index();
    if (core >= MAX_CORES)
        return;
    g_scopes[core].scope.reserve++;
    asm volatile("" ::: "memory");
}

void KHeapReserveClose(void)
{
    uint8_t core = amp_get_core_index();
    if (core >= MAX_CORES)
        return;
    asm volatile("" ::: "memory");
    if (g_scopes[core].scope.reserve)
        g_scopes[core].scope.reserve--;
}

KHeapScope KHeapScopeHere(void)
{
    KHeapScope kernel = { KHEAP_CHARGE_KERNEL, 0, 0 };
    if (IrqFrameInFlight() != NULL)
        return kernel;
    uint8_t core = amp_get_core_index();
    if (core >= MAX_CORES)
        return kernel;
    return g_scopes[core].scope;
}

void KHeapChargeAdd(KHeapCharge charge, size_t bytes)
{
    __atomic_add_fetch(&g_charges[ChargeClamp(charge)].bytes, bytes, __ATOMIC_RELAXED);
}

void KHeapChargeSub(KHeapCharge charge, size_t bytes)
{
    __atomic_sub_fetch(&g_charges[ChargeClamp(charge)].bytes, bytes, __ATOMIC_RELAXED);
}

size_t KHeapChargeHeld(KHeapCharge charge)
{
    charge = ChargeClamp(charge);
    return (size_t)__atomic_load_n(&g_charges[charge].bytes, __ATOMIC_RELAXED) +
           SlabChargeBytes(charge);
}

uint64_t KHeapChargeRefusals(KHeapCharge charge)
{
    return __atomic_load_n(&g_charges[ChargeClamp(charge)].refused, __ATOMIC_ACQUIRE);
}

bool KHeapChargeAdmits(KHeapCharge charge, uint32_t pid, size_t bytes)
{
    charge = ChargeClamp(charge);
    if (charge == KHEAP_CHARGE_KERNEL)
        return true;

    KHeapChargeLine *line = &g_charges[charge];
    size_t held = KHeapChargeHeld(charge);
    size_t free = PmmFreePagesNow() * PMM_PAGE_SIZE + KHeapRegionSlack();

    /* Held after, against left free after: held + bytes <= free - bytes. */
    if (bytes <= free / 2 && held <= free - 2 * bytes) {
        if (__atomic_load_n(&line->episode, __ATOMIC_RELAXED)) {
            uint64_t refused = __atomic_exchange_n(&line->episode, 0, __ATOMIC_ACQ_REL);
            if (refused)
                kprintf("[KLIB] %s are served again, after %llu refused allocation(s)\n",
                        g_charge_names[charge], (unsigned long long)refused);
        }
        return true;
    }

    __atomic_add_fetch(&line->refused, 1, __ATOMIC_RELEASE);
    if (__atomic_fetch_add(&line->episode, 1, __ATOMIC_ACQ_REL) == 0)
        kprintf("[KLIB] refused %zu bytes to %s (pid %u): they make the kernel hold "
                "%zu KB already, and the machine has %zu KB left — no class may hold "
                "more than it leaves free; until one of theirs is served again, their "
                "refusals are counted, not said\n",
                bytes, g_charge_names[charge], pid, held / 1024, free / 1024);
    return false;
}
