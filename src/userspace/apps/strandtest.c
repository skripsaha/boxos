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
 *   test5..test9: a program ends whole — see the block above test5.
 *   `strandtest crash` runs the crash case alone.
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
#include "box/core/cabin.h"        /* cabin_info — the child's spawner, the parent's pid */
#include "box/error.h"
#include "box/ipc.h"               /* send, receive_wait — parent and child speak */
#include "box/touch.h"             /* strand:exited — the reaper's word on each strand */
#include "box/luggage.h"           /* luggage_word — a child's role rides in its cabin */
#include "box/memory.h"            /* malloc, free — the workers' heap churn */
#include "box/string.h"
#include "proc_exit.h"             /* PROC_EXIT_KILLED, PROC_EXIT_CRASHED */

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

/* ---- test5..test9 (+ "strandtest crash"): a program ends whole ------------
 *
 * A program is its cabin, every strand of it. Killed from outside, main
 * returning, exit() from any strand, a strand that crashes — each ends all of
 * them, the main strand announced last, so whoever waits on the pid the program
 * was started as finds it wholly gone. A strand ends only
 * itself (strand_exit), and may not end a sibling: an end that lands inside a
 * strand's malloc keeps the program's heap lock for ever, and every strand
 * left alive would spin on it.
 *
 * Each test starts a child — this program again, its role in its luggage —
 * that spawns CREW_WORKERS workers churning the program's heap, plus one
 * joinable strand that finishes at once and is never joined. The crew is read
 * by the tag the child wears (proc_crew walks every strand of every cabin
 * wearing it), the end is made, the strand it came through and then main are
 * waited for as a STATE (process_gone), and at that moment the crew must be
 * empty. No timer
 * decides any verdict; the one quiet window below only stops a parent from
 * waiting on a child that never speaks.
 *
 * The crash case is its own command: every other harness reads a user-mode
 * [EXCEPTION] as a failure, so the default run cannot carry one. */

#define CREW_TAG          "strandtest:crew"
#define CREW_LINE(role)   "strandtest crew " role
#define CREW_WORKERS      3u
#define CREW_MAIN_CODE    0x21   /* main's return in the "exit" and "sibling" roles */
#define CREW_WORKER_CODE  0x22   /* worker 0's exit() in the "wexit" role           */
#define CREW_RACE_ROUNDS  4u     /* kills landing on a crew that never stops spawning */
#define CREW_ANY_CODE     INT32_MIN   /* the race's sparks end either way — not judged */

/* How long the ring may say nothing at all before a parent stops waiting for
 * its child's word — touch_test's figure for the same wait. Any message
 * restarts it; only a machine gone quiet runs it down. */
#define CREW_QUIET_MS     30000u

enum { CREW_KILL, CREW_EXIT, CREW_WEXIT, CREW_CRASH, CREW_SIBLING, CREW_RACE };

static volatile uint32_t  g_crew_role;
static volatile uint64_t  g_crew_up;       /* workers begun; main parks on it      */
static volatile uint64_t  g_crew_still;    /* park address only; never woken       */
static volatile uint32_t  g_crew_go;       /* the parent's word has come           */
static uint32_t           g_crew_pid[CREW_WORKERS];
static uint32_t *volatile g_crew_nowhere;  /* stays NULL: the crash role writes it */

static void crew_spark(void *arg)  { (void)arg; }
static void crew_zombie(void *arg) { (void)arg; }

static void crew_worker(void *arg)
{
    const uint32_t index = (uint32_t)(uintptr_t)arg;
    __atomic_add_fetch(&g_crew_up, 1u, __ATOMIC_RELEASE);
    addr_wake(&g_crew_up, 0);

    for (;;) {
        /* The program's heap, kept busy, so the end lands inside malloc and
         * free as often as anywhere. */
        void *p = malloc(48u + 16u * index);
        if (p) { memset(p, (int)index, 16); free(p); }

        if (g_crew_role == CREW_RACE) (void)strand_spawn(crew_spark, 0);

        if (index == 0 && __atomic_load_n(&g_crew_go, __ATOMIC_ACQUIRE)) {
            if (g_crew_role == CREW_WEXIT)   exit(CREW_WORKER_CODE);
            if (g_crew_role == CREW_CRASH)   *g_crew_nowhere = 1;
            if (g_crew_role == CREW_SIBLING) strand_exit();
        }
        yield();
    }
}

