#include "box/print.h"
#include "box/system.h"
#include "box/convert.h"
#include "box/time.h"
#include "box/clock.h"
#include "box/string.h"
#include "box/sync.h"

/* Format a byte count as "X.YY MB" or "X.YY GB" — two decimals, truncated
 * rather than rounded, without floats. Always emits a trailing NUL into buf
 * (>= 24 bytes). */
static void fmt_bytes(uint64_t bytes, char *buf, size_t buf_size)
{
    if (buf_size < 16) { if (buf_size) buf[0] = '\0'; return; }
    const char *unit;
    uint64_t whole;
    uint64_t frac_x100;
    if (bytes >= (1ULL << 30)) {
        whole     = bytes / (1ULL << 30);
        frac_x100 = ((bytes % (1ULL << 30)) * 100) / (1ULL << 30);
        unit      = "GB";
    } else if (bytes >= (1ULL << 20)) {
        whole     = bytes / (1ULL << 20);
        frac_x100 = ((bytes % (1ULL << 20)) * 100) / (1ULL << 20);
        unit      = "MB";
    } else if (bytes >= (1ULL << 10)) {
        whole     = bytes / (1ULL << 10);
        frac_x100 = ((bytes % (1ULL << 10)) * 100) / (1ULL << 10);
        unit      = "KB";
    } else {
        whole     = bytes;
        frac_x100 = 0;
        unit      = "B";
    }
    char tmp[24];
    uint64_to_str(whole, tmp, sizeof(tmp));
    size_t pos = 0;
    for (size_t i = 0; tmp[i] && pos < buf_size - 6; i++) buf[pos++] = tmp[i];
    if (frac_x100 > 0 && pos < buf_size - 6) {
        buf[pos++] = '.';
        buf[pos++] = (char)('0' + (frac_x100 / 10));
        buf[pos++] = (char)('0' + (frac_x100 % 10));
    }
    if (pos < buf_size - 4) {
        buf[pos++] = ' ';
        buf[pos++] = unit[0];
        if (unit[1]) buf[pos++] = unit[1];
    }
    buf[pos] = '\0';
}

static void wait_for_sample_window(uint32_t milliseconds)
{
    volatile uint64_t wait_word __attribute__((aligned(8))) = 0;
    error_t rc = addr_park(&wait_word, 0, milliseconds);
    if (rc == OK || rc == ERR_TIMEOUT) return;

    uint64_t started_ms = clock_uptime_ms();
    while (clock_uptime_ms() - started_ms < milliseconds)
        yield();
}

