/*
 * strandtest — proves the Strands stack end-to-end (P4 go-live + P5a
 * per-strand IPC rings + TLS).
 *
 *   test1 (P4 baseline): spawn ONE worker strand into the caller's cabin and
 *     exercise shared-address-space atomics + park→wake. main and the worker
 *     each do ITERS atomic increments of one shared counter (== 2*ITERS proves
 *     real concurrency with no lost updates); main parks on a done flag and the
 *     worker wakes it.
 *
 *   test2 (P5a): spawn N worker strands that run CONCURRENT IPC syscalls. Each
 *     worker confirms its per-strand TLS (strand_self() == a real distinct pid)
 *     and per-strand IPC routing (MCALLS × proc_info(self) — every reply must
 *     come back to THIS strand's ResultRing, so info.pid echoes the queried
 *     pid). With the P4 shared-cabin rings this raced (replies misrouted /
 *     lost / #PF); with per-strand rings each strand is the sole owner of its
 *     rings + stashes, so the proven SPSC algorithms hold under concurrency.
 *
 * Emits exactly one "[STRAND] PASS" on success (the matrix greps for it).
 *
 * Strands require FSGSBASE (per-strand TLS detection uses ring-3 RDFSBASE).
 * The STRICT matrix (-cpu max) and `make run` (-cpu qemu64,+fsgsbase) both
 * provide it; on a CPU without it the test SKIPs cleanly instead of failing.
 */

#include "box/print.h"
#include "box/system.h"            /* exit, proc_info, proc_info_t */
#include "box/clock.h"             /* clock_uptime_ms — the ClockBoard, no syscall */
#include "box/sync.h"              /* addr_park, addr_wake */
#include "box/strand.h"            /* strand_spawn */
#include "box/cpu.h"               /* cpu_has_fsgsbase */
#include "box/core/strand_self.h"  /* strand_self */
#include "box/error.h"

#define ITERS     100000u   /* big enough that the two strands' runs overlap   */
#define NWORKERS  4u        /* concurrent IPC workers in test2                 */
#define MCALLS    500u      /* syscalls per worker (routing stress)            */

/* How long ANY join in this file lets the machine stand STILL before it
 * convicts. Not a budget for the work — any progress restarts it — only for
 * the absence of any.
 *
 * The figure is the one these tests always meant: the caps it replaces were
 * parks of 200 ms counted out 50 times in test1 and 80 in test2 and test3 —
 * ten to sixteen seconds of nothing happening. Counted in parks that was a lie
 * in both directions: a park broken early by a spurious wake spent a turn
 * without spending the time, so "sixteen seconds" could be gone in
 * milliseconds on a loaded host, while a machine genuinely stuck spent the
 * full sixteen anyway. Measured on the ClockBoard it is sixteen real seconds —
 * the longest of the three, for every one of them, since a join that finishes
 * is never charged for it. */
#define CHURN_STILL_MS  16000u

/* ‼ Every budget in this file is ELAPSED TIME, and that is the right shape —
 * a turn count is not time, and a park broken early by a spurious wake spends
 * a turn without spending a millisecond. But it buys that correctness with a
 * dependency the old counts did not have: a count always runs out, a clock
 * only runs out while it is running. On a machine whose ClockBoard has stopped,
 * `clock_uptime_ms() - started` is forever zero, no budget can ever expire, and
 * a loop that should have judged the machine spins on it instead — burning
 * every core it can reach, which is the one failure a parked strand never has
 * and Nightwatch therefore cannot convict.
 *
 * The witness costs nothing and needs no number. addr_park returns ERR_TIMEOUT
 * only when it slept to its deadline, and sleeping to a deadline is itself
 * proof that that much real time passed. A clock that did not move across one
 * such park is not a slow clock; it is a stopped one. ONE observation settles
 * it — there is no tally here and none is wanted — and the test then reports
 * the defect it actually found (the clock) instead of the one it was looking
 * for (the reaper, the workers), which is the whole difference between a
 * harness that judges and a harness that hangs. */
