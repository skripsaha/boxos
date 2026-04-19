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

AmpLayout g_amp;

// O(1) lookup: lapic_id -> core_index
static uint8_t lapic_to_index[256];

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
    memset(lapic_to_index, 0xFF, sizeof(lapic_to_index));

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
        g_amp.bsp_lapic_id = (uint8_t)lapic_get_id();
        g_amp.cores[0].lapic_id = g_amp.bsp_lapic_id;
        g_amp.cores[0].core_index = 0;
        g_amp.cores[0].is_bsp = true;
        g_amp.cores[0].is_kcore = true;
        g_amp.cores[0].online = true;
        lapic_to_index[g_amp.bsp_lapic_id] = 0;
        return;
    }

    // BSP is whoever we are right now (LAPIC ID of executing CPU)
    uint8_t my_lapic_id = (uint8_t)lapic_get_id();
    g_amp.bsp_lapic_id = my_lapic_id;

    // Collect all enabled LAPIC IDs from MADT via amp_collect_lapics() (acpi_madt.c)
    uint8_t lapic_ids[MAX_CORES];
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
            lapic_to_index[my_lapic_id] = core_idx;
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
        lapic_to_index[lapic_ids[i]] = core_idx;
        core_idx++;
    }

    // Ensure lapic_to_index[] is visible to APs before they boot
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

static void wait_icr_not_pending(void)
{
    uint32_t timeout = 100000;
    while ((lapic_read(LAPIC_REG_ICR_LOW) & LAPIC_ICR_SEND_PENDING) && --timeout)
    {
        __asm__ volatile("pause");
    }
    if (timeout == 0)
    {
        debug_printf("[AMP] WARNING: LAPIC ICR send pending timeout\n");
    }
}

static void send_init_ipi(uint8_t dest_lapic_id)
{
    wait_icr_not_pending();
    lapic_write(LAPIC_REG_ICR_HIGH, (uint32_t)dest_lapic_id << 24);
    lapic_write(LAPIC_REG_ICR_LOW, LAPIC_IPI_INIT);

    pit_delay_us(10000); // 10ms

    wait_icr_not_pending();
    lapic_write(LAPIC_REG_ICR_HIGH, (uint32_t)dest_lapic_id << 24);
    lapic_write(LAPIC_REG_ICR_LOW, LAPIC_IPI_INIT_DEASSERT);
}

static void send_sipi(uint8_t dest_lapic_id, uint8_t vector_page)
{
    wait_icr_not_pending();
    lapic_write(LAPIC_REG_ICR_HIGH, (uint32_t)dest_lapic_id << 24);
    lapic_write(LAPIC_REG_ICR_LOW, LAPIC_IPI_SIPI | vector_page);
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

        // Allocate 16KB kernel stack for this AP (1 guard + 4 data pages).
        // Guard page at the bottom traps stack overflow via page fault.
        void *stack_phys = pmm_alloc(5);
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
            vmm_flush_tlb_page((uintptr_t)stack_virt);
        }

        uint64_t stack_top = (uint64_t)stack_virt + 5 * 4096 - 16;

        // Fill trampoline data area
        // +0: CR3
        memcpy(data_area + 0, &cr3, 8);
        // +8: stack top
        memcpy(data_area + 8, &stack_top, 8);
        // +16: core_index
        data_area[16] = c->core_index;

        // Memory fence before SIPI
        mfence();

        send_init_ipi(c->lapic_id);

        pit_delay_us(200);
        send_sipi(c->lapic_id, sipi_vector);
        pit_delay_us(200);
        send_sipi(c->lapic_id, sipi_vector);

        // Poll online flag with ~200ms timeout (1000 x 200us)
        uint32_t timeout = 1000;
        while (!c->online && timeout > 0)
        {
            pit_delay_us(200);
            timeout--;
            __asm__ volatile("" ::: "memory");
        }

        if (c->online)
        {
            booted++;
        }
        else
        {
            kprintf("[AMP] WARNING: Core %u (LAPIC %u) did not respond\n",
                    c->core_index, c->lapic_id);
        }
    }

    uint32_t expected = g_amp.total_cores - 1;
    if (booted == expected)
    {
        g_amp.multicore_active = true;
        kprintf("[AMP] All %u AP(s) online. AMP active.\n", booted);
    }
    else
    {
        kprintf("[AMP] %u/%u AP(s) online.\n", booted, expected);
        if (booted > 0)
            g_amp.multicore_active = true;
    }
}

uint8_t amp_get_core_index(void)
{
    uint8_t lapic_id = (uint8_t)lapic_get_id();
    uint8_t idx = lapic_to_index[lapic_id];
    if (idx == 0xFF)
        return 0;
    return idx;
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
