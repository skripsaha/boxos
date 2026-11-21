/*
 * bench — high-precision microbenchmark for BoxOS syscalls and IPC paths.
 *
 * Methodology
 * -----------
 * Each operation runs in a tight loop with the following discipline:
 *   1. WARMUP — N_WARMUP iterations are executed and discarded so I-cache,
 *      branch predictor, TLB and any first-touch state in boxlib reach a
 *      steady state. Without this the first few hundred samples drag the
 *      mean toward "cold" numbers that nobody hits in practice.
 *   2. SAMPLE — N iterations, each bracketed by `lfence;rdtsc` (start) and
 *      `rdtscp` (end). The two flavours bracket the window with the
 *      cleanest start/end serialisation x86 offers in user mode.
 *   3. STATS — sort the N deltas; report min / median / p99 / max in ns.
 *      The `min` is the most informative number for "what does this
 *      really cost when the CPU is left alone": it's the sample that
 *      didn't catch a PIT IRQ, a context switch or an L1 miss. Median
 *      shows the typical cost. p99 surfaces tail latency.
 *
 * The whole loop runs inside one process (no other userspace contention
 * if you start bench from shell with no other apps running). An App Core
 * has no tick — a strand alone on its core is not interrupted — so what
 * lands in the high end of the distribution is what else ran: the display,
 * a device's interrupt, the K-Core serving someone else. That is exactly
 * what you should care about.
 *
 * Warm and cold. A K-Core with nothing queued sleeps at once (HLT, kcore.c)
 * and a call wakes it with an IPI, so a call's price depends on whether the
 * K-Core was busy. Calls back to back find it just gone to sleep or still
 * awake; a call after a quiet moment — the App Core spinning without a
 * call, as a program computes between two — finds it asleep, as a key
 * pressed after a pause does (bench_run_rested).
 *
 * RDTSC vs RTC
 * ------------
 * CMOS RTC has 1-second granularity, PIT-derived uptime is ~2 ms — both
 * are useless for measuring a syscall (~1 µs). RDTSC is the only correct
 * primitive: ~25 cycles per call, < 10 ns granularity. With INVARIANT_TSC
 * the counter advances at the calibrated nominal frequency regardless of
 * CPU power state, so ticks → ns is a stable conversion.
 *
 * Limitations: any measurement that rounds to ~25 ns (a pair of rdtsc
 * calls) is at the floor of what we can resolve. Operations cheaper than
 * that — e.g. a register-only read — appear as 0 ns.
 */

#include "box/print.h"
#include "box/cpu.h"
#include "box/system.h"
#include "box/time.h"
#include "box/file.h"
#include "box/string.h"
#include "box/convert.h"
#include "box/ipc.h"
#include "box/core/notify.h"
#include "box/core/manifest.h"
#include "box/core/crate.h"
#include "box/core/cabin.h"
#include "box/core/result.h"
#include "box/core/result.h"
#include "box/error.h"
#include "box/display.h"
#include "box/touch.h"
#include "box/strand.h"
#include "box/sync.h"
#include "box/timeouts.h"

/* -------------------------------------------------------------------------
 *  Stats engine
 * ------------------------------------------------------------------------- */

#define BENCH_MAX_SAMPLES 2000

static uint64_t s_samples[BENCH_MAX_SAMPLES];

typedef struct
{
    uint32_t iters;
    uint64_t min_ns;
    uint64_t median_ns;
    uint64_t p99_ns;
    uint64_t max_ns;
    uint64_t avg_ns;
    bool ok;
    int err;
} bench_stats_t;

/* In-place insertion sort on a small array. Quicksort would be overkill
 * here — N ≤ 2000, this runs once per bench, total cost ≈ 4 M compares
 * which is < 5 ms even on a slow VM. Keeping it simple avoids a recursion
 * depth concern on our tiny user stacks. */
static void sort_u64(uint64_t *a, uint32_t n)
{
    for (uint32_t i = 1; i < n; i++)
    {
        uint64_t v = a[i];
        uint32_t j = i;
        while (j > 0 && a[j - 1] > v)
        {
            a[j] = a[j - 1];
            j--;
        }
        a[j] = v;
    }
}

