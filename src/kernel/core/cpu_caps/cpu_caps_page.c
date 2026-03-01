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
}