static bool clock_stopped_across(error_t park_rc, uint64_t before)
{
    return park_rc == ERR_TIMEOUT && clock_uptime_ms() == before;
}

#define STRAND_CLOCK_CHECK(which, rc, t0)                                     \
    do {                                                                      \
        if (clock_stopped_across((rc), (t0))) {                               \
            printf("[STRAND] FAIL %s: the machine's clock is not advancing — " \
                   "a park slept its full deadline and uptime did not move; "  \
                   "no elapsed-time budget in this test can ever expire\n",    \
                   (which));                                                  \
            return -1;                                                        \
        }                                                                     \
    } while (0)

/* ---- test1: P4 baseline — spawn + concurrent atomics + park→wake ---------- */

static uint64_t g_counter = 0;   /* incremented by BOTH strands (atomic)       */
static uint64_t g_witness = 0;   /* worker sets it — proves it ran + shares AS */
static uint64_t g_done    = 0;   /* worker-done flag; also the park address    */

static void worker1(void *arg)
{
    (void)arg;
    __atomic_store_n(&g_witness, 0xCAFEu, __ATOMIC_RELAXED);
    for (uint32_t i = 0; i < ITERS; i++)
        __atomic_fetch_add(&g_counter, 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_done, 1u, __ATOMIC_RELEASE);
    addr_wake(&g_done, 0);
}

static int test1(void)
{
    g_counter = 0; g_witness = 0; g_done = 0;

    uint32_t pid = strand_spawn(worker1, 0);
    if (pid == 0) {
        printf("[STRAND] FAIL test1: strand_spawn returned 0\n");
        return -1;
    }

    for (uint32_t i = 0; i < ITERS; i++)
        __atomic_fetch_add(&g_counter, 1u, __ATOMIC_RELAXED);

    /* Park until the worker signals done. The park IS the sleep and is
     * lost-wake-safe (a store that landed before we parked makes it return
     * VALUE_MISMATCH without sleeping); what convicts is the CLOCK saying
     * nothing has moved for CHURN_STILL_MS, not a tally of how many times we
     * parked.
     *
     * The progress being watched is the COUNTER, not the flag. main's own ITERS
     * are already spent by the time we get here, so every further increment is
     * the worker's and proves it is running — a worker still grinding through
     * 100000 adds on a starved core restarts the clock and is never convicted,
     * while a worker that never ran leaves the counter exactly where main left
     * it and the silence names it. */
    uint64_t started = clock_uptime_ms();
    uint64_t seen    = __atomic_load_n(&g_counter, __ATOMIC_ACQUIRE);
    while (__atomic_load_n(&g_done, __ATOMIC_ACQUIRE) == 0) {
        uint64_t t0 = clock_uptime_ms();
        error_t  pr = addr_park(&g_done, 0, 200);
        STRAND_CLOCK_CHECK("test1", pr, t0);
        uint64_t now = __atomic_load_n(&g_counter, __ATOMIC_ACQUIRE);
        if (now != seen) { seen = now; started = clock_uptime_ms(); }  /* it moved */
        else if (clock_uptime_ms() - started >= CHURN_STILL_MS) {
            printf("[STRAND] FAIL test1: worker never finished (%u ms with no progress)\n",
                   (unsigned)CHURN_STILL_MS);
            return -1;
        }
    }

    uint64_t total   = __atomic_load_n(&g_counter, __ATOMIC_ACQUIRE);
    uint64_t witness = __atomic_load_n(&g_witness, __ATOMIC_ACQUIRE);
    if (total != 2ull * ITERS || witness != 0xCAFEu) {
        printf("[STRAND] FAIL test1: counter=%u (want %u) witness=%u\n",
               (unsigned)total, (unsigned)(2u * ITERS), (unsigned)witness);
        return -1;
    }

    printf("[STRAND] test1 OK: spawn + atomics (2 x %u) + park->wake (worker pid %u)\n",
           ITERS, pid);
    return 0;
}

