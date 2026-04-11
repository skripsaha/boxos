#ifndef PERF_TRACE_H
#define PERF_TRACE_H

/*
 * perf_trace.h — Zero-overhead deck operation ring buffer
 *
 * Design:
 *   - Single writer (guide context only): no locks on record path
 *   - 256-slot circular buffer: oldest entry silently overwritten
 *   - Each entry is exactly 32 bytes (fits a single cache line)
 *   - When CONFIG_PERF_TRACE=0: all macros compile to nothing
 *   - perf_dump() is the only function that touches serial I/O
 *
 * Entry layout (32 bytes, one cache line):
 *   tsc_start   [8]  raw TSC at handler entry
 *   tsc_end     [8]  raw TSC at handler return
 *   pid         [4]  source process id
 *   deck_id     [1]  deck identifier (DECK_SYSTEM=0xFF etc.)
 *   opcode      [1]  operation code within the deck
 *   error_code  [2]  result error_t (0 = OK)
 *   _pad        [8]  bring total to 32 bytes (reserved for future fields)
 */

#include "ktypes.h"
#include "kernel_config.h"

/* ------------------------------------------------------------------ */
/* Capacity: power-of-two so wrap is a single AND instruction          */
/* ------------------------------------------------------------------ */
#define PERF_TRACE_CAPACITY 256u
#define PERF_TRACE_MASK     (PERF_TRACE_CAPACITY - 1u)

/* ------------------------------------------------------------------ */
/* Data structures                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t tsc_start;   /* rdtsc() before handler call    */
    uint64_t tsc_end;     /* rdtsc() after handler return   */
    uint32_t pid;         /* process that issued the pocket */
    uint8_t  deck_id;     /* DECK_SYSTEM / DECK_STORAGE etc */
    uint8_t  opcode;      /* handler-specific opcode byte   */
    uint16_t error_code;  /* error_t result (0 = OK)        */
    uint64_t _pad;        /* reserved — keeps entry 32 bytes */
} PerfTraceEntry;

/* Verify at compile-time: entry must stay exactly 32 bytes */
_Static_assert(sizeof(PerfTraceEntry) == 32,
               "PerfTraceEntry must be exactly 32 bytes");

typedef struct {
    PerfTraceEntry entries[PERF_TRACE_CAPACITY];
    uint32_t       head;      /* index of next slot to write */
    uint32_t       total;     /* total entries ever recorded (not masked) */
} PerfTraceRing;

/* ------------------------------------------------------------------ */
/* API                                                                  */
/* ------------------------------------------------------------------ */

#if CONFIG_PERF_TRACE

void perf_trace_init(void);

/*
 * perf_trace_record — called from guide hot path.
 *
 * Inline so the compiler can see through the mask and combine the five
 * stores.  No branch, no lock, no function call overhead when inlined.
 */
static inline void perf_trace_record(uint64_t tsc_start,
                                     uint64_t tsc_end,
                                     uint32_t pid,
                                     uint8_t  deck_id,
                                     uint8_t  opcode,
                                     uint16_t error_code);

/*
 * perf_dump — walks the ring oldest-to-newest and prints every entry
 * to serial via kprintf.  ONLY call this manually (debug command,
 * shutdown hook, etc.) — never in the hot path.
 */
void perf_dump(void);

/*
 * perf_dump_slow — same as perf_dump but filters: only prints entries
 * whose elapsed cycles exceed the given threshold.
 */
void perf_dump_slow(uint64_t min_cycles);

/*
 * perf_trace_flush_since — prints entries recorded since snapshot_total.
 * Called by guide() AFTER all pockets are processed, so serial I/O
 * does not affect measurement accuracy.
 */
void perf_trace_flush_since(uint32_t snapshot_total);

/*
 * perf_reset — clears all entries and resets head/total to zero.
 */
void perf_reset(void);

/* ------------------------------------------------------------------ */
/* Inline implementations (need g_perf_ring to be declared first)      */
/* ------------------------------------------------------------------ */

extern PerfTraceRing g_perf_ring;

/*
 * perf_trace_snapshot — returns current total for flush_since pairing.
 */
static inline uint32_t perf_trace_snapshot(void) { return g_perf_ring.total; }

static inline void perf_trace_record(uint64_t tsc_start,
                                     uint64_t tsc_end,
                                     uint32_t pid,
                                     uint8_t  deck_id,
                                     uint8_t  opcode,
                                     uint16_t error_code)
{
    uint32_t slot = g_perf_ring.head & PERF_TRACE_MASK;
    PerfTraceEntry *e = &g_perf_ring.entries[slot];

    e->tsc_start  = tsc_start;
    e->tsc_end    = tsc_end;
    e->pid        = pid;
    e->deck_id    = deck_id;
    e->opcode     = opcode;
    e->error_code = error_code;
    e->_pad       = 0;

    /* Advance head after all fields are written.  No fence needed:
       single writer, single reader (dump), and dump is not concurrent. */
    g_perf_ring.head  = (g_perf_ring.head + 1u) & PERF_TRACE_MASK;
    g_perf_ring.total += 1u;
}

/* ------------------------------------------------------------------ */
/* Hot-path macros — used in guide.c                                   */
/* ------------------------------------------------------------------ */

/* Used in guide.c around each deck handler call */
#define PERF_TRACE_START(var)   uint64_t var = rdtsc()

#define PERF_TRACE_END(start_var, pocket_pid, did, op, err)    \
    perf_trace_record((start_var), rdtsc(),                     \
                      (uint32_t)(pocket_pid),                   \
                      (uint8_t)(did),                           \
                      (uint8_t)(op),                            \
                      (uint16_t)(err))

#else /* CONFIG_PERF_TRACE == 0 */

/* Zero overhead: all macros vanish entirely */
#define PERF_TRACE_START(var)                           ((void)0)
#define PERF_TRACE_END(sv, pid, did, op, err)           ((void)0)

static inline void perf_trace_init(void)  { }
static inline void perf_dump(void)        { }
static inline void perf_dump_slow(uint64_t c) { (void)c; }
static inline void perf_trace_flush_since(uint32_t s) { (void)s; }
static inline uint32_t perf_trace_snapshot(void) { return 0; }
static inline void perf_reset(void)       { }

#endif /* CONFIG_PERF_TRACE */

#endif /* PERF_TRACE_H */
