
#include "box/core/strand_self.h"
#include "box/cpu.h"
#include "box/core/cabin.h"

INLINE uint64_t strand_read_fsbase(void)
{
    uint64_t v;
    __asm__ volatile("rdfsbase %0" : "=r"(v));
    return v;
}

static int g_fsgsbase_cached = -1;

StrandInfo *strand_info_or_null(void)
{
    int has = __atomic_load_n(&g_fsgsbase_cached, __ATOMIC_RELAXED);
    if (has < 0) {
        has = cpu_has_fsgsbase() ? 1 : 0;
        __atomic_store_n(&g_fsgsbase_cached, has, __ATOMIC_RELAXED);
    }
    if (!has)
        return NULL;

    uint64_t fsb = strand_read_fsbase();

    if (fsb < CABIN_HAMMOCK_BASE || fsb >= CABIN_HAMMOCK_END)
        return NULL;

    StrandInfo *si = (StrandInfo *)(uintptr_t)fsb;
    if (si->magic != STRAND_INFO_MAGIC)
        return NULL;

    return si;
}

strand_rings_t strand_rings(void)
{
    StrandInfo *si = strand_info_or_null();
    strand_rings_t r;
    if (si)
    {
        r.pocket_va = si->pocket_ring_va;
        r.result_va = si->result_ring_va;
        r.touch_va  = si->touch_ring_va;
    }
    else
    {
        r.pocket_va = POCKET_RING_VADDR;
        r.result_va = RESULT_RING_VADDR;
        r.touch_va  = TOUCH_RING_VADDR;
    }
    return r;
}

uint32_t strand_self(void)
{
    StrandInfo *si = strand_info_or_null();
    if (si)
        return si->strand_pid;
    return cabin_info()->pid;
}

uint32_t strand_self_generation(void)
{
    StrandInfo *si = strand_info_or_null();
    return si ? si->generation : cabin_info()->generation;
}