/* ---- test2: N concurrent IPC-worker strands — per-strand rings + TLS ------ */

static volatile uint64_t g_remaining;          /* workers decrement; main parks */
static volatile uint32_t g_worker_fail;        /* set by any worker on mismatch */
static uint32_t          g_worker_self[NWORKERS]; /* strand_self() per worker   */

static void ipc_worker(void *arg)
{
    uint32_t idx  = (uint32_t)(uintptr_t)arg;
    uint32_t self = strand_self();
    g_worker_self[idx] = self;

    if (self == 0) {
        /* TLS proof failed: a spawned strand must see its own (non-cabin) pid. */
        __atomic_store_n(&g_worker_fail, 1u, __ATOMIC_RELAXED);
    } else {
        /* Routing proof: every proc_info(self) reply must return on THIS
         * strand's ResultRing — info.pid echoes the queried pid. A shared-ring
         * misroute would surface a sibling's reply (info.pid != self). */
        for (uint32_t i = 0; i < MCALLS; i++) {
            proc_info_t info;
            if (proc_info((uint16_t)self, &info) != 0 ||
                info.pid != (uint16_t)self) {
                __atomic_store_n(&g_worker_fail, 1u, __ATOMIC_RELAXED);
                break;
            }
        }
    }

    __atomic_sub_fetch(&g_remaining, 1u, __ATOMIC_RELEASE);
    addr_wake(&g_remaining, 0);
}

static int test2(void)
{
    g_remaining   = NWORKERS;
    g_worker_fail = 0;
    for (uint32_t i = 0; i < NWORKERS; i++)
        g_worker_self[i] = 0;

    for (uint32_t i = 0; i < NWORKERS; i++) {
        uint32_t pid = strand_spawn(ipc_worker, (void *)(uintptr_t)i);
        if (pid == 0) {
            printf("[STRAND] FAIL test2: strand_spawn #%u returned 0\n", i);
            return -1;
        }
    }

    /* Join: park until every worker has decremented g_remaining. Re-read the
     * live value before each park so a decrement we missed returns
     * ERR_ADDR_VALUE_MISMATCH immediately (lost-wake-safe). Each decrement is
     * a worker finishing its MCALLS, so the count falling IS the progress and
     * it restarts the clock; only CHURN_STILL_MS in which not one of the four
     * moved convicts — three workers that finish while the fourth grinds
     * through a contended ring buy the fourth its full margin again. */
    uint64_t started = clock_uptime_ms();
    uint64_t cur;
    while ((cur = __atomic_load_n(&g_remaining, __ATOMIC_ACQUIRE)) != 0) {
        uint64_t t0 = clock_uptime_ms();
        error_t  pr = addr_park(&g_remaining, cur, 200);
        STRAND_CLOCK_CHECK("test2", pr, t0);
        if (__atomic_load_n(&g_remaining, __ATOMIC_ACQUIRE) != cur)
            started = clock_uptime_ms();          /* it moved — begin again */
        else if (clock_uptime_ms() - started >= CHURN_STILL_MS) {
            printf("[STRAND] FAIL test2: %u worker(s) stuck (%u ms with no progress)\n",
                   (unsigned)cur, (unsigned)CHURN_STILL_MS);
            return -1;
        }
    }

    if (__atomic_load_n(&g_worker_fail, __ATOMIC_ACQUIRE) != 0) {
        printf("[STRAND] FAIL test2: a worker reported a TLS/routing mismatch\n");
        return -1;
    }

    /* Every worker saw a real, DISTINCT pid (each strand is its own context). */
    for (uint32_t i = 0; i < NWORKERS; i++) {
        if (g_worker_self[i] == 0) {
            printf("[STRAND] FAIL test2: worker %u never recorded a pid\n", i);
            return -1;
        }
        for (uint32_t j = i + 1; j < NWORKERS; j++) {
            if (g_worker_self[i] == g_worker_self[j]) {
                printf("[STRAND] FAIL test2: duplicate strand pid %u\n", g_worker_self[i]);
                return -1;
            }
        }
    }

    printf("[STRAND] test2 OK: %u concurrent IPC workers x %u proc_info calls, "
           "distinct pids, routed clean\n", NWORKERS, MCALLS);
    return 0;
}