static void compute_stats(uint64_t *deltas_tsc, uint32_t n, bench_stats_t *out)
{
    if (n == 0)
    {
        out->iters = 0;
        out->ok = false;
        return;
    }
    sort_u64(deltas_tsc, n);

    /* sum can grow large for slow ops × many iters; uint64 is enough for
     * any practical bench (1 s × 2000 iters ≈ 2 × 10^12 ticks ≪ 2^64). */
    uint64_t sum = 0;
    for (uint32_t i = 0; i < n; i++)
        sum += deltas_tsc[i];

    uint32_t med_idx = n / 2;
    uint32_t p99_idx = (n * 99u) / 100u;
    if (p99_idx >= n)
        p99_idx = n - 1;

    out->iters = n;
    out->min_ns = cpu_tsc_to_ns(deltas_tsc[0]);
    out->median_ns = cpu_tsc_to_ns(deltas_tsc[med_idx]);
    out->p99_ns = cpu_tsc_to_ns(deltas_tsc[p99_idx]);
    out->max_ns = cpu_tsc_to_ns(deltas_tsc[n - 1]);
    out->avg_ns = cpu_tsc_to_ns(sum / n);
    out->ok = true;
    out->err = 0;
}

/* -------------------------------------------------------------------------
 *  Pretty-print: right-aligned columns, scaled units (ns/µs/ms)
 * ------------------------------------------------------------------------- */

/* Format an integer right-aligned into a fixed-width field. The result
 * is appended to buf at *pos and *pos is advanced. */
static void emit_num_padded(char *buf, int *pos, int field, uint64_t v)
{
    char tmp[24];
    int len = 0;
    if (v == 0)
    {
        tmp[len++] = '0';
    }
    else
    {
        char rev[24];
        int rl = 0;
        while (v > 0 && rl < 23)
        {
            rev[rl++] = (char)('0' + (v % 10));
            v /= 10;
        }
        for (int i = 0; i < rl; i++)
            tmp[len++] = rev[rl - 1 - i];
    }
    int pad = field - len;
    while (pad-- > 0)
        buf[(*pos)++] = ' ';
    for (int i = 0; i < len; i++)
        buf[(*pos)++] = tmp[i];
}

static void emit_str_padded(char *buf, int *pos, int field, const char *s)
{
    int len = 0;
    while (s[len])
        len++;
    int copy = (len < field) ? len : field;
    for (int i = 0; i < copy; i++)
        buf[(*pos)++] = s[i];
    for (int i = copy; i < field; i++)
        buf[(*pos)++] = ' ';
}

static void print_header(void)
{
    println("");
    println("operation                                      iters       min       med       p99       max       avg");
    println("-----------------------------------         -------- --------- --------- --------- --------- ---------");
}

static void print_row(const char *name, const bench_stats_t *s)
{
    char line[160];
    int p = 0;
    emit_str_padded(line, &p, 41, name);
    line[p++] = ' ';
    if (!s->ok)
    {
        const char *msg = "ERR";
        emit_str_padded(line, &p, 9, msg);
        emit_str_padded(line, &p, 50, "");
    }
    else
    {
        emit_num_padded(line, &p, 9, s->iters);
        emit_num_padded(line, &p, 10, s->min_ns);
        emit_num_padded(line, &p, 10, s->median_ns);
        emit_num_padded(line, &p, 10, s->p99_ns);
        emit_num_padded(line, &p, 10, s->max_ns);
        emit_num_padded(line, &p, 10, s->avg_ns);
    }
    line[p] = '\0';
    println(line);
}

/* -------------------------------------------------------------------------
 *  Bench harness
 * ------------------------------------------------------------------------- */

/* Bench-callable function. Return < 0 to mark the whole row ERR (e.g. a
 * storage op that failed mid-loop). */
typedef int (*bench_fn)(uint32_t iter, void *ctx);

#define WARMUP_ITERS 64

/* `us` microseconds on this App Core without a call: the K-Cores are left
 * nothing of ours to do. */
static void bench_quiet(uint32_t us, uint64_t khz)
{
    const uint64_t until = cpu_rdtsc() + (uint64_t)us * khz / 1000u;
    while (cpu_rdtsc() < until)
        __builtin_ia32_pause();
}

