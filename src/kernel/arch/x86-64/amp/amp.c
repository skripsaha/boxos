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
#include "per_core.h"   // PerCpuData layout + g_per_core_active for the gs fast path

AmpLayout g_amp;

// Reverse map lapic_id -> dense core_index. Used only on slow paths:
// amp_init() while building the table, and amp_get_core_index()'s early-boot
// fallback (before per-cpu GS is live). A linear scan over the (<=256) built
// descriptors is width-independent — no 256-entry array that would break for
// 32-bit x2APIC ids.
static uint8_t amp_index_for_lapic(uint32_t lapic_id)
{
    for (uint8_t i = 0; i < g_amp.total_cores; i++) {
        if (g_amp.cores[i].lapic_id == lapic_id)
            return g_amp.cores[i].core_index;
    }
    return 0;   // unknown id -> attribute to BSP (matches prior behaviour)
}

// PIT channel 2 busy-wait delay
static void pit_delay_us(uint32_t microseconds)
{
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

    while (!(inb(0x61) & 0x20))
    {
        __asm__ volatile("pause");
    }
}

// log2 floor: returns 0 for 0 or 1
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
    // We need to re-walk MADT ourselves to collect ALL LAPIC IDs.
    // acpi_parse_madt() only stores the BSP LAPIC. So we call it for
    // the base info, then re-parse the MADT entry list directly.
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

    // BSP is whoever we are right now (full-width LAPIC ID of executing CPU)
    uint32_t my_lapic_id = lapic_get_id();
    g_amp.bsp_lapic_id = my_lapic_id;

    // Collect all enabled LAPIC IDs from MADT — Type 0 (8-bit) + Type 9 (32-bit)
    uint32_t lapic_ids[MAX_CORES];
    // max_count is uint8_t; MAX_CORES=256 wraps to 0, so use 255 (ACPI limit anyway)
    uint8_t count = amp_collect_lapics(lapic_ids, 255);

    if (count == 0)
    {
        // Fallback to single-core
        count = 1;
        lapic_ids[0] = my_lapic_id;
    }

    // Build core descriptors: BSP first, then APs
    uint8_t core_idx = 0;
    uint8_t max_idx = (uint8_t)(MAX_CORES - 1);

    // Find BSP in collected list and put it at index 0
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

    // APs: all others
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

    // Ensure g_amp.cores[] is fully populated and visible to APs before they boot
    mfence();

    g_amp.total_cores = core_idx;
    g_amp.k_count = (uint8_t)amp_calculate_kcores(g_amp.total_cores);
    g_amp.app_count = g_amp.total_cores - g_amp.k_count;

    // Assign K-core / App-core roles
    // Core 0 (BSP) is always a K-core. Next k_count-1 are K-cores too.
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

/* INIT and SIPI delivery to a single AP, mode-correct on both xAPIC
 * and x2APIC. lapic_icr_write picks the right path (legacy MMIO with
 * delivery-status polling, or single wrmsr on 0x830). Writing the
 * legacy MMIO ICR offsets directly on an x2APIC-enabled CPU is
 * reserved (Intel SDM Vol 3A §10.12.9) and produces #GP — observed on
 * STRICT mode with -cpu max prior to this refactor.
 *
 * Spec note on the missing INIT-DEASSERT: the level-deassert phase is
 * required ONLY for the discrete 82489DX APIC (Intel SDM Vol 3A
 * §9.4.1). Pentium and later integrated APICs treat any INIT IPI as
 * edge-triggered and ignore the deassert form — sending it just burns
 * an ICR write (and on x2APIC a wrmsr round-trip). The §9.4.4 "MP
 * Initialization Example" pseudo-code wraps the deassert in
 * `if (APIC_VERSION is an 82489DX)` for this exact reason. BoxOS
 * targets x86_64 (P6-era and later integrated APIC mandatory), so
 * the deassert is dead code on every supported part. */