static void crew_park_for_ever(void)
{
    for (;;) (void)addr_park(&g_crew_still, 0, 0);
}

static void crew_await_parent(uint32_t parent)
{
    Result r;
    for (;;)
        if (receive_wait(&r, CREW_QUIET_MS) && r.sender_pid == parent) return;
}

/* The child: its crew up, its report sent, then the role's end. */
static int crew_child(const char *word)
{
    static const struct { const char *word; uint32_t role; } roles[] = {
        { "kill", CREW_KILL }, { "exit", CREW_EXIT }, { "wexit", CREW_WEXIT },
        { "crash", CREW_CRASH }, { "sibling", CREW_SIBLING }, { "race", CREW_RACE },
    };
    uint32_t role = UINT32_MAX;
    for (uint32_t i = 0; i < sizeof(roles) / sizeof(roles[0]); i++)
        if (word && strcmp(word, roles[i].word) == 0) role = roles[i].role;
    if (role == UINT32_MAX) {
        printf("[STRAND] FAIL crew: unknown role '%s'\n", word ? word : "");
        return 255;
    }
    g_crew_role = role;
    const uint32_t parent = cabin_info()->spawner_pid;

    uint32_t zombie = strand_spawn_joinable(crew_zombie, 0);
    if (zombie == 0) return 254;   /* the parent hears no report */
    for (uint32_t i = 0; i < CREW_WORKERS; i++) {
        g_crew_pid[i] = strand_spawn(crew_worker, (void *)(uintptr_t)i);
        if (g_crew_pid[i] == 0) return 254;   /* the parent hears no report */
    }
    uint64_t up;
    while ((up = __atomic_load_n(&g_crew_up, __ATOMIC_ACQUIRE)) < CREW_WORKERS)
        (void)addr_park(&g_crew_up, up, 200);

    /* Who the end will come through, and the strand nobody will join. */
    const bool worker_ends = role == CREW_WEXIT || role == CREW_CRASH ||
                             role == CREW_SIBLING;
    uint32_t report[2] = { worker_ends ? g_crew_pid[0] : cabin_info()->pid, zombie };
    send(parent, report, sizeof(report));

    if (role == CREW_KILL || role == CREW_RACE) crew_park_for_ever();

    crew_await_parent(parent);
    if (role == CREW_EXIT) return CREW_MAIN_CODE;
    if (role == CREW_SIBLING) {
        int32_t refused = proc_kill(g_crew_pid[1]);
        __atomic_store_n(&g_crew_go, 1u, __ATOMIC_RELEASE);
        send(parent, &refused, sizeof(refused));
        crew_await_parent(parent);
        return CREW_MAIN_CODE;
    }
    __atomic_store_n(&g_crew_go, 1u, __ATOMIC_RELEASE);
    crew_park_for_ever();
    return 0;
}

/* The parent's side. */

typedef struct {
    uint32_t  main_pid, main_gen;
    uint32_t  actor;     /* the strand the end comes through */
    uint32_t  zombie;
    ProcMate *mates;     /* the crew as it stood before the end */
    int       count;
} Crew;

static bool crew_hear(uint32_t from, void *out, uint32_t len)
{
    uint64_t last = clock_uptime_ms();
    for (;;) {
        Result r;
        if (receive_wait(&r, CREW_QUIET_MS)) {
            last = clock_uptime_ms();
            if (r.sender_pid == from && r.data_length >= len && r.data_addr != 0) {
                memcpy(out, (const void *)(uintptr_t)r.data_addr, len);
                return true;
            }
        } else if (clock_uptime_ms() - last >= CREW_QUIET_MS) {
            return false;
        }
    }
}

/* Who wears the crew's tag and is alive (proc_crew drops the dead), or -1. */
static int crew_now(ProcMate **mates)
{
    uint32_t total = 0;
    *mates = NULL;
    int n = proc_crew(CREW_TAG, mates, &total);
    if (n < 0 || (uint32_t)n != total) { free(*mates); *mates = NULL; return -1; }
    return n;
}

