#include "perf_trace.h"
#include "klib.h"
#include "cpu_calibrate.h"
#include "boxos_decks.h"

#if CONFIG_PERF_TRACE

/* ------------------------------------------------------------------ */
/* Global ring buffer — lives in BSS, zero-initialized by loader       */
/* ------------------------------------------------------------------ */
PerfTraceRing g_perf_ring;

/* ------------------------------------------------------------------ */
/* Deck id -> human-readable name table                                 */
/* ------------------------------------------------------------------ */
static const char *deck_name(uint8_t deck_id)
{
    switch (deck_id) {
    case DECK_EXECUTION:  return "EXEC";
    case DECK_OPERATIONS: return "OPS ";
    case DECK_STORAGE:    return "STOR";
    case DECK_HARDWARE:   return "HW  ";
    case DECK_SYSTEM:     return "SYS ";
    default:              return "????";
    }
}

/* ------------------------------------------------------------------ */
/* perf_trace_init                                                      */
/* ------------------------------------------------------------------ */
void perf_trace_init(void)
{
    memset(&g_perf_ring, 0, sizeof(g_perf_ring));
    kprintf("[PERF] Trace ring initialized: %u slots x %u bytes = %u bytes\n",
            PERF_TRACE_CAPACITY,
            (uint32_t)sizeof(PerfTraceEntry),
            (uint32_t)sizeof(g_perf_ring.entries));
}

/* ------------------------------------------------------------------ */
/* Internal: walk entries from oldest to newest and call visitor        */
/* ------------------------------------------------------------------ */
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

    /* If we have wrapped, oldest entry is at current head.
       If we haven't wrapped yet, oldest entry is at index 0. */
    uint32_t n     = (count < PERF_TRACE_CAPACITY) ? count : PERF_TRACE_CAPACITY;
    uint32_t start = (count < PERF_TRACE_CAPACITY)
                         ? 0u
                         : (g_perf_ring.head & PERF_TRACE_MASK);

    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx              = (start + i) & PERF_TRACE_MASK;
        const PerfTraceEntry *e   = &g_perf_ring.entries[idx];
        uint64_t elapsed          = (e->tsc_end >= e->tsc_start)
                                        ? (e->tsc_end - e->tsc_start)
                                        : 0u;
        uint64_t elapsed_us       = cpu_tsc_to_us(elapsed);
        uint32_t seq              = (g_perf_ring.total - n) + i;
        visitor(seq, e, elapsed, elapsed_us, ctx);
    }
}

/* ------------------------------------------------------------------ */
/* Visitor: print every entry                                           */
/* ------------------------------------------------------------------ */
static void visitor_print_all(uint32_t seq,
                              const PerfTraceEntry *e,
                              uint64_t elapsed_cycles,
                              uint64_t elapsed_us,
                              void *ctx)
{
    (void)ctx;
    kprintf("[PERF] #%5u pid=%-5u %s op=0x%02x err=%-4u  %6llu us  (%llu cy)\n",
            seq,
            e->pid,
            deck_name(e->deck_id),
            e->opcode,
            (uint32_t)e->error_code,
            elapsed_us,
            elapsed_cycles);
}

/* ------------------------------------------------------------------ */
/* Visitor: print only entries above a cycle threshold                  */
/* ------------------------------------------------------------------ */
static void visitor_print_slow(uint32_t seq,
                               const PerfTraceEntry *e,
                               uint64_t elapsed_cycles,
                               uint64_t elapsed_us,
                               void *ctx)
{
    uint64_t min_cycles = *(const uint64_t *)ctx;
    if (elapsed_cycles >= min_cycles) {
        kprintf("[PERF] SLOW #%5u pid=%-5u %s op=0x%02x err=%-4u  %6llu us  (%llu cy)\n",
                seq,
                e->pid,
                deck_name(e->deck_id),
                e->opcode,
                (uint32_t)e->error_code,
                elapsed_us,
                elapsed_cycles);
    }
}

/* ------------------------------------------------------------------ */
/* Public dump functions                                                */
/* ------------------------------------------------------------------ */
void perf_dump(void)
{
    uint32_t n = (g_perf_ring.total < PERF_TRACE_CAPACITY)
                     ? g_perf_ring.total
                     : PERF_TRACE_CAPACITY;

    kprintf("[PERF] === Dump: %u entries (total recorded: %u) ===\n",
            n, g_perf_ring.total);

    walk_ring(visitor_print_all, NULL);

    kprintf("[PERF] === End of dump ===\n");
}

void perf_dump_slow(uint64_t min_cycles)
{
    kprintf("[PERF] === Slow entries (>= %llu cycles) ===\n", min_cycles);
    walk_ring(visitor_print_slow, &min_cycles);
    kprintf("[PERF] === End of slow dump ===\n");
}

void perf_reset(void)
{
    memset(&g_perf_ring, 0, sizeof(g_perf_ring));
    kprintf("[PERF] Trace ring reset.\n");
}

#endif /* CONFIG_PERF_TRACE */
