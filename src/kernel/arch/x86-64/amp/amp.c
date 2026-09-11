#include "amp.h"
#include "acpi_madt.h"
#include "lapic.h"
#include "pit.h"
#include "pmm.h"
#include "vmm.h"
#include "klib.h"
#include "io.h"
#include "atomics.h"
#include "linker_symbols.h"
#include "per_core.h"
#include "cpu_calibrate.h"
#include "cpuid.h"

AmpLayout g_amp;

static uint8_t amp_index_for_lapic(uint32_t lapic_id)
{
    for (uint8_t i = 0; i < g_amp.total_cores; i++) {
        if (g_amp.cores[i].lapic_id == lapic_id)
            return g_amp.cores[i].core_index;
    }
    return 0;
}

static void amp_delay_us(uint32_t microseconds)
{
    uint32_t khz = cpu_get_tsc_freq_khz();
    if (khz != 0) {
        uint64_t target = rdtsc() + ((uint64_t)khz * (uint64_t)microseconds) / 1000u;
        while ((int64_t)(rdtsc() - target) < 0)
            __asm__ volatile("pause");
        return;
    }

    uint16_t count = (uint16_t)((uint32_t)PIT_FREQUENCY * microseconds / 1000000);
    if (count < 1)
        count = 1;

    outb(0x61, (inb(0x61) & 0xFD) | 0x01);
    outb(0x43, 0xB0);
    outb(0x42, (uint8_t)(count & 0xFF));
    outb(0x42, (uint8_t)((count >> 8) & 0xFF));

    uint8_t tmp = inb(0x61);
    outb(0x61, tmp & 0xFE);
    outb(0x61, tmp | 0x01);

    static bool warned = false;
    for (uint32_t spins = 0; spins < 2000000u; spins++) {
        if (inb(0x61) & 0x20)
            return;
        __asm__ volatile("pause");
    }
    if (!warned) {
        warned = true;
        kprintf("[AMP] PIT channel 2 gate never asserted and no calibrated TSC "
                "— AP timing is approximate on this board\n");
    }
}

static uint32_t log2_floor(uint32_t n)
{
    if (n <= 1)
        return 0;
    uint32_t result = 0;
    while (n > 1)
    {
        n >>= 1;
        result++;
    }
    return result;
}

uint32_t amp_calculate_kcores(uint32_t total_cores)
{
    if (total_cores <= 1)
        return 1;
    uint32_t k = log2_floor(total_cores);
    if (k < 1)
        k = 1;
    return k;
}

void amp_init(void)
{
    memset(&g_amp, 0, sizeof(g_amp));

    madt_info_t madt;
    acpi_error_t err = acpi_parse_madt(&madt);
    if (err != ACPI_OK || !madt.valid)
    {
        kprintf("[AMP] MADT unavailable — single-core mode\n");
        g_amp.total_cores = 1;
        g_amp.k_count = 1;
        g_amp.app_count = 0;
        g_amp.bsp_index = 0;
        g_amp.bsp_lapic_id = lapic_get_id();
        g_amp.cores[0].lapic_id = g_amp.bsp_lapic_id;
        g_amp.cores[0].core_index = 0;
        g_amp.cores[0].is_bsp = true;
        g_amp.cores[0].is_kcore = true;
        g_amp.cores[0].online = true;
        return;
    }

    uint32_t my_lapic_id = lapic_get_id();
    g_amp.bsp_lapic_id = my_lapic_id;

    uint32_t lapic_ids[MAX_CORES];
    uint8_t count = amp_collect_lapics(lapic_ids, 255);

    if (count == 0)
    {
        count = 1;
        lapic_ids[0] = my_lapic_id;
    }

    uint8_t core_idx = 0;
    uint8_t max_idx = (uint8_t)(MAX_CORES - 1);

    for (uint8_t i = 0; i < count && core_idx < max_idx; i++)
    {
        if (lapic_ids[i] == my_lapic_id)
        {
            g_amp.cores[core_idx].lapic_id = my_lapic_id;
            g_amp.cores[core_idx].core_index = core_idx;
            g_amp.cores[core_idx].is_bsp = true;
            g_amp.cores[core_idx].online = true;
            g_amp.bsp_index = core_idx;
            core_idx++;
            break;
        }
    }

    for (uint8_t i = 0; i < count && core_idx < max_idx; i++)
    {
        if (lapic_ids[i] == my_lapic_id)
            continue;
        g_amp.cores[core_idx].lapic_id = lapic_ids[i];
        g_amp.cores[core_idx].core_index = core_idx;
        g_amp.cores[core_idx].is_bsp = false;
        g_amp.cores[core_idx].online = false;
        core_idx++;
    }

    mfence();

    g_amp.total_cores = core_idx;
    g_amp.k_count = (uint8_t)amp_calculate_kcores(g_amp.total_cores);
    g_amp.app_count = g_amp.total_cores - g_amp.k_count;

    for (uint8_t i = 0; i < g_amp.total_cores; i++)
    {
        g_amp.cores[i].is_kcore = (i < g_amp.k_count);
    }

    kprintf("[AMP] Detected %u core(s) (%u K-core(s), %u App Core(s))\n",
            g_amp.total_cores, g_amp.k_count, g_amp.app_count);

    for (uint8_t i = 0; i < g_amp.total_cores; i++)
    {
        CoreDescriptor *c = &g_amp.cores[i];
        kprintf("[AMP] Core %u: LAPIC ID %u [%s%s]\n",
                c->core_index, c->lapic_id,
                c->is_bsp ? "BSP, " : "",
                c->is_kcore ? "K-Core" : "App Core");
    }
}

