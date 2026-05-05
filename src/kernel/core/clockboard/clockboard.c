#include "clockboard.h"
#include "pmm.h"
#include "vmm.h"
#include "klib.h"

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

    s_clockboard_kva->magic           = CLOCKBOARD_MAGIC;
    s_clockboard_kva->version         = CLOCKBOARD_VERSION;
    s_clockboard_kva->uptime_us       = 0;
    s_clockboard_kva->uptime_ms       = 0;
    s_clockboard_kva->tick_count      = 0;
    s_clockboard_kva->tsc_freq_khz    = 0;  /* set by clockboard_set_tsc_freq_khz */
    s_clockboard_kva->boot_unix_secs  = 0;  /* set by clockboard_set_boot_unix_secs */

    kprintf("[CLOCKBOARD] page allocated phys=0x%lx kva=0x%lx\n",
            (unsigned long)s_clockboard_phys, (unsigned long)s_clockboard_kva);
}

uint64_t clockboard_phys(void)
{
    return s_clockboard_phys;
}

void clockboard_tick_update(uint64_t uptime_us, uint64_t tick_count)
{
    if (!s_clockboard_kva) return;
    /* Single-writer (PIT IRQ on BSP). Plain stores; cross-core readers
     * see naturally-aligned 8-byte word atomicity guaranteed by x86-64. */
    s_clockboard_kva->uptime_us  = uptime_us;
    s_clockboard_kva->uptime_ms  = uptime_us / 1000ULL;
    s_clockboard_kva->tick_count = tick_count;
}

void clockboard_set_tsc_freq_khz(uint64_t khz)
{
    if (s_clockboard_kva) s_clockboard_kva->tsc_freq_khz = khz;
}

void clockboard_set_boot_unix_secs(uint64_t secs)
{
    if (s_clockboard_kva) s_clockboard_kva->boot_unix_secs = secs;
}
