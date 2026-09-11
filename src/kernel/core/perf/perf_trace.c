#include "perf_trace.h"
#include "klib.h"
#include "cpu_calibrate.h"
#include "boxos_decks.h"
#include "klib_logring.h"

#if CONFIG_PERF_TRACE

PerfTraceRing g_perf_ring;

static void ring_print(const char *s)
{
    while (*s) LogRingPut(*s++);
}

static void serial_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    while (*fmt)
    {
        if (*fmt != '%') { LogRingPut(*fmt++); continue; }
        fmt++;

        int pad_zero = 0, width = 0;
        if (*fmt == '0') { pad_zero = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9')
        {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        int ll = 0;
        while (*fmt == 'l') { ll++; fmt++; }

        switch (*fmt)
        {
        case 'u': {
            char tmp[24];
            if (ll >= 2) utoa64(va_arg(ap, uint64_t), tmp, 10);
            else         utoa(va_arg(ap, unsigned int), tmp, 10);
            ring_print(tmp);
            break;
        }
        case 'd': case 'i': {
            char tmp[24];
            if (ll >= 2)
            {
                long long v = va_arg(ap, long long);
                if (v < 0) { LogRingPut('-'); v = -v; }
                utoa64((uint64_t)v, tmp, 10);
            }
            else
            {
                itoa(va_arg(ap, int), tmp, 10);
            }
            ring_print(tmp);
            break;
        }
        case 'x': {
            char tmp[20];
            if (ll >= 2) utoa64(va_arg(ap, uint64_t), tmp, 16);
            else         utoa(va_arg(ap, unsigned int), tmp, 16);
            int len = 0;
            for (const char *p = tmp; *p; p++) len++;
            while (len < width) { LogRingPut(pad_zero ? '0' : ' '); len++; }
            ring_print(tmp);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (s) ring_print(s); else ring_print("(null)");
            break;
        }
        case '%':
            LogRingPut('%');
            break;
        default:
            LogRingPut('%');
            LogRingPut(*fmt);
            break;
        }
        fmt++;
    }

    va_end(ap);
}

static const char *deck_name(uint8_t deck_id)
{
    switch (deck_id)
    {
    case DECK_EXECUTION:
        return "EXEC";
    case DECK_OPERATIONS:
        return "OPS ";
    case DECK_STORAGE:
        return "STOR";
    case DECK_HARDWARE:
        return "HW  ";
    case DECK_SYSTEM:
        return "SYS ";
    default:
        return "????";
    }
}

void perf_trace_init(void)
{
    memset(&g_perf_ring, 0, sizeof(g_perf_ring));
    serial_printf("[PERF] Trace ring initialized: %u slots x %u bytes\n",
                  PERF_TRACE_CAPACITY,
                  (uint32_t)sizeof(PerfTraceEntry));
}

typedef void (*entry_visitor_t)(uint32_t seq,
                                const PerfTraceEntry *e,
                                uint64_t elapsed_cycles,
                                uint64_t elapsed_us,
                                void *ctx);

static void walk_ring(entry_visitor_t visitor, void *ctx)
{
    uint32_t count = g_perf_ring.total;
    if (count == 0)
        return;

    uint32_t n = (count < PERF_TRACE_CAPACITY) ? count : PERF_TRACE_CAPACITY;
    uint32_t start = (count < PERF_TRACE_CAPACITY)
                         ? 0u
                         : (g_perf_ring.head & PERF_TRACE_MASK);

    for (uint32_t i = 0; i < n; i++)
    {
        uint32_t idx = (start + i) & PERF_TRACE_MASK;
        const PerfTraceEntry *e = &g_perf_ring.entries[idx];
        uint64_t elapsed = (e->tsc_end >= e->tsc_start)
                               ? (e->tsc_end - e->tsc_start)
                               : 0u;
        uint64_t elapsed_us = cpu_tsc_to_us(elapsed);
        uint32_t seq = (g_perf_ring.total - n) + i;
        visitor(seq, e, elapsed, elapsed_us, ctx);
    }
}

static void visitor_print_all(uint32_t seq,
                              const PerfTraceEntry *e,
                              uint64_t elapsed_cycles,
                              uint64_t elapsed_us,
                              void *ctx)
{
    (void)ctx;
    serial_printf("[PERF] #%u pid=%u %s op=0x%02x err=%u  %llu us  (%llu cy)\n",
                  seq, e->pid, deck_name(e->deck_id), e->opcode,
                  (uint32_t)e->error_code, elapsed_us, elapsed_cycles);
}

static void visitor_print_slow(uint32_t seq,
                               const PerfTraceEntry *e,
                               uint64_t elapsed_cycles,
                               uint64_t elapsed_us,
                               void *ctx)
{
    uint64_t min_cycles = *(const uint64_t *)ctx;
    if (elapsed_cycles >= min_cycles)
    {
        serial_printf("[PERF] SLOW #%u pid=%u %s op=0x%02x err=%u  %llu us\n",
                      seq, e->pid, deck_name(e->deck_id), e->opcode,
                      (uint32_t)e->error_code, elapsed_us);
    }
}

void perf_dump(void)
{
    uint32_t n = (g_perf_ring.total < PERF_TRACE_CAPACITY)
                     ? g_perf_ring.total
                     : PERF_TRACE_CAPACITY;

    serial_printf("[PERF] === Dump: %u entries (total recorded: %u) ===\n",
                  n, g_perf_ring.total);
    walk_ring(visitor_print_all, NULL);
    serial_printf("[PERF] === End of dump ===\n");
}

void perf_dump_slow(uint64_t min_cycles)
{
    serial_printf("[PERF] === Slow entries (>= %llu cycles) ===\n", min_cycles);
    walk_ring(visitor_print_slow, &min_cycles);
    serial_printf("[PERF] === End of slow dump ===\n");
}

void perf_trace_flush_since(uint32_t snapshot_total)
{
    uint32_t new_entries = g_perf_ring.total - snapshot_total;
    if (new_entries == 0)
        return;
    if (new_entries > PERF_TRACE_CAPACITY)
        new_entries = PERF_TRACE_CAPACITY;

    uint32_t start_idx = (g_perf_ring.head - new_entries) & PERF_TRACE_MASK;

    for (uint32_t i = 0; i < new_entries; i++)
    {
        uint32_t idx = (start_idx + i) & PERF_TRACE_MASK;
        const PerfTraceEntry *e = &g_perf_ring.entries[idx];
        uint64_t elapsed = (e->tsc_end >= e->tsc_start)
                               ? (e->tsc_end - e->tsc_start)
                               : 0;
        uint64_t us = cpu_tsc_to_us(elapsed);
        serial_printf("[PERF] pid=%u %s op=0x%02x err=%u  %llu us\n",
                      e->pid, deck_name(e->deck_id), e->opcode,
                      (uint32_t)e->error_code, us);
    }
}

void perf_reset(void)
{
    memset(&g_perf_ring, 0, sizeof(g_perf_ring));
    serial_printf("[PERF] Trace ring reset.\n");
}

#endif