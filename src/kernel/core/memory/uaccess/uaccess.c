/*
 * uaccess — implementation of bracketed user-space access primitives.
 *
 * See uaccess.h for the design rationale + threat model.
 *
 * Fixup table layout
 * ------------------
 * Each access primitive emits a (fault_rip, recovery_rip) pair into the
 * `.uaccess_fixup` linker section. Linker bounds:
 *   __uaccess_fixup_start  — first byte of the table (8-byte aligned)
 *   __uaccess_fixup_end    — one past the last entry
 * Pair count: (end - start) / 16.
 *
 * The table is consulted by the VMM page-fault handler on every kernel-
 * mode #PF. A linear scan is acceptable because the table is small
 * (every static call site that does user access contributes one entry,
 * so a real kernel has on the order of dozens). If we ever cross into
 * hundreds, sort at link time + binary search; not now.
 *
 * GCC asm goto
 * ------------
 * The fault path returns control to a C-side label via `asm goto`. The
 * fixup table's recovery RIP is the linker's resolution of `%l[fault]`
 * — a code address inside the function, AFTER the asm block. On fault,
 * the VMM PF handler sets frame->rip to that address and IRETs; CPU
 * resumes execution at the fault label, which executes `clac()` and
 * returns the error code.
 *
 * AC=1 leak risk: between the faulting instruction and the C-side
 * `clac()`, RFLAGS.AC=1. If an interrupt fires in that window, the
 * IRQ handler runs with AC=1 — which is fine for safety because the
 * interrupt frame on iret restores RFLAGS from the saved frame (which
 * had AC=0 at interrupt entry from kernel mode). On a more conservative
 * design we'd `clac()` inside the asm block before jumping out — but
 * that requires an extra section + cross-section jump that's brittle
 * with toolchain versions. The asm-goto pattern is the canonical
 * Linux solution.
 */

#include "uaccess.h"
#include "klib.h"   /* debug_printf for boot-time fixup table report */

extern uintptr_t __uaccess_fixup_start[];
extern uintptr_t __uaccess_fixup_end[];

/* Sorted-by-fault_rip mirror of the linker-emitted fixup table. Populated
 * by uaccess_init at boot; consulted via binary search from the #PF
 * handler. Sized for plenty of headroom — every fixup-emitting helper
 * (copy_to/from_user, put/get_user_uN, plus any future ones) contributes
 * one entry, so 256 covers the table even after the audit adds another
 * dozen direct user-VA sites. If the actual count exceeds the cap,
 * lookup falls back to the linker-emitted linear scan — slower but
 * correct.
 *
 * Static .bss array (NOT kmalloc) so uaccess_init has no allocator
 * dependency — it runs from main.c before kmalloc's heap is committed
 * via demand-paging. */
#define UACCESS_FIXUP_CAP   256

static struct {
    uintptr_t fault;
    uintptr_t recovery;
} g_fixup_sorted[UACCESS_FIXUP_CAP];

static size_t g_fixup_sorted_count = 0;
static bool   g_fixup_sorted_ready = false;
static bool   g_fixup_overflowed   = false;

void uaccess_init(void) {
    if (g_fixup_sorted_ready) return;

    size_t pair_count = (size_t)(__uaccess_fixup_end - __uaccess_fixup_start) / 2;
    if (pair_count > UACCESS_FIXUP_CAP) {
        /* The static buffer can't hold the full table — leave
         * g_fixup_sorted_ready=false so lookup falls back to the
         * linker-emitted linear scan. The overflow flag surfaces in
         * the debug log so an operator can bump UACCESS_FIXUP_CAP. */
        g_fixup_overflowed = true;
        debug_printf("[uaccess] WARN fixup table has %zu entries (cap=%d) — "
                     "lookup falls back to linear scan\n",
                     pair_count, UACCESS_FIXUP_CAP);
        return;
    }

    for (size_t i = 0; i < pair_count; i++) {
        g_fixup_sorted[i].fault    = __uaccess_fixup_start[i * 2];
        g_fixup_sorted[i].recovery = __uaccess_fixup_start[i * 2 + 1];
    }

    /* Insertion sort by fault_rip. Pair count is small (~dozens) so the
     * O(n²) cost is invisible at boot. The simpler algorithm avoids
     * pulling a generic qsort from kmalloc / libstd; it's also easier
     * to audit. Stable sort isn't strictly needed (each fault_rip is
     * unique by construction — emitted by a distinct asm site). */
    for (size_t i = 1; i < pair_count; i++) {
        uintptr_t f = g_fixup_sorted[i].fault;
        uintptr_t r = g_fixup_sorted[i].recovery;
        size_t j = i;
        while (j > 0 && g_fixup_sorted[j - 1].fault > f) {
            g_fixup_sorted[j] = g_fixup_sorted[j - 1];
            j--;
        }
        g_fixup_sorted[j].fault    = f;
        g_fixup_sorted[j].recovery = r;
    }

    g_fixup_sorted_count = pair_count;
    __atomic_store_n(&g_fixup_sorted_ready, true, __ATOMIC_RELEASE);

    debug_printf("[uaccess] fixup table sorted: %zu entries, binary-search active\n",
                 pair_count);
}