/* ---- test3: strand reaper — exited strands are reclaimed at runtime ------- */

#define CHURN 100u   /* well below MAX_PROCESSES; proof is "count returns to base" */

/* The park that IS the sleep between two looks at the reaper's work. Nothing in
 * userspace can be woken by the reaper — it runs in the kernel and publishes no
 * address — so this word is never woken and the park always runs to its own
 * length. That is the point: the kernel holds the core for those 200 ms, where
 * the `yield()` this replaces handed it back in under a microsecond and spent no
 * time at all. Its length only decides how often we ask, never what we decide. */
#define REAPER_LOOK_MS  200u

static volatile uint64_t g_churn_remaining;
static volatile uint64_t g_reaper_sleep;   /* park address only; never woken */

static void noop_worker(void *arg)
{
    (void)arg;
    __atomic_sub_fetch(&g_churn_remaining, 1u, __ATOMIC_RELEASE);
    addr_wake(&g_churn_remaining, 0);
}

static int test3(void)
{
    system_info_t si;
    if (sysinfo(&si) != 0) {
        printf("[STRAND] test3 SKIP: sysinfo unavailable\n");
        return 0;
    }

    /* Let any test1/test2 corpses settle so the baseline is clean. */
    for (int k = 0; k < 16; k++) yield();
    if (sysinfo(&si) != 0) return 0;
    uint32_t base = si.process_count;

    /* Churn: spawn+join CHURN strands. Each exits → becomes a corpse the
     * reaper must reclaim. Without a runtime reaper, process_count would climb
     * to base+CHURN and stay there (and eventually exhaust the table). */
    for (uint32_t r = 0; r < CHURN; r++) {
        g_churn_remaining = 1;
        uint32_t pid = strand_spawn(noop_worker, 0);
        if (pid == 0) {
            printf("[STRAND] FAIL test3: strand_spawn failed at round %u (table exhausted — reaper not reclaiming)\n", r);
            return -1;
        }
        /* Join on the worker's own decrement. The park is the sleep and is
         * lost-wake-safe (a decrement we missed returns VALUE_MISMATCH at park
         * time); what convicts is the CLOCK saying the count has not moved for
         * CHURN_STILL_MS, not a tally of how many times we parked. */
        uint64_t started = clock_uptime_ms();
        uint64_t cur;
        while ((cur = __atomic_load_n(&g_churn_remaining, __ATOMIC_ACQUIRE)) != 0) {
            uint64_t t0 = clock_uptime_ms();
            error_t  pr = addr_park(&g_churn_remaining, cur, 200);
            STRAND_CLOCK_CHECK("test3", pr, t0);
            if (__atomic_load_n(&g_churn_remaining, __ATOMIC_ACQUIRE) != cur)
                started = clock_uptime_ms();          /* it moved — begin again */
            else if (clock_uptime_ms() - started >= CHURN_STILL_MS) {
                printf("[STRAND] FAIL test3: join stuck at round %u (%u ms with no progress)\n",
                       r, (unsigned)CHURN_STILL_MS);
                return -1;
            }
        }
    }

    /* process_count must return toward baseline — the CHURN exited strands
     * were process_destroy'd by the reaper (process_count-- happens only
     * there). A broken reaper leaves it pinned near base+CHURN. */
    uint32_t cnt = base + CHURN;
    if (sysinfo(&si) == 0) cnt = si.process_count;
    uint32_t best    = cnt;                    /* the lowest count seen so far */
    uint64_t started = clock_uptime_ms();
    while (cnt > base + 4u) {
        if (clock_uptime_ms() - started >= CHURN_STILL_MS) {
            printf("[STRAND] FAIL test3: process_count stuck at %u (base %u, churned %u) — reaper not reclaiming\n",
                   cnt, base, CHURN);
            return -1;
        }
        uint64_t t0 = clock_uptime_ms();
        error_t  pr = addr_park(&g_reaper_sleep, 0, REAPER_LOOK_MS);
        STRAND_CLOCK_CHECK("test3", pr, t0);
        if (sysinfo(&si) == 0) cnt = si.process_count;
        if (cnt < best) { best = cnt; started = clock_uptime_ms(); }  /* it fell — begin again */
    }

    printf("[STRAND] test3 OK: %u spawn+join churn reclaimed (process_count base %u -> %u)\n",
           CHURN, base, cnt);
    return 0;
}