int main(void) {

    system_info_t info;
    if (sysinfo(&info) != 0) {
        printf("%colorError:%color failed to read system info\n",
               COLOR_RED, COLOR_DEFAULT);
        exit(1);
        return 1;
    }

    char total[24], used[24], free[24];
    fmt_bytes(info.total_memory, total, sizeof(total));
    fmt_bytes(info.used_memory,  used,  sizeof(used));
    fmt_bytes(info.free_memory,  free,  sizeof(free));

    /* Printed at the width the kernel counts in. The 32-bit cast that stood
     * here turned the millisecond total over at 2^32 ms — 49.7 days — and said
     * nothing about it, so a machine two months up showed an uptime shorter
     * than yesterday's. boxlib's printf reads the ll length modifier
     * (print.c), so the whole counter costs two characters of format. */
    uint64_t uptime_ms = info.uptime_ns / 1000000ULL;
    uint64_t s         = uptime_ms / 1000;
    unsigned tenths    = (unsigned)((uptime_ms % 1000) / 100);

    printf("%color%s%color\n", COLOR_CYAN, info.version, COLOR_DEFAULT);
    printf("  Uptime:    %llu.%u s (%llu ms total)\n",
           (unsigned long long)s, tenths,
           (unsigned long long)uptime_ms);
    printf("  Memory:    %s used / %s free / %s total\n",
           used, free, total);
    printf("  CPU:       %u core%s — %u K-Core, %u App-Core%s\n",
           info.cpu_total, info.cpu_total == 1 ? "" : "s",
           info.cpu_k_cores, info.cpu_app_cores,
           info.cpu_app_cores == 1 ? "" : "s");

    system_resources_t resources_before;
    system_resources_t resources_after;
    int resources_rc = system_resources(&resources_before);
    int usage_rc = resources_rc;
    if (resources_rc == 0) {
        wait_for_sample_window(100);
        usage_rc = system_resources(&resources_after);
    }
    if (resources_rc == 0 && resources_before.volume_present) {
        char disk_free[24];
        fmt_bytes(resources_before.volume_free_bytes, disk_free, sizeof(disk_free));
        char disk_name[17];
        memcpy(disk_name, resources_before.volume_name, 16);
        disk_name[16] = '\0';
        if (resources_before.volume_free_counted)
            printf("  Disk:      %s — %s free\n",
                   disk_name[0] ? disk_name : "unnamed volume", disk_free);
        else
            printf("  Disk:      %s — its free ground is not counted\n",
                   disk_name[0] ? disk_name : "unnamed volume");
    } else {
        printf("  Disk:      unavailable\n");
    }
    if (usage_rc == 0) {
        uint64_t busy = resources_after.cpu_busy_tsc >= resources_before.cpu_busy_tsc
                      ? resources_after.cpu_busy_tsc - resources_before.cpu_busy_tsc : 0;
        uint64_t total_cpu = resources_after.cpu_total_tsc >= resources_before.cpu_total_tsc
                           ? resources_after.cpu_total_tsc - resources_before.cpu_total_tsc : 0;
        unsigned load = 0;
        if (total_cpu != 0) {
            if (busy >= total_cpu) {
                load = 100;
            } else {
                load = (unsigned)((busy * 100ULL) / total_cpu);
            }
        }
        if (load > 100) load = 100;
        printf("  Load:      %u%% CPU\n", load);
    } else {
        printf("  Load:      unavailable\n");
    }
    /* Inverted UX: silent when the TSC invariant CPUID bit is set
     * (the production-correct state), loud when it isn't (timing may
     * drift with P-states). Previous "(invariant)" tag confused users
     * into thinking it was a value rather than a feature flag. */
    if (info.tsc_freq_khz)
        printf("  TSC:       %u kHz%s\n",
               (unsigned)info.tsc_freq_khz,
               info.has_invariant_tsc ? "" : "  [WARN: not invariant — timing may drift]");
    /* Name the part that is ticking, not the one that used to. On a board
     * where the HPET takes IRQ0 through LegacyReplacement the 8254 is off the
     * line and the kernel never writes a divisor into it, so the old
     * "PIT: 250 Hz" sent anyone chasing tick jitter to a chip that was not in
     * the circuit. */
    switch (info.tick_source) {
    case SYSINFO_TICK_SOURCE_HPET_LEGACY:
        printf("  Tick:      %u Hz from HPET timer 0, routed to IRQ0 by "
               "LegacyReplacement\n", info.tick_freq_hz);
        break;
    case SYSINFO_TICK_SOURCE_8254:
        printf("  Tick:      %u Hz from the 8254, channel 0\n",
               info.tick_freq_hz);
        break;
    default:
        printf("  Tick:      nothing is driving IRQ0 on this machine\n");
        break;
    }

    /* The two ways userspace can ask how long the machine has been up: the
     * page the kernel publishes, read here by a plain load, and the system
     * deck, which is where the uptime above came from. They are the same
     * clock now, so this says nothing on a healthy machine and names the gap
     * the moment they come apart again. Two tick periods of slack, because
     * the page is refreshed once per tick and both numbers are whole
     * milliseconds; a floor of one millisecond, because that is the
     * quantisation of the comparison itself. */
    uint64_t board_ms = 0;
    if (info.tick_freq_hz && time_uptime_ms(&board_ms) == 0) {
        uint64_t slack = 2000ULL / info.tick_freq_hz;
        if (slack == 0) slack = 1;
        uint64_t gap = board_ms > uptime_ms ? board_ms - uptime_ms
                                            : uptime_ms - board_ms;
        if (gap > slack)
            printf("  Clock:     the published page says %llu ms while the "
                   "kernel's own clock says %llu ms - %llu ms apart, which is "
                   "more than the tick can explain\n",
                   (unsigned long long)board_ms, (unsigned long long)uptime_ms,
                   (unsigned long long)gap);
    }
    printf("  Processes: %u live\n", info.process_count);
    printf("  Mode:      %s%s\n",
           info.multicore_active ? "AMP active" : "single-core",
           info.has_waitpkg ? ", UMWAIT supported" : "");

    exit(0);
    return 0;
}