/* Each sample after `rest_us` of quiet (0: back to back), not counted. */
static bench_stats_t bench_run_raw(bench_fn fn, void *ctx, uint32_t n, uint32_t rest_us)
{
    const uint64_t khz = cpu_get_tsc_freq_khz();
    bench_stats_t st = {0};
    if (n > BENCH_MAX_SAMPLES)
        n = BENCH_MAX_SAMPLES;

    /* Warmup */
    for (uint32_t i = 0; i < WARMUP_ITERS; i++)
    {
        if (fn(i, ctx) < 0)
        {
            st.ok = false;
            st.err = -1;
            return st;
        }
    }

    /* Sampling */
    for (uint32_t i = 0; i < n; i++)
    {
        if (rest_us)
            bench_quiet(rest_us, khz);
        uint64_t t0 = cpu_rdtsc();
        int rc = fn(i, ctx);
        uint64_t t1 = cpu_rdtsc_end();
        if (rc < 0)
        {
            st.ok = false;
            st.err = rc;
            return st;
        }
        s_samples[i] = (t1 > t0) ? (t1 - t0) : 0;
    }

    compute_stats(s_samples, n, &st);
    return st;
}

static void bench_run(const char *name, bench_fn fn, void *ctx, uint32_t iters)
{
    bench_stats_t s = bench_run_raw(fn, ctx, iters, 0);
    print_row(name, &s);
}

static void bench_run_rested(const char *name, bench_fn fn, void *ctx, uint32_t iters, uint32_t rest_us)
{
    bench_stats_t s = bench_run_raw(fn, ctx, iters, rest_us);
    print_row(name, &s);
}

/* -------------------------------------------------------------------------
 *  Bench targets
 * ------------------------------------------------------------------------- */

/* (1) RDTSC overhead — lower bound, every other measurement minus this
 *     is a closer estimate of the operation's true cost. */
static int b_rdtsc_overhead(uint32_t i, void *ctx)
{
    (void)i;
    (void)ctx;
    return 0;
}

/* (2) Cabin info read — pure memory load from CABIN_INFO_PAGE (no syscall).
 *     Establishes the floor for "anything that crosses a page boundary". */
static int b_cabin_info(uint32_t i, void *ctx)
{
    (void)i;
    (void)ctx;
    volatile CabinInfo *ci = cabin_info();
    /* prevent dead-store elimination of the read */
    if ((volatile uint32_t)ci->magic == 0)
        return 0;
    return 0;
}

/* (3) Yield — pushes a YIELD-flag Pocket and issues __notify. Pure
 *     syscall + scheduler decision (no Manifest, no Result). The thinnest
 *     userspace→kernel→userspace round-trip BoxOS exposes. */
static int b_yield(uint32_t i, void *ctx)
{
    (void)i;
    (void)ctx;
    yield();
    return 0;
}

/* (4) The least call: one op in a Manifest — ops.fill of an 8-byte crate,
 *     memory alone — served by a K-Core, its Result posted and taken. The
 *     whole Manifest pipeline (build, validate, dispatch, result push, result
 *     pop) and nothing else, and the same op the 1000-op chain carries: this
 *     row less the chain's cost of one op is the price of a call. Not
 *     time_uptime_ms, which reads the ClockBoard page and enters no kernel
 *     (0 ns), nor the hardware deck's timer read, which reads the HPET — three
 *     uncached reads of a device, microseconds on a board. */
static int b_least_call(uint32_t i, void *ctx)
{
    (void)ctx;
    uint8_t fill = (uint8_t)i;
    uint8_t out[8];
    return MfCall1(DECK_OPERATIONS, OP_BUF_FILL, &fill, 1, NULL, 0, out, sizeof(out), NULL,
                   BOX_ANSWER_GUARANTEED, NULL) == 0 ? 0 : -1;
}

/* (5) The least call told apart by the stopwatch a kernel built MEASURE=on
 *     puts on its answer (box/core/result.h): made and come to the gate,
 *     taken by a K-Core (on one core, by the gate), served, and its answer
 *     taken by the caller — each stage's own spread over the same calls.
 *     Nothing when the kernel keeps no stopwatch. A call whose moments are
 *     not in order is left out and counted: the cores' TSCs not in step. */
static void print_kv_u64(const char *label, uint64_t v, const char *unit);

#define STAGES 4
static uint64_t s_stage[STAGES][BENCH_MAX_SAMPLES];