/* Whatever of a crew is still alive, ended one strand at a time — what a
 * failed test owes the machine it leaves. */
static void crew_scrap(void)
{
    ProcMate *m;
    int n = crew_now(&m);
    for (int i = 0; i < n; i++) (void)proc_finish(m[i].pid, m[i].generation);
    free(m);
}

/* The strands a verdict found alive, named from the same look that found
 * them — a later look may no longer see what this one saw. */
static void crew_name(const ProcMate *m, int n, uint32_t skip, uint32_t main_pid)
{
    for (int i = 0; i < n; i++)
        if (m[i].pid != skip)
            printf("[STRAND]   aboard: strand %u%s in state %u\n", m[i].pid,
                   m[i].pid == main_pid ? " (main)" : "", m[i].state);
}

static uint32_t crew_gen_of(const Crew *c, uint32_t pid)
{
    for (int i = 0; i < c->count; i++)
        if (c->mates[i].pid == pid) return c->mates[i].generation;
    return 0;
}

static void crew_forget(Crew *c, uint32_t pid)
{
    for (int i = 0; i < c->count; i++)
        if (c->mates[i].pid == pid) { c->mates[i] = c->mates[--c->count]; return; }
}

static int crew_start(const char *test, const char *line, Crew *c)
{
    memset(c, 0, sizeof(*c));
    int pid = proc_exec_gen(line, CREW_TAG, &c->main_gen);
    if (pid <= 0) {
        printf("[STRAND] FAIL %s: could not start '%s' (%d)\n", test, line, pid);
        return -1;
    }
    c->main_pid = (uint32_t)pid;

    uint32_t report[2];
    if (!crew_hear(c->main_pid, report, sizeof(report))) {
        printf("[STRAND] FAIL %s: the child never said its crew was up\n", test);
        crew_scrap();
        return -1;
    }
    c->actor  = report[0];
    c->zombie = report[1];

    /* main and the workers — and the never-joined one, if it has not yet
     * finished. */
    c->count = crew_now(&c->mates);
    if (c->count < (int)(1 + CREW_WORKERS) || crew_gen_of(c, c->actor) == 0) {
        printf("[STRAND] FAIL %s: a crew of %d without its strand %u, want %u or more\n",
               test, c->count, c->actor, 1 + CREW_WORKERS);
        crew_scrap();
        return -1;
    }
    return 0;
}

/* Every strand of the ended program announced its end, each with the end it
 * shared — want_actor for the strand it came through, want_rest for the rest.
 * Read from process:died, matched by pid AND generation: process_gone cannot
 * say it, since an end already committed is answered without its code. The
 * never-joined strand ended itself before any of it and is not judged. */
static int crew_deaths(const char *test, TouchTag died, const Crew *c,
                       int32_t want_actor, int32_t want_rest)
{
    bool *heard = (bool *)malloc((size_t)c->count + 1);
    if (!heard) {
        printf("[STRAND] FAIL %s: no memory to follow the crew's deaths\n", test);
        return -1;
    }
    uint32_t missing = 0;
    for (int i = 0; i < c->count; i++) {
        heard[i] = c->mates[i].pid == c->zombie;
        if (!heard[i]) missing++;
    }

    int      rc   = 0;
    uint64_t last = clock_uptime_ms();
    while (rc == 0 && missing != 0) {
        Touch t;
        if (touch_await(died, &t, CREW_QUIET_MS) == 0) {
            last = clock_uptime_ms();
            TouchProcessDied d;
            if (t.payload_len < sizeof(d)) continue;
            memcpy(&d, t.payload, sizeof(d));
            for (int i = 0; i < c->count; i++) {
                const ProcMate *m = &c->mates[i];
                if (heard[i] || m->pid != d.pid || m->generation != d.generation) continue;
                heard[i] = true;
                missing--;
                int32_t want = m->pid == c->actor ? want_actor : want_rest;
                if (want != CREW_ANY_CODE && d.exit_code != want) {
                    printf("[STRAND] FAIL %s: strand %u ended with %d, want %d\n",
                           test, m->pid, (int)d.exit_code, (int)want);
                    rc = -1;
                }
            }
        } else if (clock_uptime_ms() - last >= CREW_QUIET_MS) {
            printf("[STRAND] FAIL %s: %u strand(s) of the ended program never announced "
                   "their end\n", test, missing);
            rc = -1;
        }
    }
    free(heard);
    return rc;
}

