
#include "box/print.h"
#include "box/cpu.h"
#include "box/system.h"
#include "box/time.h"
#include "box/vga.h"
#include "box/file.h"
#include "box/string.h"
#include "box/convert.h"
#include "box/ipc.h"
#include "box/core/notify.h"
#include "box/core/manifest.h"
#include "box/core/crate.h"
#include "box/core/cabin.h"
#include "box/core/result.h"
#include "box/error.h"
#include "box/display.h"
#include "box/touch.h"


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


typedef int (*bench_fn)(uint32_t iter, void *ctx);

#define WARMUP_ITERS 64

static bench_stats_t bench_run_raw(bench_fn fn, void *ctx, uint32_t n)
{
    bench_stats_t st = {0};
    if (n > BENCH_MAX_SAMPLES)
        n = BENCH_MAX_SAMPLES;

    for (uint32_t i = 0; i < WARMUP_ITERS; i++)
    {
        if (fn(i, ctx) < 0)
        {
            st.ok = false;
            st.err = -1;
            return st;
        }
    }

    for (uint32_t i = 0; i < n; i++)
    {
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
    bench_stats_t s = bench_run_raw(fn, ctx, iters);
    print_row(name, &s);
}


static int b_rdtsc_overhead(uint32_t i, void *ctx)
{
    (void)i;
    (void)ctx;
    return 0;
}

static int b_cabin_info(uint32_t i, void *ctx)
{
    (void)i;
    (void)ctx;
    volatile CabinInfo *ci = cabin_info();
    if ((volatile uint32_t)ci->magic == 0)
        return 0;
    return 0;
}

static int b_yield(uint32_t i, void *ctx)
{
    (void)i;
    (void)ctx;
    yield();
    return 0;
}

static int b_uptime_ms(uint32_t i, void *ctx)
{
    (void)i;
    (void)ctx;
    uint64_t ms = 0;
    return time_uptime_ms(&ms);
}

static int b_vga_setcolor(uint32_t i, void *ctx)
{
    (void)ctx;
    return vga_setcolor_rgb((i & 1) ? COLOR_LIGHT_GRAY : COLOR_WHITE,
                            COLOR_BLACK);
}

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

#define BULK_OPS 1000
static uint8_t s_bulk_mbuf[16 + BULK_OPS * 13 + 64];
static uint8_t s_bulk_buf[64];
static bool s_bulk_built = false;

#define DECK_OPS_FILL 0x02

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
            if (ManifestBuilderAddOp(&mb, DECK_OPERATIONS, DECK_OPS_FILL, 0,
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
    Touch t;
    while (touch_await(s_rtt_tag, &t, 1) == 0) {  }
}

static int b_touch_publish_only(uint32_t i, void *ctx)
{
    (void)ctx;
    if (bench_setup_touch() != 0) return -1;
    if ((i & 15) == 0) bench_drain_touches();
    uint32_t payload = i;
    return touch_send(s_rtt_pair, &payload, sizeof(payload), 0);
}

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

static int b_touch_await_fastpath(uint32_t i, void *ctx)
{
    (void)i;
    (void)ctx;
    if (bench_setup_touch() != 0) return -1;
    uint32_t payload = i;
    int rc = touch_send(s_rtt_pair, &payload, sizeof(payload), 0);
    if (rc < 0) return rc;
    Touch t;
    return touch_await(s_rtt_tag, &t, 100);
}

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

    int file_id = create(name, "");
    if (file_id < 0)
        return file_id;

    static const uint8_t payload[64] = {0};
    int wrc = fwrite((uint32_t)file_id, 0, payload, sizeof(payload));
    if (wrc < 0)
    {
        delete((uint32_t)file_id);
        return wrc;
    }

    int drc = delete((uint32_t)file_id);
    if (drc < 0)
        return drc;
    return 0;
}


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
    io_set_mode(IO_MODE_VGA);

    println("BoxOS bench v1 — RDTSC microbenchmarks");

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

    bench_run("rdtsc-pair overhead", b_rdtsc_overhead, NULL, 2000);
    bench_run("cabin_info() read (no syscall)", b_cabin_info, NULL, 2000);

    bench_run("yield (notify pocket)", b_yield, NULL, 2000);

    bench_run("vga_setcolor (HW deck min)", b_vga_setcolor, NULL, 1000);
    bench_run("time_uptime_ms (HW deck)", b_uptime_ms, NULL, 1000);

    bench_run("display PING + reply (cross-proc IPC)", b_display_ping, NULL, 500);

    bench_run("touch_send (publish only, self-sub)",  b_touch_publish_only, NULL, 1000);
    bench_run("touch_send + touch_await (rtt)",       b_touch_self_rtt,     NULL, 1000);
    bench_run("touch_await fast-path (pre-queued)",   b_touch_await_fastpath, NULL, 1000);

    bench_run("chain 1000-op Manifest (1 syscall)", b_chain_bulk, NULL, 50);

    bench_run("create+write64+delete (TagFS+disk)", b_file_cycle, NULL, 50);

    println("");
    println("notes:");
    println("  - 'min' is the cleanest sample (no IRQ during the window)");
    println("  - chain median / 1000 ≈ per-Manifest-op amortised cost");
    println("  - IRQ tick is 500 Hz → max ≈ med + ~2 ms when an IRQ lands");
    return 0;
}