static void send_init_ipi(uint8_t dest_lapic_id)
{
    lapic_icr_write((uint32_t)dest_lapic_id, LAPIC_IPI_INIT);
    pit_delay_us(10000); // 10ms per Intel MP boot sequence
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

    // Copy trampoline code to physical 0x8000.
    // After vmm_init() the identity map at PML4[0] is removed, so we cannot
    // use the physical address 0x8000 directly. We write via the Pull Map
    // which covers all physical RAM at PULL_MAP_BASE + phys.
    size_t trampoline_size = (size_t)(&ap_trampoline_end - &ap_trampoline_start);
    if (trampoline_size > 4096)
    {
        kprintf("[AMP] ERROR: trampoline too large (%zu bytes)\n", trampoline_size);
        return;
    }
    void *trampoline_virt = vmm_phys_to_virt(AP_TRAMPOLINE_PHYS);
    memcpy(trampoline_virt, &ap_trampoline_start, trampoline_size);

    // Map physical 0x8000 at virtual 0x8000 in the kernel context so that
    // after the AP enables paging, the trampoline code and data area are
    // accessible at the absolute addresses the trampoline code references.
    vmm_context_t *kctx = vmm_get_kernel_context();
    vmm_map_result_t map_res = vmm_map_page(
        kctx, AP_TRAMPOLINE_PHYS, AP_TRAMPOLINE_PHYS,
        VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE);
    if (!map_res.success)
    {
        kprintf("[AMP] ERROR: cannot map trampoline page at 0x%x\n", AP_TRAMPOLINE_PHYS);
        return;
    }

    // Trampoline data area offset within the page
    size_t data_offset = (size_t)(&ap_trampoline_data - &ap_trampoline_start);
    uint8_t *data_area = (uint8_t *)((uintptr_t)trampoline_virt + data_offset);

    // Read current CR3 (BSP's page tables, shared with all APs initially)
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));

    uint8_t sipi_vector = (uint8_t)(AP_TRAMPOLINE_PHYS >> 12); // = 8

    uint32_t booted = 0;

    for (uint8_t i = 0; i < g_amp.total_cores; i++)
    {
        CoreDescriptor *c = &g_amp.cores[i];
        if (c->is_bsp)
            continue;

        kprintf("[AMP] Booting AP %u (LAPIC ID %u)...\n", c->core_index, c->lapic_id);

        /* AP kernel stack layout: same shape as per-process kernel stack —
         * 1 guard page + CONFIG_KERNEL_STACK_PAGES data pages. Hardcoded
         * page-counts here used to silently desync from kernel_config.h:
         * a change to CONFIG_KERNEL_STACK_PAGES left AP stacks at the old
         * size, so process_t::kernel_stack_guard_base could no longer be
         * paired with this AP stack's geometry by the IST overflow path. */
        void *stack_phys = pmm_alloc(CONFIG_KERNEL_STACK_TOTAL_PAGES);
        if (!stack_phys)
        {
            kprintf("[AMP] ERROR: cannot allocate stack for core %u\n", c->core_index);
            continue;
        }
        void *stack_virt = vmm_phys_to_virt((uintptr_t)stack_phys);

        // Unmap first page as guard — stack overflow triggers #PF instead of
        // silently corrupting adjacent memory.
        vmm_context_t *kctx = vmm_get_kernel_context();
        pte_t *guard_pte = vmm_get_or_create_pte(kctx, (uintptr_t)stack_virt);
        if (guard_pte) {
            *guard_pte = 0;
            /* Cross-core shootdown — other cores keep the cached huge
             * Pull-Map entry covering this VA until invalidated. */
            vmm_shootdown_page(kctx, (uintptr_t)stack_virt);
        }

        uint64_t stack_top = (uint64_t)stack_virt +
                             CONFIG_KERNEL_STACK_TOTAL_PAGES * VMM_PAGE_SIZE - 16;

        // Fill trampoline data area
        // +0: CR3
        memcpy(data_area + 0, &cr3, 8);
        // +8: stack top
        memcpy(data_area + 8, &stack_top, 8);
        // +16: core_index
        data_area[16] = c->core_index;
        // +20: extra CR4 bits to OR into CR4 in the trampoline's 32-bit
        // phase BEFORE CR0.PG=1. Currently only CR4.LA57 (bit 12) needs to
        // be propagated — Intel SDM Vol 3A §4.5: CR4.LA57 cannot be modified
        // while paging is enabled, and the BSP's chosen CR3 is a PML5 root
        // when 5-level paging is active. Without LA57 set on the AP before
        // CR0.PG=1, the AP would walk the PML5 as a PML4 and crash.
        uint32_t extra_cr4 = __atomic_load_n(&g_vmm_la57_active, __ATOMIC_ACQUIRE)
                                 ? (1u << 12)
                                 : 0u;
        memcpy(data_area + 20, &extra_cr4, 4);

        // Memory fence before SIPI
        mfence();

        send_init_ipi(c->lapic_id);

        /* Intel SDM Vol 3A §9.4.4 example loop: send SIPI, poll briefly,
         * if AP didn't acknowledge send SIPI #2, then long-wait. Skipping
         * the second SIPI on responsive APs saves ~200 µs per AP (visible
         * at high core counts). The poll-window between SIPIs must be at
         * least 200 µs (the SDM "BSP DELAYs (200 µSEC)" requirement); we
         * use that as the floor and bail early as soon as online flips. */
        pit_delay_us(200);
        send_sipi(c->lapic_id, sipi_vector);

        uint32_t sipi1_polls = 0;
        const uint32_t SIPI1_POLL_MAX = 1; /* >=200 µs minimum window */
        bool ap_up = amp_core_online(c);
        while (!ap_up && sipi1_polls < SIPI1_POLL_MAX) {
            pit_delay_us(200);
            sipi1_polls++;
            ap_up = amp_core_online(c);
        }

        if (!ap_up) {
            send_sipi(c->lapic_id, sipi_vector);
            pit_delay_us(200);
        }

        /* Long-wait timeout. Linux uses 1000 ms because BIOS-parked AP
         * loops, microcode reload, and PLL relock on physically cold
         * cores routinely exceed the old 200 ms ceiling on real boards.
         * Iterating at LONG_WAIT_GRANULARITY_US keeps the wait
         * responsive once the AP actually comes up. Intel SDM Vol 3A §9.4
         * gives no upper bound — the OS is told to retry/wait "as long
         * as practical". */
        const uint32_t LONG_WAIT_MS            = 1000;
        const uint32_t LONG_WAIT_GRANULARITY_US = 1000;     /* 1 ms */
        const uint32_t LONG_WAIT_ITERATIONS    =
            LONG_WAIT_MS * 1000U / LONG_WAIT_GRANULARITY_US;

        uint32_t waited_iters = 0;
        ap_up = amp_core_online(c);
        while (!ap_up && waited_iters < LONG_WAIT_ITERATIONS) {
            pit_delay_us(LONG_WAIT_GRANULARITY_US);
            waited_iters++;
            ap_up = amp_core_online(c);
        }

        if (ap_up) {
            booted++;
        } else {
            kprintf("[AMP] WARNING: Core %u (LAPIC %u) did not respond in %u ms\n",
                    c->core_index, c->lapic_id, LONG_WAIT_MS);
            /* Restore the guard PTE BEFORE pmm_free. Without this the
             * zeroed leaf persists past the free; PMM hands the same
             * physical frame to a future allocator, that caller's
             * vmm_phys_to_virt resolves to the same VA, and the first
             * access faults on the still-cleared PTE. Re-map to the
             * original physical frame with kernel-RW so the Pull-Map
             * invariant (every RAM byte addressable via vmm_phys_to_virt)
             * is preserved after the free. IST stacks intentionally
             * leak — per_core_alloc_ist may have allocated them with
             * their own guard pages before the AP timed out, and we
             * cannot recover their stack_phys pointers from here. */
            if (guard_pte) {
                *guard_pte = (uint64_t)(uintptr_t)stack_phys
                             | VMM_FLAGS_KERNEL_RW;
                vmm_shootdown_page(kctx, (uintptr_t)stack_virt);
            }
            pmm_free(stack_phys, CONFIG_KERNEL_STACK_TOTAL_PAGES);
        }
    }

    /* Re-derive the per-role counts from what actually came up. The
     * pre-boot counts (set in amp_init from total_cores) lie when an AP
     * declined to start — a stale k_count makes kcore_init allocate
     * queues for dead cores AND makes kcore_find_least_loaded pick them.
     * After this re-count, every consumer that walks g_amp.cores[]
     * sees a layout that matches what is actually servicing IPIs. */
    {
        uint8_t live_k = 0, live_a = 0;
        for (uint8_t i = 0; i < g_amp.total_cores; i++) {
            CoreDescriptor *c = &g_amp.cores[i];
            if (c->is_bsp) {
                /* BSP is always online — it is the one running this code. */
                __atomic_store_n(&c->online, (uint8_t)1, __ATOMIC_RELEASE);
            }
            if (!amp_core_online(c)) continue;
            if (c->is_kcore) live_k++; else live_a++;
        }
        g_amp.k_count   = live_k;
        g_amp.app_count = live_a;
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

    /* Trampoline cleanup: the 0x8000 identity mapping installed for AP
     * bring-up is no longer needed. Leaving it wired exposes a writable
     * code page at a well-known low address (Intel BIOS Writer's Guide
     * §"AP cleanup") and pins one PMM page for the boot lifetime. The
     * page was identity-mapped via vmm_map_page (PRESENT|WRITABLE) and
     * its trampoline contents were never re-used after the last SIPI;
     * unmap with cross-core shootdown so peer APs drop any cached TLB
     * entry for the VA before some unrelated alloc reuses the physical
     * frame. */
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
    /* Fast path: once per-cpu data is live, this CPU's dense core index is
     * cached in PerCpuData.core_index and read directly through %gs. The entry
     * stubs (isr.asm / notify_entry.asm / jump_to_userspace) keep the active
     * GS base pointed at this CPU's PerCpuData in EVERY kernel context (IRQ,
     * exception, syscall, thread), so this read is always valid here — and it
     * is independent of APIC ID width (works on xAPIC and x2APIC alike). */
    if (__atomic_load_n(&g_per_core_active, __ATOMIC_ACQUIRE)) {
        uint32_t idx;
        __asm__ volatile("mov %%gs:%c1, %0"
                         : "=r"(idx)
                         : "i"(__builtin_offsetof(PerCpuData, core_index)));
        return (uint8_t)idx;
    }

    /* Early-boot fallback: BSP only, before per-core GS is established. Derive
     * the index from the (x2APIC-correct) APIC ID via a width-independent scan
     * of the descriptor table.
     *
     * ‼ Earlier still, the LAPIC MMIO window is not mapped, and asking it for
     * an ID is a load from virtual address 0x20 — lapic_read() indexes off a
     * base that is null until lapic_init() maps it. This function is called
     * from the kernel panic path, so that window is not academic: on the first
     * boot of BoxOS on real silicon, a panic raised before lapic_init() printed
     * "Unhandled kernel #PF at 0x20 err=0x0" and stopped, in place of the dump
     * naming the fault that caused it. A diagnostic that cannot survive being
     * needed early is not a diagnostic.
     *
     * Nothing but the BSP is running this early, so 0 is not a guess. */
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