/* The end has been made through c->actor. Wait for that strand as a state:
 * every worker is gone by then, and main — announced last — is the only one
 * that may still be on its way, so it is waited for next. Then nothing of the
 * program may be alive, and every strand announced the end it shared. */
static int crew_judge(const char *test, Crew *c, TouchTag died,
                      int32_t want_actor, int32_t want_rest)
{
    if (process_gone(c->actor, crew_gen_of(c, c->actor), NULL) != 0) {
        printf("[STRAND] FAIL %s: the strand %u the end came through cannot be waited for\n",
               test, c->actor);
        crew_scrap();
        return -1;
    }

    /* Only main may outlive the strand the end came through, and only on its
     * way out — so main is waited for too, and then no one may be left. */
    ProcMate *left;
    int n = crew_now(&left);
    int outlived = 0;
    uint32_t skip = c->main_pid;
    for (int i = 0; i < n; i++)
        if (left[i].pid != c->main_pid) outlived++;
    if (n >= 0 && outlived == 0) {
        free(left);
        if (process_gone(c->main_pid, c->main_gen, NULL) != 0) {
            printf("[STRAND] FAIL %s: main cannot be waited for\n", test);
            crew_scrap();
            return -1;
        }
        n = outlived = crew_now(&left);
        skip = 0;
    }
    if (n < 0) {
        printf("[STRAND] FAIL %s: the crew could not be read\n", test);
        crew_scrap();
        return -1;
    }
    if (outlived != 0) {
        printf("[STRAND] FAIL %s: %d strand(s) of the program outlived its end\n",
               test, outlived);
        crew_name(left, n, skip, c->main_pid);
        free(left);
        crew_scrap();
        return -1;
    }
    free(left);
    return crew_deaths(test, died, c, want_actor, want_rest);
}

/* Every strand of the ended program reaches the reaper — the never-joined one
 * as well: nobody is left to join it, so its program's end released it.
 * strand:exited is what the reaper says of each, naming the cabin's spawner,
 * which is this strand. */
static int crew_reaped(const char *test, TouchTag tag, const Crew *c)
{
    uint32_t pids[2 + CREW_WORKERS + 1];
    bool     seen[2 + CREW_WORKERS + 1];
    uint32_t n = 0;
    for (int i = 0; i < c->count && n < sizeof(pids) / sizeof(pids[0]); i++)
        pids[n++] = c->mates[i].pid;
    if (crew_gen_of(c, c->zombie) == 0 && n < sizeof(pids) / sizeof(pids[0]))
        pids[n++] = c->zombie;
    for (uint32_t i = 0; i < n; i++) seen[i] = false;

    const uint32_t me = cabin_info()->pid;
    uint32_t missing = n;
    uint64_t last    = clock_uptime_ms();
    while (missing != 0) {
        Touch t;
        if (touch_await(tag, &t, CREW_QUIET_MS) == 0) {
            last = clock_uptime_ms();
            uint32_t ev[2];
            if (t.payload_len < sizeof(ev)) continue;
            memcpy(ev, t.payload, sizeof(ev));
            if (ev[1] != me) continue;
            for (uint32_t i = 0; i < n; i++)
                if (!seen[i] && pids[i] == ev[0]) { seen[i] = true; missing--; }
        } else if (clock_uptime_ms() - last >= CREW_QUIET_MS) {
            for (uint32_t i = 0; i < n; i++)
                if (!seen[i])
                    printf("[STRAND] FAIL %s: strand %u%s of the ended program was never reclaimed\n",
                           test, pids[i], pids[i] == c->zombie ? " (the never-joined one)" : "");
            return -1;
        }
    }
    return 0;
}

/* Every end below is heard from process:died, claimed before the child starts
 * so that no death of its crew can come first. */