static void bench_call_stages(const char *title, uint32_t n, uint32_t rest_us)
{
    static const char *const names[STAGES] = {
        "  made, at the gate", "  gate to taken", "  served", "  answered to caller",
    };
    volatile ResultRingHeader *h = &result_ring()->hdr;
    const uint64_t khz = cpu_get_tsc_freq_khz();
    if (n > BENCH_MAX_SAMPLES)
        n = BENCH_MAX_SAMPLES;
    for (uint32_t i = 0; i < WARMUP_ITERS; i++)
        if (b_least_call(i, NULL) < 0)
            return;
    if (h->call_answered == 0)
        return;

    uint32_t got = 0, out_of_order = 0;
    for (uint32_t i = 0; i < n; i++)
    {
        if (rest_us)
            bench_quiet(rest_us, khz);
        uint64_t made = cpu_rdtsc();
        if (b_least_call(i, NULL) < 0)
            return;
        uint64_t seen     = cpu_rdtsc_end();
        uint64_t gate     = h->call_gate;
        uint64_t taken    = h->call_taken;
        uint64_t answered = h->call_answered;
        if (!(made <= gate && gate <= taken && taken <= answered && answered <= seen))
        {
            out_of_order++;
            continue;
        }
        s_stage[0][got] = gate - made;
        s_stage[1][got] = taken - gate;
        s_stage[2][got] = answered - taken;
        s_stage[3][got] = seen - answered;
        got++;
    }
    println(title);
    for (uint32_t k = 0; k < STAGES; k++)
    {
        bench_stats_t st = {0};
        compute_stats(s_stage[k], got, &st);
        print_row(names[k], &st);
    }
    if (out_of_order)
        print_kv_u64("  left out, moments not in order:", out_of_order, "calls");
}

/* (6) Cross-process IPC roundtrip: broadcast a 1-byte DISP_CMD_PING,
 *     wait for display.elf to reply with its PID. Exercises the full
 *     end-to-end pipeline:
 *       bench: ipc_submit_one_op(BROADCAST, "display", 1B payload)
 *         → kernel: system.broadcast routes to display, pushes Pocket
 *           into display's PocketRing, sets display ready
 *         → scheduler: switches to display, restores its context
 *         → display: receive_wait drains its result_pop_ipc, sees PING
 *         → display: send(bench_pid, &my_pid, 4) — system.route op
 *           → kernel: pushes Result into bench's ResultRing
 *         → scheduler: returns to bench
 *         → bench: receive_wait pops the IPC reply
 *     This is the real "shell hits Enter, display prints prompt" cost. */
static int b_display_ping(uint32_t i, void *ctx)
{
    (void)i;
    (void)ctx;
    uint8_t ping = DISP_CMD_PING;
    int rc = broadcast("display", &ping, 1);
    if (rc < 0)
        return rc;
    Result r;
    if (!receive_wait(&r, 2000))
        return -ERR_TIMEOUT;
    return 0;
}

/* (7) Bulk Manifest: 1000 ops.fill operations in one syscall. Demonstrates
 *     the per-op cost amortised over the syscall overhead — divide
 *     median_ns by 1000 to get the per-op number. The chain.elf utility
 *     does the same thing but only times once. */
#define BULK_OPS 1000
static uint8_t s_bulk_mbuf[16 + BULK_OPS * 13 + 64];
static uint8_t s_bulk_buf[64];
static bool s_bulk_built = false;

static int b_chain_bulk(uint32_t i, void *ctx)
{
    (void)i;
    (void)ctx;
    if (!s_bulk_built)
    {
        ManifestBuilder mb;
        if (ManifestBuilderInit(&mb, s_bulk_mbuf, sizeof(s_bulk_mbuf)) != 0)
            return -1;
        for (int k = 0; k < BULK_OPS; k++)
        {
            uint8_t fb = (uint8_t)k;
            if (ManifestBuilderAddOp(&mb, DECK_OPERATIONS, OP_BUF_FILL, 0,
                                     CRATE_INDEX_NONE, 0, &fb, 1) != 0)
                return -1;
        }
        if (ManifestBuilderFinalize(&mb) != 0)
            return -1;
        s_bulk_built = true;
    }
    Crate c;
    CrateSetOutput(&c, s_bulk_buf, sizeof(s_bulk_buf));
    Result r;
    int rc = ManifestSubmit((Manifest *)s_bulk_mbuf, &c, 1, &r);
    if (rc != 0 || r.error_code != 0)
        return -1;
    return 0;
}

