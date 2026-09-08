/*
 * strand_self.c — userspace per-strand identity + ring routing (P5a).
 *
 * See box/core/strand_self.h. The discriminator between a spawned strand and
 * the main strand is the FS base: a spawned strand's FS base is its StrandInfo,
 * which the kernel ALWAYS places inside the per-cabin Hammock window. The main
 * strand's FS base is either 0 (a plain C program) or its C++ TCB on the heap
 * (a boxcxx program) — both outside the Hammock window. Testing the range
 * first means we never dereference an unknown base (no read of a C++ TCB), so
 * the probe is airtight regardless of TCB size or layout.
 */

#include "box/core/strand_self.h"
#include "box/cpu.h"          /* cpu_has_fsgsbase */
#include "box/core/cabin.h"   /* cabin_info */

INLINE uint64_t strand_read_fsbase(void)
{
    uint64_t v;
    __asm__ volatile("rdfsbase %0" : "=r"(v));
    return v;
}

/* FSGSBASE availability is a machine-uniform constant (CR4.FSGSBASE is enabled
 * on every online core, or on none — see the kernel per_core init). Cache it
 * process-wide so the hot ring accessors (pocket_ring/result_ring/touch_ring,
 * which call strand_rings() → here on every access) don't re-read the CPU_CAPS
 * page each time. -1 = unresolved, 0/1 = result; the first-call write is
 * idempotent (always the same value) so the benign cross-strand race is safe.
 * The PER-STRAND part below (the FS base read + Hammock-range test) genuinely
 * CANNOT be cached in a process-wide static — every strand in the cabin shares
 * this code and its statics — so it stays per-call: a register read plus two
 * compares, which is the minimum needed to tell strands apart. */
static int g_fsgsbase_cached = -1;

StrandInfo *strand_info_or_null(void)
{
    /* Spawned strands REQUIRE FSGSBASE; without it RDFSBASE would #UD and we
     * are necessarily the main strand. */
    int has = __atomic_load_n(&g_fsgsbase_cached, __ATOMIC_RELAXED);
    if (has < 0) {
        has = cpu_has_fsgsbase() ? 1 : 0;
        __atomic_store_n(&g_fsgsbase_cached, has, __ATOMIC_RELAXED);
    }
    if (!has)
        return NULL;

    uint64_t fsb = strand_read_fsbase();

    /* Identify the main strand by the FS base lying OUTSIDE the Hammock window
     * — no dereference of a possibly-foreign base. Covers FS==0 (plain C main)
     * and a heap C++ TCB alike. */
    if (fsb < CABIN_HAMMOCK_BASE || fsb >= CABIN_HAMMOCK_END)
        return NULL;

    StrandInfo *si = (StrandInfo *)(uintptr_t)fsb;
    /* Defensive: the base is in the Hammock window, so it is a StrandInfo —
     * the magic check guards against a torn/garbage block and documents intent. */
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