static int crew_listen(const char *test, TouchTag tag)
{
    if (touch_claim(tag, TOUCH_REST, 0, 0) == 0) return 0;
    printf("[STRAND] FAIL %s: could not claim the strands' ends\n", test);
    return -1;
}

/* A kill of the program, through the strand that started it. */
static int crew_kill(const char *test, const Crew *c)
{
    if (proc_finish(c->main_pid, c->main_gen) == 0) return 0;
    printf("[STRAND] FAIL %s: the kill was refused\n", test);
    crew_scrap();
    return -1;
}

/* test5: a kill of the main strand ends the whole program, and the reaper
 * takes every strand of it, the never-joined one too. */
static int test5(void)
{
    TouchTag died   = TOUCH_TAG_ID("process:died");
    TouchTag exited = TOUCH_TAG_ID("strand:exited");
    if (crew_listen("test5", died) != 0) return -1;
    if (crew_listen("test5", exited) != 0) { touch_release(died); return -1; }

    Crew c;
    int rc = crew_start("test5", CREW_LINE("kill"), &c);
    if (rc == 0) rc = crew_kill("test5", &c);
    if (rc == 0) rc = crew_judge("test5", &c, died, PROC_EXIT_KILLED, PROC_EXIT_KILLED);
    if (rc == 0) rc = crew_reaped("test5", exited, &c);
    touch_release(exited);
    touch_release(died);

    if (rc == 0)
        printf("[STRAND] test5 OK: a kill of the main strand ended all %d strands of the "
               "program, every one KILLED and reclaimed, the never-joined one too\n", c.count);
    free(c.mates);
    return rc;
}

/* test6: main returning ends the program, its workers with it. */
static int test6(void)
{
    TouchTag died = TOUCH_TAG_ID("process:died");
    if (crew_listen("test6", died) != 0) return -1;

    Crew c;
    int rc = crew_start("test6", CREW_LINE("exit"), &c);
    if (rc == 0) {
        send(c.main_pid, "go", 2);
        rc = crew_judge("test6", &c, died, CREW_MAIN_CODE, CREW_MAIN_CODE);
    }
    touch_release(died);

    if (rc == 0)
        printf("[STRAND] test6 OK: main's return ended all %d strands, each with its code\n",
               c.count);
    free(c.mates);
    return rc;
}

/* test7: exit() from a worker ends the program, main with it. */
static int test7(void)
{
    TouchTag died = TOUCH_TAG_ID("process:died");
    if (crew_listen("test7", died) != 0) return -1;

    Crew c;
    int rc = crew_start("test7", CREW_LINE("wexit"), &c);
    if (rc == 0) {
        send(c.main_pid, "go", 2);
        rc = crew_judge("test7", &c, died, CREW_WORKER_CODE, CREW_WORKER_CODE);
    }
    touch_release(died);

    if (rc == 0)
        printf("[STRAND] test7 OK: exit() on a worker ended all %d strands, main too\n",
               c.count);
    free(c.mates);
    return rc;
}

/* test8: a strand may not end its sibling, and strand_exit ends only the
 * strand that calls it. */