uintptr_t uaccess_lookup_fixup(uintptr_t fault_rip) {
    /* Fast path: binary search on the sorted mirror. ACQUIRE on the
     * ready flag pairs with the RELEASE store at the end of uaccess_init
     * so a fault that lands the instant init publishes the table sees
     * the fully-populated array, not a half-written one. */
    if (__atomic_load_n(&g_fixup_sorted_ready, __ATOMIC_ACQUIRE)) {
        size_t lo = 0, hi = g_fixup_sorted_count;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            uintptr_t mid_fault = g_fixup_sorted[mid].fault;
            if (mid_fault < fault_rip)      lo = mid + 1;
            else if (mid_fault > fault_rip) hi = mid;
            else                            return g_fixup_sorted[mid].recovery;
        }
        return 0;
    }

    /* Fallback path: linear scan on the linker-emitted .rodata table.
     * Triggered for #PFs that fire before uaccess_init runs (early-boot
     * window) or when the table overflowed UACCESS_FIXUP_CAP. Correct
     * but slower; the boot window is microseconds and the overflow case
     * is a soft warning, not a regression. */
    for (uintptr_t *p = __uaccess_fixup_start; p < __uaccess_fixup_end; p += 2) {
        if (p[0] == fault_rip) return p[1];
    }
    return 0;
}

/* ─── Bulk copies — rep movsb under STAC ──────────────────────────── */

size_t copy_to_user(void *dst, const void *src, size_t n) {
    if (!access_ok(dst, n)) return n;
    /* rep movsb is the ERMSB fast path on Intel + AMD and is SMAP-aware:
     * with CR4.SMAP=1 + AC=0 it faults on user-mapped target. We wrap
     * it in STAC/CLAC so the access succeeds; the fixup catches PF on
     * a genuinely unmapped page (unmap-while-syscall race). */
    __asm__ volatile goto (
        "stac\n\t"
        "1: rep movsb\n\t"
        "clac\n\t"
        ".pushsection .uaccess_fixup, \"a\", @progbits\n\t"
        ".quad 1b, %l[fault]\n\t"
        ".popsection\n\t"
        : "+c"(n), "+S"(src), "+D"(dst)
        :
        : "memory"
        : fault
    );
    return 0;

fault:
    /* `n`, `src`, `dst` are clobbered by `rep movsb`: on fault, RCX has
     * remaining count, RSI/RDI have advanced. C compiler can't see this
     * across the goto, so we re-read the count by inline asm. */
    {
        size_t remaining;
        __asm__ volatile ("mov %%rcx, %0" : "=r"(remaining));
        clac();
        return remaining;
    }
}

size_t copy_from_user(void *dst, const void *src, size_t n) {
    if (!access_ok(src, n)) return n;
    __asm__ volatile goto (
        "stac\n\t"
        "1: rep movsb\n\t"
        "clac\n\t"
        ".pushsection .uaccess_fixup, \"a\", @progbits\n\t"
        ".quad 1b, %l[fault]\n\t"
        ".popsection\n\t"
        : "+c"(n), "+S"(src), "+D"(dst)
        :
        : "memory"
        : fault
    );
    return 0;

fault:
    {
        size_t remaining;
        __asm__ volatile ("mov %%rcx, %0" : "=r"(remaining));
        clac();
        return remaining;
    }
}

/* ─── Single-word accessors ───────────────────────────────────────── */

int put_user_u32(uint32_t val, uint32_t *ptr) {
    if (!access_ok(ptr, sizeof(uint32_t))) return -1;
    __asm__ volatile goto (
        "stac\n\t"
        "1: movl %[val], (%[ptr])\n\t"
        "clac\n\t"
        ".pushsection .uaccess_fixup, \"a\", @progbits\n\t"
        ".quad 1b, %l[fault]\n\t"
        ".popsection\n\t"
        :
        : [val]"r"(val), [ptr]"r"(ptr)
        : "memory"
        : fault
    );
    return 0;

fault:
    clac();
    return -1;
}

int put_user_u64(uint64_t val, uint64_t *ptr) {
    if (!access_ok(ptr, sizeof(uint64_t))) return -1;
    __asm__ volatile goto (
        "stac\n\t"
        "1: movq %[val], (%[ptr])\n\t"
        "clac\n\t"
        ".pushsection .uaccess_fixup, \"a\", @progbits\n\t"
        ".quad 1b, %l[fault]\n\t"
        ".popsection\n\t"
        :
        : [val]"r"(val), [ptr]"r"(ptr)
        : "memory"
        : fault
    );
    return 0;

fault:
    clac();
    return -1;
}

int get_user_u32(uint32_t *out, const uint32_t *ptr) {
    if (!access_ok(ptr, sizeof(uint32_t))) return -1;
    uint32_t v;
    __asm__ volatile goto (
        "stac\n\t"
        "1: movl (%[ptr]), %[v]\n\t"
        "clac\n\t"
        ".pushsection .uaccess_fixup, \"a\", @progbits\n\t"
        ".quad 1b, %l[fault]\n\t"
        ".popsection\n\t"
        : [v]"=r"(v)
        : [ptr]"r"(ptr)
        : "memory"
        : fault
    );
    *out = v;
    return 0;

fault:
    clac();
    return -1;
}

int get_user_u64(uint64_t *out, const uint64_t *ptr) {
    if (!access_ok(ptr, sizeof(uint64_t))) return -1;
    uint64_t v;
    __asm__ volatile goto (
        "stac\n\t"
        "1: movq (%[ptr]), %[v]\n\t"
        "clac\n\t"
        ".pushsection .uaccess_fixup, \"a\", @progbits\n\t"
        ".quad 1b, %l[fault]\n\t"
        ".popsection\n\t"
        : [v]"=r"(v)
        : [ptr]"r"(ptr)
        : "memory"
        : fault
    );
    *out = v;
    return 0;

fault:
    clac();
    return -1;
}