/* (7b) Strand ping-pong: a strand of this program and the main strand hand a
 *      word back and forth, each parked on it in the kernel until the other
 *      wakes it — two addr_wake calls, two parks and two wakes of a strand on
 *      another App Core (woken onto a free core) per round trip. The real
 *      cost of one strand waiting on another's work.
 *
 * Linux comparison: two threads on two cores handing a futex back and forth
 * (FUTEX_WAKE / FUTEX_WAIT) — a few microseconds a round trip on a desktop
 * processor, as lmbench's and the futex papers' figures put it; not measured
 * on this machine. */
#define PP_IDLE 0u
#define PP_SERVE 1u
#define PP_SERVED 2u
#define PP_END 3u
static volatile uint64_t s_pp_word __attribute__((aligned(8)));
static volatile uint64_t s_pp_gone __attribute__((aligned(8)));

static void pp_partner(void *arg)
{
    (void)arg;
    for (;;)
    {
        uint64_t v;
        while ((v = __atomic_load_n(&s_pp_word, __ATOMIC_ACQUIRE)) != PP_SERVE && v != PP_END)
            (void)addr_park(&s_pp_word, v, 0);
        if (v == PP_END)
            break;
        __atomic_store_n(&s_pp_word, PP_SERVED, __ATOMIC_RELEASE);
        (void)addr_wake(&s_pp_word, 1);
    }
    __atomic_store_n(&s_pp_gone, 1u, __ATOMIC_RELEASE);
    (void)addr_wake(&s_pp_gone, 0);
}

static int b_strand_pingpong(uint32_t i, void *ctx)
{
    (void)i;
    (void)ctx;
    __atomic_store_n(&s_pp_word, PP_SERVE, __ATOMIC_RELEASE);
    if (addr_wake(&s_pp_word, 1) != OK)
        return -1;
    uint64_t v;
    while ((v = __atomic_load_n(&s_pp_word, __ATOMIC_ACQUIRE)) != PP_SERVED)
        (void)addr_park(&s_pp_word, v, 0);
    return 0;
}

/* The partner started before the row and ended after it. */
static void bench_pingpong(const char *name, uint32_t iters)
{
    s_pp_word = PP_IDLE;
    s_pp_gone = 0;
    if (strand_spawn(pp_partner, NULL) == 0)
    {
        bench_stats_t none = {0};
        print_row(name, &none);
        return;
    }
    bench_run(name, b_strand_pingpong, NULL, iters);
    __atomic_store_n(&s_pp_word, PP_END, __ATOMIC_RELEASE);
    (void)addr_wake(&s_pp_word, 0);
    while (__atomic_load_n(&s_pp_gone, __ATOMIC_ACQUIRE) == 0)
        (void)addr_park(&s_pp_gone, 0, 0);
}

/* (8a) Touch: tag-multicast event delivery to self.
 *
 * Linux comparison:
 *   raise(SIGUSR1) + sigaction handler ≈ 0.5–1.5 µs on a 2026 box.
 *   kill(getpid(), SIGUSR1)            ≈ 1–3 µs (full syscall path).
 *   signalfd read after delivery      ≈ 1–4 µs.
 *
 * BoxOS Touch is NOT a 1-bit signal — every event carries an arbitrary
 * payload, is broadcast (1→N), and is capability-checked. So the fair
 * comparison is "kill + signalfd_read" in Linux vs "touch_send +
 * touch_await" here. The b_touch_self_rtt below measures exactly that
 * round-trip on one process subscribing to its own tag.
 *
 * b_touch_publish_only times the producer side in isolation: claim
 * already done in setup, send fires the manifest, kernel routes to self
 * (one subscriber), pushes one Touch into our ring. We DO NOT consume
 * it here — bench_setup_touch runs a drain at the start of each call.
 */

#define TAG_BENCH_RTT "bench:rtt"

static bool        s_touch_setup_done = false;
static TouchTagPair s_rtt_pair        = { TOUCH_TAG_INVALID, TOUCH_TAG_INVALID };
static TouchTag     s_rtt_tag         = TOUCH_TAG_INVALID;

static int bench_setup_touch(void)
{
    if (s_touch_setup_done) return 0;
    s_rtt_pair = touch_intern(TAG_BENCH_RTT);
    s_rtt_tag  = touch_pair_choose(s_rtt_pair);
    if (s_rtt_tag == TOUCH_TAG_INVALID) return -1;
    int rc = touch_claim(s_rtt_tag, TOUCH_REST, 0, 0);
    if (rc != 0) return rc;
    s_touch_setup_done = true;
    return 0;
}

