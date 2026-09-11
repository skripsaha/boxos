#ifndef PERF_TRACE_H
#define PERF_TRACE_H


#include "ktypes.h"
#include "kernel_config.h"

#define PERF_TRACE_CAPACITY 256u
#define PERF_TRACE_MASK     (PERF_TRACE_CAPACITY - 1u)


typedef struct {
    uint64_t tsc_start;
    uint64_t tsc_end;
    uint32_t pid;
    uint8_t  deck_id;
    uint8_t  opcode;
    uint16_t error_code;
    uint64_t _pad;
} PerfTraceEntry;

_Static_assert(sizeof(PerfTraceEntry) == 32,
               "PerfTraceEntry must be exactly 32 bytes");

typedef struct {
    PerfTraceEntry entries[PERF_TRACE_CAPACITY];
    uint32_t       head;
    uint32_t       total;
} PerfTraceRing;


#if CONFIG_PERF_TRACE

void perf_trace_init(void);

static inline void perf_trace_record(uint64_t tsc_start,
                                     uint64_t tsc_end,
                                     uint32_t pid,
                                     uint8_t  deck_id,
                                     uint8_t  opcode,
                                     uint16_t error_code);

void perf_dump(void);

void perf_dump_slow(uint64_t min_cycles);

void perf_trace_flush_since(uint32_t snapshot_total);

void perf_reset(void);


extern PerfTraceRing g_perf_ring;

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

    g_perf_ring.head  = (g_perf_ring.head + 1u) & PERF_TRACE_MASK;
    g_perf_ring.total += 1u;
}


#define PERF_TRACE_START(var)   uint64_t var = rdtsc()

#define PERF_TRACE_END(start_var, pocket_pid, did, op, err)    \
    perf_trace_record((start_var), rdtsc(),                     \
                      (uint32_t)(pocket_pid),                   \
                      (uint8_t)(did),                           \
                      (uint8_t)(op),                            \
                      (uint16_t)(err))

#else

#define PERF_TRACE_START(var)                           ((void)0)
#define PERF_TRACE_END(sv, pid, did, op, err)           ((void)0)

static inline void perf_trace_init(void)  { }
static inline void perf_dump(void)        { }
static inline void perf_dump_slow(uint64_t c) { (void)c; }
static inline void perf_trace_flush_since(uint32_t s) { (void)s; }
static inline uint32_t perf_trace_snapshot(void) { return 0; }
static inline void perf_reset(void)       { }

#endif

#endif