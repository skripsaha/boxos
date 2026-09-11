
#include "uaccess.h"
#include "klib.h"

bool g_uaccess_smap = false;

void uaccess_set_smap(bool present)
{
    g_uaccess_smap = present;

    kprintf("[uaccess] SMAP %s — user access is bracketed with STAC/CLAC%s\n",
            present ? "present" : "absent",
            present ? "" : " nowhere, because this CPU would fault on them");
}

extern uintptr_t __uaccess_fixup_start[];
extern uintptr_t __uaccess_fixup_end[];

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

    for (uintptr_t *p = __uaccess_fixup_start; p < __uaccess_fixup_end; p += 2) {
        if (p[0] == fault_rip) return p[1];
    }
    return 0;
}


size_t copy_to_user(void *dst, const void *src, size_t n) {
    if (!access_ok(dst, n)) return n;
    stac();
    __asm__ volatile goto (
        "1: rep movsb\n\t"
        ".pushsection .uaccess_fixup, \"a\", @progbits\n\t"
        ".quad 1b, %l[fault]\n\t"
        ".popsection\n\t"
        : "+c"(n), "+S"(src), "+D"(dst)
        :
        : "memory"
        : fault
    );
    clac();
    return 0;

fault:
    {
        size_t remaining;
        __asm__ volatile ("mov %%rcx, %0" : "=r"(remaining));
        clac();
        return remaining;
    }
}

size_t copy_from_user(void *dst, const void *src, size_t n) {
    if (!access_ok(src, n)) return n;
    stac();
    __asm__ volatile goto (
        "1: rep movsb\n\t"
        ".pushsection .uaccess_fixup, \"a\", @progbits\n\t"
        ".quad 1b, %l[fault]\n\t"
        ".popsection\n\t"
        : "+c"(n), "+S"(src), "+D"(dst)
        :
        : "memory"
        : fault
    );
    clac();
    return 0;

fault:
    {
        size_t remaining;
        __asm__ volatile ("mov %%rcx, %0" : "=r"(remaining));
        clac();
        return remaining;
    }
}


int put_user_u32(uint32_t val, uint32_t *ptr) {
    if (!access_ok(ptr, sizeof(uint32_t))) return -1;
    stac();
    __asm__ volatile goto (
        "1: movl %[val], (%[ptr])\n\t"
        ".pushsection .uaccess_fixup, \"a\", @progbits\n\t"
        ".quad 1b, %l[fault]\n\t"
        ".popsection\n\t"
        :
        : [val]"r"(val), [ptr]"r"(ptr)
        : "memory"
        : fault
    );
    clac();
    return 0;

fault:
    clac();
    return -1;
}

int put_user_u64(uint64_t val, uint64_t *ptr) {
    if (!access_ok(ptr, sizeof(uint64_t))) return -1;
    stac();
    __asm__ volatile goto (
        "1: movq %[val], (%[ptr])\n\t"
        ".pushsection .uaccess_fixup, \"a\", @progbits\n\t"
        ".quad 1b, %l[fault]\n\t"
        ".popsection\n\t"
        :
        : [val]"r"(val), [ptr]"r"(ptr)
        : "memory"
        : fault
    );
    clac();
    return 0;

fault:
    clac();
    return -1;
}

int get_user_u32(uint32_t *out, const uint32_t *ptr) {
    if (!access_ok(ptr, sizeof(uint32_t))) return -1;
    uint32_t v;
    stac();
    __asm__ volatile goto (
        "1: movl (%[ptr]), %[v]\n\t"
        ".pushsection .uaccess_fixup, \"a\", @progbits\n\t"
        ".quad 1b, %l[fault]\n\t"
        ".popsection\n\t"
        : [v]"=r"(v)
        : [ptr]"r"(ptr)
        : "memory"
        : fault
    );
    clac();
    *out = v;
    return 0;

fault:
    clac();
    return -1;
}

int get_user_u64(uint64_t *out, const uint64_t *ptr) {
    if (!access_ok(ptr, sizeof(uint64_t))) return -1;
    uint64_t v;
    stac();
    __asm__ volatile goto (
        "1: movq (%[ptr]), %[v]\n\t"
        ".pushsection .uaccess_fixup, \"a\", @progbits\n\t"
        ".quad 1b, %l[fault]\n\t"
        ".popsection\n\t"
        : [v]"=r"(v)
        : [ptr]"r"(ptr)
        : "memory"
        : fault
    );
    clac();
    *out = v;
    return 0;

fault:
    clac();
    return -1;
}