static void bench_drain_touches(void)
{
    /* What the ring holds, taken without a wait. It ran inside the timed
     * window of every 16th publish and ended in a wait of 1 ms on the ring it
     * had emptied: six samples in a hundred were a millisecond long, and the
     * row's p99 of 1.03 ms on every machine was that wait, not a send. */
    Touch t;
    while (touch_try_pop_tag(s_rtt_tag, &t)) { /* discard */ }
}

/* Pure publish: send only, do NOT await. The kernel still delivers the
 * touch into our ring (one subscriber == self), so this measures
 *   userspace MfCall1 + kernel SysTouchSend + TouchPublishId iterating
 *   one subscriber + KResultPush of the Touch + manifest reply path.
 * The unconsumed Touch from each iter would pile up; we periodically
 * drain inside the harness via the warmup callback (handled by re-using
 * the bench harness's own drain — see calls below). */
static int b_touch_publish_only(uint32_t i, void *ctx)
{
    (void)ctx;
    if (bench_setup_touch() != 0) return -1;
    /* Drain accumulated unconsumed Touches every 16 iters so the ring
     * doesn't overflow — inside the timed window of those iterations, and
     * at no more than the pops of what is there. */
    if ((i & 15) == 0) bench_drain_touches();
    uint32_t payload = i;
    return touch_send(s_rtt_pair, &payload, sizeof(payload), 0);
}

/* Round-trip: send + await on the same tag. Closest analog to Linux
 * kill(self) + signalfd_read. Includes everything: producer manifest,
 * kernel publish, KResultPush, fast-path pop in the same userspace. */
static int b_touch_self_rtt(uint32_t i, void *ctx)
{
    (void)ctx;
    if (bench_setup_touch() != 0) return -1;
    uint32_t payload = i;
    int rc = touch_send(s_rtt_pair, &payload, sizeof(payload), 0);
    if (rc < 0) return rc;
    Touch t;
    rc = touch_await(s_rtt_tag, &t, 1000);
    return rc;
}

/* Just the receiving side: drains a pre-staged Touch from the ring.
 * Pre-staging happens via the warmup loop (which already calls fn 64×
 * before the timed window) — so by the timed window the ring is full
 * of pre-queued touches. Each iter pops one. This is the pure
 * fast-path (no kernel round-trip). */
static int b_touch_await_fastpath(uint32_t i, void *ctx)
{
    (void)i;
    (void)ctx;
    if (bench_setup_touch() != 0) return -1;
    /* Pre-stage one touch per iter so the ring has data when we pop. */
    uint32_t payload = i;
    int rc = touch_send(s_rtt_pair, &payload, sizeof(payload), 0);
    if (rc < 0) return rc;
    Touch t;
    return touch_await(s_rtt_tag, &t, 100);
}

/* (9) Storage: file create + write 64 B + delete. Hits TagFS and
 *     possibly disk I/O via AHCI/PIO. The slowest path BoxOS has end-to-
 *     end. Each iteration uses a unique filename so the tagfs allocator
 *     is exercised, not just an inode rewrite.
 *
 *     We delete after each iteration to avoid filling TagFS during the
 *     measurement (the delete itself is part of what we measure — that's
 *     intentional; "create then delete" is one logical op). */
static int b_file_cycle(uint32_t i, void *ctx)
{
    (void)ctx;
    char name[24];
    int p = 0;
    const char *prefix = "_bench_";
    while (prefix[p])
    {
        name[p] = prefix[p];
        p++;
    }
    /* append decimal i */
    char rev[12];
    int rl = 0;
    uint32_t v = i;
    if (v == 0)
    {
        rev[rl++] = '0';
    }
    else
    {
        while (v > 0 && rl < 11)
        {
            rev[rl++] = (char)('0' + (v % 10));
            v /= 10;
        }
    }
    while (rl > 0)
        name[p++] = rev[--rl];
    name[p] = '\0';

    int64_t file_id = create(name, "");
    if (file_id < 0)
        return (int)file_id;

    static const uint8_t payload[64] = {0};
    int wrc = fwrite((file_id_t)file_id, 0, payload, sizeof(payload));
    if (wrc < 0)
    {
        delete((file_id_t)file_id);
        return wrc;
    }

    int drc = delete((file_id_t)file_id);
    if (drc < 0)
        return drc;
    return 0;
}

