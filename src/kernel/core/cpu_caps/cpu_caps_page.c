#include "cpu_caps_page.h"
#include "pmm.h"
#include "vmm.h"
#include "cpuid.h"
#include "klib.h"

uint64_t g_cpu_caps_page_phys = 0;

void cpu_caps_page_init(void) {
    g_cpu_caps_page_phys = (uint64_t)pmm_alloc(1);
    if (g_cpu_caps_page_phys == 0) {
        return;
    }

    cpu_caps_page_t* caps_page = (cpu_caps_page_t*)vmm_phys_to_virt(g_cpu_caps_page_phys);

    memset(caps_page, 0, sizeof(cpu_caps_page_t));
    caps_page->magic = CPU_CAPS_PAGE_MAGIC;
    caps_page->has_waitpkg = g_cpu_caps.has_waitpkg;
    caps_page->has_invariant_tsc = g_cpu_caps.has_invariant_tsc;
    caps_page->has_pku = g_cpu_caps.has_pku;
    caps_page->tsc_freq_khz = 0;  // Will be filled after TSC calibration

    /* Register as shared so vmm_destroy_context skips the pmm_free —
     * this page lives for the whole kernel session. */
    vmm_register_shared_phys(g_cpu_caps_page_phys);
}

void cpu_caps_page_set_tsc_freq(uint64_t freq_khz) {
    if (g_cpu_caps_page_phys == 0) return;

    cpu_caps_page_t* caps_page = (cpu_caps_page_t*)vmm_phys_to_virt(g_cpu_caps_page_phys);
    caps_page->tsc_freq_khz = freq_khz;
}

void cpu_caps_page_refresh_features(void) {
    if (g_cpu_caps_page_phys == 0) return;

    /* RELEASE-store via __atomic so any userspace consumer that
     * ACQUIRE-loads these single-byte fields (boxlib cpu_has_*) sees
     * the post-intersect values coherently. Each field is independently
     * volatile in the userspace view; this barrier is for the benefit
     * of the kernel-side writer's compiler. */
    cpu_caps_page_t* caps_page = (cpu_caps_page_t*)vmm_phys_to_virt(g_cpu_caps_page_phys);
    __atomic_store_n(&caps_page->has_waitpkg,
                     g_cpu_caps.has_waitpkg, __ATOMIC_RELEASE);
    __atomic_store_n(&caps_page->has_pku,
                     g_cpu_caps.has_pku,     __ATOMIC_RELEASE);
}