/* ---- test4: a timed park is broken EARLY by a concurrent wake (Ф20d fix) ---
 * Pre-fix, a timed addr_park slept to its FULL deadline because addr_wake never
 * wrote the channel result_wait monitors (it only rescheduled the waiter into a
 * futile poll). Proof — host-timing independent AND race-free: main does ONE
 * long (5 s) park whose watched value NEVER changes, so the ONLY thing that can
 * end it before the deadline is a real wake delivering a completion Result. A
 * worker wakes REPEATEDLY until main reports done, which closes the spawn-vs-park
 * ordering window (an early wake that lands before main parks is a harmless
 * no-op; the next one delivers). addr_park must return OK (woken), never
 * ERR_TIMEOUT (slept to deadline) — that return value alone distinguishes the
 * fix from the bug, with no wall-clock measurement to flake on. */

static volatile uint64_t g_t4_flag;   /* park address; stays 0 — pure notify, no value change */
static volatile uint64_t g_t4_done;   /* main sets it when its park returns; stops the waker  */

static void wake_worker(void *arg)
{
    (void)arg;
    uint32_t guard = 0;
    while (__atomic_load_n(&g_t4_done, __ATOMIC_ACQUIRE) == 0) {
        addr_wake(&g_t4_flag, 0);
        if (++guard > 200000u) break;   /* never wedge the harness on a regression */
        yield();
    }
}

static int test4(void)
{
    g_t4_flag = 0;
    g_t4_done = 0;

    uint32_t pid = strand_spawn(wake_worker, 0);
    if (pid == 0) {
        printf("[STRAND] FAIL test4: strand_spawn returned 0\n");
        return -1;
    }

    /* One long park on a value that never changes. OK proves a concurrent wake
     * broke it early; ERR_TIMEOUT means the wake never reached result_wait. */
    error_t rc = addr_park(&g_t4_flag, 0, 5000);
    __atomic_store_n(&g_t4_done, 1u, __ATOMIC_RELEASE);

    if (rc == ERR_TIMEOUT) {
        printf("[STRAND] FAIL test4: 5s timed park slept to deadline — wake did not break it early (Ф20d bug)\n");
        return -1;
    }
    if (rc != OK) {
        printf("[STRAND] FAIL test4: addr_park returned %d (want OK)\n", (int)rc);
        return -1;
    }

    printf("[STRAND] test4 OK: concurrent wake broke a 5s timed park early (addr_park returned OK)\n");
    return 0;
}

int main(void)
{
    printf("[STRAND] strandtest start\n");

    /* Strands require FSGSBASE for per-strand TLS (ring-3 RDFSBASE). Without
     * it strand_spawn refuses, so SKIP cleanly rather than report a failure. */
    if (!cpu_has_fsgsbase()) {
        printf("[STRAND] SKIP: strands require FSGSBASE "
               "(run under STRICT or qemu64,+fsgsbase)\n");
        exit(0);
    }

    if (test1() != 0) exit(1);
    if (test2() != 0) exit(1);
    if (test3() != 0) exit(1);
    if (test4() != 0) exit(1);

    printf("[STRAND] PASS: test1 + test2 + test3 + test4 (per-strand rings + TLS + park/wake + reaper + timed-wake)\n");
    exit(0);
    return 0;
}
