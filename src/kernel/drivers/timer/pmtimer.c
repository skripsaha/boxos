#include "pmtimer.h"
#include "acpi.h"
#include "acpi_internal.h"
#include "io.h"

/* All PM Timer state lives in g_acpi.pm_timer_*; populated by
 * acpi_parse_tables. We never write to the counter — it's read-only
 * hardware. */

bool pmtimer_is_present(void)
{
    return g_acpi.pm_timer_present && g_acpi.pm_timer_io != 0;
}

uint32_t pmtimer_read(void)
{
    if (!pmtimer_is_present()) return 0;
    /* inl gives all 32 bits; mask to timer width so callers can subtract
     * without sign-extending past the wrap boundary. */
    uint32_t v = inl((uint16_t)g_acpi.pm_timer_io);
    if (g_acpi.pm_timer_bits == 24) v &= 0x00FFFFFFu;
    return v;
}

void pmtimer_busy_wait_us(uint64_t us)
{
    if (!pmtimer_is_present()) return;

    /* ticks = us * 3.579545 MHz = us * 3579545 / 1_000_000.
     * For us <= 5_000_000 (5 seconds) this fits comfortably in
     * uint64_t. Anything longer would risk a 24-bit wrap before
     * we even start the loop — refuse. */
    if (us > 5000000ULL) us = 5000000ULL;
    uint64_t target_ticks = (us * (uint64_t)PMTIMER_FREQ_HZ) / 1000000ULL;
    uint32_t mask = (g_acpi.pm_timer_bits == 24) ? 0x00FFFFFFu : 0xFFFFFFFFu;

    uint32_t start = pmtimer_read();
    uint64_t elapsed = 0;
    uint32_t last = start;
    while (elapsed < target_ticks) {
        uint32_t now = pmtimer_read();
        /* Mask-wrap-safe delta: (now - last) & mask handles a single
         * wrap inside the loop iteration. We sum into 64-bit elapsed
         * so multiple wraps over a long wait also tally correctly. */
        uint32_t step = (now - last) & mask;
        elapsed += step;
        last = now;
        __asm__ volatile("pause");
    }
}
