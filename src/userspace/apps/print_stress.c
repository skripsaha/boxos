
#include "box/print.h"
#include "box/strand.h"
#include "box/cpu.h"
#include "box/sync.h"
#include "box/system.h"
#include "box/clock.h"

#define STRAND_COUNT 16u
#define ITERS        2000u

static volatile uint32_t g_barrier_count = 0;
static volatile uint32_t g_done_count    = 0;

static void barrier_wait(void)
{
    __atomic_fetch_add(&g_barrier_count, 1u, __ATOMIC_ACQ_REL);
    while (__atomic_load_n(&g_barrier_count, __ATOMIC_ACQUIRE) < STRAND_COUNT) {
        __asm__ volatile("pause");
    }
}

static void zero_pad_u32(uint32_t value, int width, char *out)
{
    for (int i = width - 1; i >= 0; i--) {
        out[i] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    out[width] = '\0';
}

static void run_pattern(uint32_t id)
{
    char id_str[3];
    zero_pad_u32(id, 2, id_str);

    for (uint32_t i = 0; i < ITERS; i++) {
        char iter_str[9];
        zero_pad_u32(i, 8, iter_str);
        printf("[PS-%s] %s\n", id_str, iter_str);
    }

    __atomic_fetch_add(&g_done_count, 1u, __ATOMIC_RELEASE);
    addr_wake(&g_done_count, 0);
}

static void worker(void *arg)
{
    uint32_t id = (uint32_t)(uintptr_t)arg;
    barrier_wait();
    run_pattern(id);
}

int main(void)
{
    if (!cpu_has_fsgsbase()) {
        printf("[PS] SKIP: strands require FSGSBASE (run under STRICT)\n");
        exit(0);
    }

    printf("[PS] print_stress starting: %u strands x %u iters each\n",
           STRAND_COUNT, ITERS);
    io_flush();

    for (uint32_t id = 1; id < STRAND_COUNT; id++) {
        uint32_t pid = strand_spawn(worker, (void *)(uintptr_t)id);
        if (pid == 0) {
            printf("[PS] FAIL: strand_spawn %u returned 0\n", id);
            exit(1);
        }
    }

    barrier_wait();
    run_pattern(0);

    uint32_t cur;
    uint32_t last_done      = 0;
    uint64_t last_change_ms = clock_uptime_ms();
    while ((cur = __atomic_load_n(&g_done_count, __ATOMIC_ACQUIRE)) < STRAND_COUNT) {
        uint64_t now = clock_uptime_ms();
        if (cur != last_done) {
            last_done      = cur;
            last_change_ms = now;
        }
        if (now - last_change_ms > 10u * 60u * 1000u) {
            printf("[PS] FAIL: only %u/%u strands finished; no progress in 10 min\n",
                   cur, STRAND_COUNT);
            exit(1);
        }
        addr_park(&g_done_count, cur, 200);
    }

    printf("[PS SUMMARY] strands=%u iters=%u expected=%u PASS\n",
           STRAND_COUNT, ITERS, STRAND_COUNT * ITERS);
    exit(0);
    return 0;
}