static int test8(void)
{
    TouchTag died = TOUCH_TAG_ID("process:died");
    if (crew_listen("test8", died) != 0) return -1;

    Crew c;
    int rc = crew_start("test8", CREW_LINE("sibling"), &c);
    if (rc != 0) { touch_release(died); free(c.mates); return rc; }

    send(c.main_pid, "go", 2);
    int32_t refused = 0;
    if (!crew_hear(c.main_pid, &refused, sizeof(refused))) {
        printf("[STRAND] FAIL test8: the child never said what its kill came to\n");
        rc = -1;
    } else if (refused != -ERR_INVALID_ARGUMENT) {
        printf("[STRAND] FAIL test8: a strand's kill of its sibling answered %d, want %d\n",
               (int)refused, -ERR_INVALID_ARGUMENT);
        rc = -1;
    }

    /* The strand that called strand_exit ended, alone and with 0 ... */
    if (rc == 0) {
        Crew one = c;
        ProcMate leaver = { 0 };
        for (int i = 0; i < c.count; i++)
            if (c.mates[i].pid == c.actor) leaver = c.mates[i];
        one.mates = &leaver;
        one.count = 1;
        rc = crew_deaths("test8", died, &one, 0, 0);
    }

    /* ... and everyone else is still aboard. */
    if (rc == 0) {
        crew_forget(&c, c.actor);
        ProcMate *now;
        int n = crew_now(&now);
        for (int i = 0; rc == 0 && i < c.count; i++) {
            if (c.mates[i].pid == c.zombie) continue;
            bool aboard = false;
            for (int j = 0; j < n; j++)
                if (now[j].pid == c.mates[i].pid) aboard = true;
            if (!aboard) {
                printf("[STRAND] FAIL test8: strand %u left with its sibling\n", c.mates[i].pid);
                rc = -1;
            }
        }
        free(now);
    }

    if (rc == 0) {
        c.actor = c.main_pid;
        send(c.main_pid, "go", 2);
        rc = crew_judge("test8", &c, died, CREW_MAIN_CODE, CREW_MAIN_CODE);
    } else {
        crew_scrap();
    }
    touch_release(died);

    if (rc == 0)
        printf("[STRAND] test8 OK: a sibling's kill refused, strand_exit ended one strand, "
               "main's return the rest\n");
    free(c.mates);
    return rc;
}

/* test9: a kill landing while the program spawns strands as fast as it can
 * leaves none behind — a strand born in the middle of the end is refused or
 * ended with the rest. */
static int test9(void)
{
    TouchTag died = TOUCH_TAG_ID("process:died");
    if (crew_listen("test9", died) != 0) return -1;

    int rc = 0;
    for (uint32_t r = 0; rc == 0 && r < CREW_RACE_ROUNDS; r++) {
        Crew c;
        rc = crew_start("test9", CREW_LINE("race"), &c);
        if (rc == 0) rc = crew_kill("test9", &c);
        if (rc == 0) rc = crew_judge("test9", &c, died, PROC_EXIT_KILLED, CREW_ANY_CODE);
        free(c.mates);
    }
    touch_release(died);

    if (rc == 0)
        printf("[STRAND] test9 OK: %u kills of a spawning program left no strand behind\n",
               CREW_RACE_ROUNDS);
    return rc;
}

/* strandtest crash: a strand's crash ends its program. */
static int crash_test(void)
{
    TouchTag died = TOUCH_TAG_ID("process:died");
    if (crew_listen("crash", died) != 0) return -1;

    Crew c;
    int rc = crew_start("crash", CREW_LINE("crash"), &c);
    if (rc == 0) {
        send(c.main_pid, "go", 2);
        rc = crew_judge("crash", &c, died, PROC_EXIT_CRASHED, PROC_EXIT_CRASHED);
    }
    touch_release(died);

    if (rc == 0)
        printf("[STRAND] crash PASS: a worker's crash ended all %d strands, each CRASHED\n",
               c.count);
    free(c.mates);
    return rc;
}

int main(void)
{
    const char *word = luggage_word(1);
    if (word && strcmp(word, "crew") == 0) return crew_child(luggage_word(2));

    printf("[STRAND] strandtest start\n");

    /* Strands require FSGSBASE for per-strand TLS (ring-3 RDFSBASE). Without
     * it strand_spawn refuses, so SKIP cleanly rather than report a failure. */
    if (!cpu_has_fsgsbase()) {
        printf("[STRAND] SKIP: strands require FSGSBASE "
               "(run under STRICT or qemu64,+fsgsbase)\n");
        exit(0);
    }

    if (word && strcmp(word, "crash") == 0) exit(crash_test() == 0 ? 0 : 1);

    if (test1() != 0) exit(1);
    if (test2() != 0) exit(1);
    if (test3() != 0) exit(1);
    if (test4() != 0) exit(1);

    /* Each judged on its own, so one program end that fails does not hide
     * how the others fared. */
    int ended_whole = test5();
    ended_whole |= test6();
    ended_whole |= test7();
    ended_whole |= test8();
    ended_whole |= test9();
    if (ended_whole != 0) exit(1);

    printf("[STRAND] PASS: test1..test9 (per-strand rings + TLS + park/wake + reaper + "
           "timed-wake + a program ends whole)\n");
    exit(0);
    return 0;
}
