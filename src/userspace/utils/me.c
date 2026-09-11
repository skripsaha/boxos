#include "box/print.h"
#include "box/system.h"
#include "box/convert.h"

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
    uint_to_str((unsigned)whole, tmp, sizeof(tmp));
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

    uint64_t uptime_ms = info.uptime_ns / 1000000ULL;
    uint64_t s         = uptime_ms / 1000;
    uint32_t ms        = (uint32_t)(uptime_ms % 1000);

    printf("%color%s%color\n", COLOR_CYAN, info.version, COLOR_DEFAULT);
    printf("  Uptime:    %u.%u s (%u ms total)\n",
           (unsigned)s, (unsigned)(ms / 100),
           (unsigned)uptime_ms);
    printf("  Memory:    %s used / %s free / %s total\n",
           used, free, total);
    printf("  CPU:       %u core%s — %u K-Core, %u App-Core%s\n",
           info.cpu_total, info.cpu_total == 1 ? "" : "s",
           info.cpu_k_cores, info.cpu_app_cores,
           info.cpu_app_cores == 1 ? "" : "s");
    if (info.tsc_freq_khz)
        printf("  TSC:       %u kHz%s\n",
               (unsigned)info.tsc_freq_khz,
               info.has_invariant_tsc ? "" : "  [WARN: not invariant — timing may drift]");
    printf("  PIT:       %u Hz\n", info.pit_freq_hz);
    printf("  Processes: %u live\n", info.process_count);
    printf("  Mode:      %s%s\n",
           info.multicore_active ? "AMP active" : "single-core",
           info.has_waitpkg ? ", UMWAIT supported" : "");

    exit(0);
    return 0;
}