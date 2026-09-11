
#include "box/print.h"
#include "box/system.h"
#include "box/clock.h"
#include "box/sync.h"
#include "box/strand.h"
#include "box/cpu.h"
#include "box/core/strand_self.h"
#include "box/error.h"

#define ITERS     100000u
#define NWORKERS  4u
#define MCALLS    500u


static uint64_t g_counter = 0;
static uint64_t g_witness = 0;
static uint64_t g_done    = 0;

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

    uint32_t cycles = 0;
    while (__atomic_load_n(&g_done, __ATOMIC_ACQUIRE) == 0) {
        if (++cycles > 50u) {
            printf("[STRAND] FAIL test1: worker never finished (%u cycles)\n", cycles);
            return -1;
        }
        addr_park(&g_done, 0, 200);
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


static volatile uint64_t g_remaining;
static volatile uint32_t g_worker_fail;
static uint32_t          g_worker_self[NWORKERS];

static void ipc_worker(void *arg)
{
    uint32_t idx  = (uint32_t)(uintptr_t)arg;
    uint32_t self = strand_self();
    g_worker_self[idx] = self;

    if (self == 0) {
        __atomic_store_n(&g_worker_fail, 1u, __ATOMIC_RELAXED);
    } else {
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

    uint32_t cycles = 0;
    uint64_t cur;
    while ((cur = __atomic_load_n(&g_remaining, __ATOMIC_ACQUIRE)) != 0) {
        if (++cycles > 80u) {
            printf("[STRAND] FAIL test2: %u worker(s) stuck after %u cycles\n",
                   (unsigned)cur, cycles);
            return -1;
        }
        addr_park(&g_remaining, cur, 200);
    }

    if (__atomic_load_n(&g_worker_fail, __ATOMIC_ACQUIRE) != 0) {
        printf("[STRAND] FAIL test2: a worker reported a TLS/routing mismatch\n");
        return -1;
    }

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


#define CHURN 100u

#define CHURN_STILL_MS  16000u

#define REAPER_LOOK_MS  200u

static volatile uint64_t g_churn_remaining;
static volatile uint64_t g_reaper_sleep;

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

    for (int k = 0; k < 16; k++) yield();
    if (sysinfo(&si) != 0) return 0;
    uint32_t base = si.process_count;

    for (uint32_t r = 0; r < CHURN; r++) {
        g_churn_remaining = 1;
        uint32_t pid = strand_spawn(noop_worker, 0);
        if (pid == 0) {
            printf("[STRAND] FAIL test3: strand_spawn failed at round %u (table exhausted — reaper not reclaiming)\n", r);
            return -1;
        }
        uint64_t started = clock_uptime_ms();
        uint64_t cur;
        while ((cur = __atomic_load_n(&g_churn_remaining, __ATOMIC_ACQUIRE)) != 0) {
            addr_park(&g_churn_remaining, cur, 200);
            if (__atomic_load_n(&g_churn_remaining, __ATOMIC_ACQUIRE) != cur)
                started = clock_uptime_ms();
            else if (clock_uptime_ms() - started >= CHURN_STILL_MS) {
                printf("[STRAND] FAIL test3: join stuck at round %u (%u ms with no progress)\n",
                       r, (unsigned)CHURN_STILL_MS);
                return -1;
            }
        }
    }

    uint32_t cnt = base + CHURN;
    if (sysinfo(&si) == 0) cnt = si.process_count;
    uint32_t best    = cnt;
    uint64_t started = clock_uptime_ms();
    while (cnt > base + 4u) {
        if (clock_uptime_ms() - started >= CHURN_STILL_MS) {
            printf("[STRAND] FAIL test3: process_count stuck at %u (base %u, churned %u) — reaper not reclaiming\n",
                   cnt, base, CHURN);
            return -1;
        }
        addr_park(&g_reaper_sleep, 0, REAPER_LOOK_MS);
        if (sysinfo(&si) == 0) cnt = si.process_count;
        if (cnt < best) { best = cnt; started = clock_uptime_ms(); }
    }

    printf("[STRAND] test3 OK: %u spawn+join churn reclaimed (process_count base %u -> %u)\n",
           CHURN, base, cnt);
    return 0;
}


static volatile uint64_t g_t4_flag;
static volatile uint64_t g_t4_done;

static void wake_worker(void *arg)
{
    (void)arg;
    uint32_t guard = 0;
    while (__atomic_load_n(&g_t4_done, __ATOMIC_ACQUIRE) == 0) {
        addr_wake(&g_t4_flag, 0);
        if (++guard > 200000u) break;
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