/* -------------------------------------------------------------------------
 *  Entry
 * ------------------------------------------------------------------------- */

static void print_kv_u64(const char *label, uint64_t v, const char *unit)
{
    char line[120];
    int p = 0;
    while (*label)
        line[p++] = *label++;
    line[p++] = ' ';
    char rev[24];
    int rl = 0;
    uint64_t x = v;
    if (x == 0)
        rev[rl++] = '0';
    else
    {
        while (x > 0 && rl < 23)
        {
            rev[rl++] = (char)('0' + (x % 10));
            x /= 10;
        }
    }
    while (rl > 0)
        line[p++] = rev[--rl];
    if (unit && *unit)
    {
        line[p++] = ' ';
        while (*unit)
            line[p++] = *unit++;
    }
    line[p] = '\0';
    println(line);
}

int main(void)
{
    /* Said straight into the log ring: skip the display's lanes so the
     * bench itself is not amortised through the console's merge. The rows
     * we print are measured cleanly because every print is said at once. */
    io_set_mode(IO_MODE_RING);

    println("BoxOS bench — RDTSC microbenchmarks");

    uint64_t khz = cpu_get_tsc_freq_khz();
    if (khz == 0)
    {
        println("WARNING: TSC frequency unavailable — ns conversions will be 0");
    }
    else
    {
        print_kv_u64("TSC freq:", khz, "kHz");
    }
    print_kv_u64("CabinInfo PID:", cabin_info()->pid, "");

    print_header();

    /* Cheap operations: many iters for tight stats */
    bench_run("rdtsc-pair overhead", b_rdtsc_overhead, NULL, 2000);
    bench_run("cabin_info() read (no syscall)", b_cabin_info, NULL, 2000);

    /* Pure syscall costs */
    bench_run("yield (notify pocket)", b_yield, NULL, 2000);

    /* Manifest dispatch costs: back to back, and after 2 ms of quiet — the
     * K-Core asleep, woken by the call */
    bench_run("1-op Manifest (ops.fill)", b_least_call, NULL, 1000);
    bench_run_rested("1-op Manifest after 2 ms quiet", b_least_call, NULL, 500, 2000);
    bench_call_stages("1-op Manifest, its stages:", 1000, 0);
    bench_call_stages("1-op Manifest after 2 ms quiet, its stages:", 500, 2000);

    /* One strand waiting on another's work, through the kernel */
    bench_pingpong("strand ping-pong (park/wake, rtt)", 1000);

    /* IPC */
    bench_run("display PING + reply (cross-proc IPC)", b_display_ping, NULL, 500);

    /* Touch — tag-multicast event system. Compare to Linux signal /
     * signalfd: kill(self)+sigaction ≈ 0.5-3 µs there. Touch carries an
     * arbitrary payload, is broadcast, and is capability-checked, so it
     * does strictly more work than a 1-bit signal — but the fast path
     * should still be the same order of magnitude. */
    bench_run("touch_send (publish only, self-sub)",  b_touch_publish_only, NULL, 1000);
    bench_run("touch_send + touch_await (rtt)",       b_touch_self_rtt,     NULL, 1000);
    bench_run("touch_await fast-path (pre-queued)",   b_touch_await_fastpath, NULL, 1000);

    /* Bulk Manifest — divide by BULK_OPS for per-op */
    bench_run("chain 1000-op Manifest (1 syscall)", b_chain_bulk, NULL, 50);

    /* Slow path: storage. Keep iters modest so we don't overrun the
     * journal. */
    bench_run("create+write64+delete (TagFS+disk)", b_file_cycle, NULL, 50);

    println("");
    println("notes:");
    println("  - 'min' is the cleanest sample (no IRQ during the window)");
    println("  - chain median / 1000 ≈ per-Manifest-op amortised cost");
    println("  - 'after 2 ms quiet': the K-Core asleep, woken by the call itself");
    println("  - App Cores have no tick: max is what else ran meanwhile");
    println("  - stages: a kernel built MEASURE=on stamps when the call came to the");
    println("    gate, was taken and was answered; the rows are absent otherwise");
    return 0;
}
