#include "clockboard.h"
#include "pmm.h"
#include "vmm.h"
#include "klib.h"
#include "pit.h"   /* pit_get_uptime_us/ms, pit_get_ticks — the kernel's one clock */

/* The single shared page. Allocated once in clockboard_init, never freed. */
static uint64_t    s_clockboard_phys = 0;
static ClockBoard *s_clockboard_kva  = NULL;

void clockboard_init(void)
{
    if (s_clockboard_phys) return;  /* idempotent */

    void *page = pmm_alloc_zero(1);
    if (!page) {
        kprintf("[CLOCKBOARD] FATAL: pmm_alloc for page failed\n");
        return;
    }
    s_clockboard_phys = (uint64_t)page;
    s_clockboard_kva  = (ClockBoard *)vmm_phys_to_virt(s_clockboard_phys);

    s_clockboard_kva->uptime_us       = 0;
    s_clockboard_kva->uptime_ms       = 0;
    s_clockboard_kva->tick_count      = 0;
    s_clockboard_kva->tsc_freq_khz    = 0;  /* set by clockboard_set_tsc_freq_khz */
    s_clockboard_kva->boot_unix_secs  = 0;  /* set by clockboard_set_boot_unix_secs */
    s_clockboard_kva->magic           = CLOCKBOARD_MAGIC;

    /* Register as shared so vmm_destroy_context skips pmm_free — this
     * page is mapped R/O into every Cabin and lives for the whole
     * kernel session. */
    vmm_register_shared_phys(s_clockboard_phys);

    kprintf("[CLOCKBOARD] page allocated phys=0x%lx kva=0x%lx\n",
            (unsigned long)s_clockboard_phys, (unsigned long)s_clockboard_kva);
}

uint64_t clockboard_phys(void)
{
    return s_clockboard_phys;
}

void clockboard_publish_uptime(void)
{
    if (!s_clockboard_kva) return;

    /* The page carries what the rest of the kernel would answer, read here
     * rather than passed in: a caller that supplies the number is a caller
     * that can supply a different one, and that is precisely how the page and
     * the system deck came to disagree by the length of a boot.
     *
     * Single-writer by construction: the BSP's IRQ0 handler once the machine
     * is running, and kernel_main once before any Ring 3 code exists to read
     * the page. IRQ0 is addressed physically to the BSP's LAPIC, so no App
     * Core ever enters here. Plain stores; cross-core readers see the
     * naturally-aligned 8-byte word atomicity x86-64 guarantees. */
    uint64_t us = pit_get_uptime_us();
    s_clockboard_kva->uptime_us  = us;
    s_clockboard_kva->uptime_ms  = us / 1000ULL;
    s_clockboard_kva->tick_count = pit_get_ticks();
}

uint64_t clockboard_uptime_ms(void)
{
    /* Kernel callers get the clock, not the mirror of it. The page only moves
     * when IRQ0 arrives, and the kernel asks this question during boot, where
     * no interrupt has arrived yet — Touch's deadlines read it from exactly
     * there. It is the same clock the page carries, so the two can no longer
     * answer differently; it simply costs the counter read instead of a load,
     * and takes no lock, which is what the idle path needs of it. */
    return pit_get_uptime_ms();
}

void clockboard_set_tsc_freq_khz(uint64_t khz)
{
    if (s_clockboard_kva) s_clockboard_kva->tsc_freq_khz = khz;
}

void clockboard_set_boot_unix_secs(uint64_t secs)
{
    if (s_clockboard_kva) s_clockboard_kva->boot_unix_secs = secs;
}