static void send_init_ipi(uint8_t dest_lapic_id)
{
    lapic_icr_write((uint32_t)dest_lapic_id, LAPIC_IPI_INIT);
    amp_delay_us(10000);
}

static void send_sipi(uint8_t dest_lapic_id, uint8_t vector_page)
{
    lapic_icr_write((uint32_t)dest_lapic_id, LAPIC_IPI_SIPI | (uint32_t)vector_page);
}

void amp_boot_aps(void)
{
    if (g_amp.total_cores <= 1)
    {
        debug_printf("[AMP] Single-core, no APs to boot\n");
        return;
    }

    size_t trampoline_size = (size_t)(&ap_trampoline_end - &ap_trampoline_start);
    if (trampoline_size > 4096)
    {
        kprintf("[AMP] ERROR: trampoline too large (%zu bytes)\n", trampoline_size);
        return;
    }
    void *trampoline_virt = vmm_phys_to_virt(AP_TRAMPOLINE_PHYS);
    memcpy(trampoline_virt, &ap_trampoline_start, trampoline_size);

    vmm_context_t *kctx = vmm_get_kernel_context();
    vmm_map_result_t map_res = vmm_map_page(
        kctx, AP_TRAMPOLINE_PHYS, AP_TRAMPOLINE_PHYS,
        VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE);
    if (!map_res.success)
    {
        kprintf("[AMP] ERROR: cannot map trampoline page at 0x%x\n", AP_TRAMPOLINE_PHYS);
        return;
    }

    size_t data_offset = (size_t)(&ap_trampoline_data - &ap_trampoline_start);
    uint8_t *data_area = (uint8_t *)((uintptr_t)trampoline_virt + data_offset);

    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));

    uint8_t sipi_vector = (uint8_t)(AP_TRAMPOLINE_PHYS >> 12);

    uint32_t booted = 0;

    for (uint8_t i = 0; i < g_amp.total_cores; i++)
    {
        CoreDescriptor *c = &g_amp.cores[i];
        if (c->is_bsp)
            continue;

        kprintf("[AMP] Booting AP %u (LAPIC ID %u)...\n", c->core_index, c->lapic_id);

        void *stack_phys = pmm_alloc(CONFIG_KERNEL_STACK_TOTAL_PAGES);
        if (!stack_phys)
        {
            kprintf("[AMP] ERROR: cannot allocate stack for core %u\n", c->core_index);
            continue;
        }
        void *stack_virt = vmm_phys_to_virt((uintptr_t)stack_phys);

        vmm_context_t *kctx = vmm_get_kernel_context();
        pte_t *guard_pte = vmm_get_or_create_pte(kctx, (uintptr_t)stack_virt);
        if (guard_pte) {
            *guard_pte = 0;
            vmm_shootdown_page(kctx, (uintptr_t)stack_virt);
        }

        uint64_t stack_top = (uint64_t)stack_virt +
                             CONFIG_KERNEL_STACK_TOTAL_PAGES * VMM_PAGE_SIZE - 16;

        memcpy(data_area + 0, &cr3, 8);
        memcpy(data_area + 8, &stack_top, 8);
        data_area[16] = c->core_index;
        uint32_t extra_cr4 = __atomic_load_n(&g_vmm_la57_active, __ATOMIC_ACQUIRE)
                                 ? (1u << 12)
                                 : 0u;
        memcpy(data_area + 20, &extra_cr4, 4);
        uint32_t extra_efer = g_cpu_caps.has_nx ? (1u << 11) : 0u;
        memcpy(data_area + 24, &extra_efer, 4);

        mfence();

        send_init_ipi(c->lapic_id);

        amp_delay_us(200);
        send_sipi(c->lapic_id, sipi_vector);

        uint32_t sipi1_polls = 0;
        const uint32_t SIPI1_POLL_MAX = 1;
        bool ap_up = amp_core_online(c);
        while (!ap_up && sipi1_polls < SIPI1_POLL_MAX) {
            amp_delay_us(200);
            sipi1_polls++;
            ap_up = amp_core_online(c);
        }

        if (!ap_up) {
            send_sipi(c->lapic_id, sipi_vector);
            amp_delay_us(200);
        }

        const uint32_t LONG_WAIT_MS            = 1000;
        const uint32_t LONG_WAIT_GRANULARITY_US = 1000;
        const uint32_t LONG_WAIT_ITERATIONS    =
            LONG_WAIT_MS * 1000U / LONG_WAIT_GRANULARITY_US;

        uint32_t waited_iters = 0;
        ap_up = amp_core_online(c);
        while (!ap_up && waited_iters < LONG_WAIT_ITERATIONS) {
            amp_delay_us(LONG_WAIT_GRANULARITY_US);
            waited_iters++;
            ap_up = amp_core_online(c);
        }

        if (ap_up) {
            booted++;
            c->counted_at_boot = 1;
        } else {
            kprintf("[AMP] WARNING: Core %u (LAPIC %u) has not responded in "
                    "%u ms; its stack is kept in case it still starts\n",
                    c->core_index, c->lapic_id, LONG_WAIT_MS);

            (void)guard_pte;
            (void)stack_virt;
            (void)kctx;
        }
    }

    {
        uint8_t live_k = 0, live_a = 0;
        for (uint8_t i = 0; i < g_amp.total_cores; i++) {
            CoreDescriptor *c = &g_amp.cores[i];
            if (c->is_bsp) {
                __atomic_store_n(&c->online, (uint8_t)1, __ATOMIC_RELEASE);
            }
            if (!amp_core_online(c)) continue;
            if (c->is_kcore) live_k++; else live_a++;
        }
        g_amp.k_count   = live_k;
        g_amp.app_count = live_a;
    }

    {
        bool anyone_missing = false;
        for (uint8_t i = 0; i < g_amp.total_cores; i++) {
            CoreDescriptor *c2 = &g_amp.cores[i];
            if (!c2->is_bsp && !amp_core_online(c2)) { anyone_missing = true; break; }
        }
        if (anyone_missing) {
            const uint32_t GRACE_MS = 2000;
            kprintf("[AMP] waiting up to %u ms more for cores that were slow "
                    "to start...\n", GRACE_MS);
            for (uint32_t ms = 0; ms < GRACE_MS; ms++) {
                bool still_missing = false;
                for (uint8_t i = 0; i < g_amp.total_cores; i++) {
                    CoreDescriptor *c2 = &g_amp.cores[i];
                    if (!c2->is_bsp && !amp_core_online(c2)) {
                        still_missing = true;
                        break;
                    }
                }
                if (!still_missing) break;
                amp_delay_us(1000);
            }
        }
    }

    {
        uint8_t late = 0;
        for (uint8_t i = 0; i < g_amp.total_cores; i++) {
            CoreDescriptor *c2 = &g_amp.cores[i];
            if (c2->is_bsp || !amp_core_online(c2)) continue;
            if (!c2->counted_at_boot) {
                late++;
                booted++;
                kprintf("[AMP] Core %u (LAPIC %u) came up after its window — "
                        "counted\n", c2->core_index, c2->lapic_id);
            }
        }
        if (late)
            kprintf("[AMP] %u core(s) were late, not dead\n", late);
    }

    uint32_t expected = g_amp.total_cores - 1;
    if (booted == expected) {
        g_amp.multicore_active = true;
        kprintf("[AMP] All %u AP(s) online. AMP active "
                "(K-Cores=%u App-Cores=%u).\n",
                booted, g_amp.k_count, g_amp.app_count);
    } else {
        kprintf("[AMP] %u/%u AP(s) online "
                "(K-Cores=%u App-Cores=%u after dead-AP cleanup).\n",
                booted, expected, g_amp.k_count, g_amp.app_count);
        if (booted > 0)
            g_amp.multicore_active = true;
    }

    {
        vmm_context_t *kctx = vmm_get_kernel_context();
        pte_t *t = vmm_get_or_create_pte(kctx, AP_TRAMPOLINE_PHYS);
        if (t) {
            *t = 0;
            vmm_shootdown_page(kctx, AP_TRAMPOLINE_PHYS);
        }
    }
}

uint8_t amp_get_core_index(void)
{
    if (__atomic_load_n(&g_per_core_active, __ATOMIC_ACQUIRE)) {
        uint32_t idx;
        __asm__ volatile("mov %%gs:%c1, %0"
                         : "=r"(idx)
                         : "i"(__builtin_offsetof(PerCpuData, core_index)));
        return (uint8_t)idx;
    }

    if (!lapic_is_mapped())
        return 0;
    return amp_index_for_lapic(lapic_get_id());
}

bool amp_is_kcore(void)
{
    uint8_t idx = amp_get_core_index();
    if (idx >= g_amp.total_cores)
        return true;
    return g_amp.cores[idx].is_kcore;
}

bool amp_is_appcore(void)
{
    return !amp_is_kcore();
}