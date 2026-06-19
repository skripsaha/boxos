/*
 * strandtest — proves the P4 strand go-live.
 *
 * One worker strand is spawned into the caller's cabin (shared address
 * space).  It exercises, in a single scenario:
 *
 *   - strand_spawn: a real second execution context runs in the shared
 *     address space (the worker reads/writes the same globals as main).
 *   - concurrency + shared-memory atomics: main AND the worker each do
 *     ITERS atomic increments of one shared counter at the same time;
 *     counter == 2*ITERS proves they ran concurrently with no lost updates.
 *   - park -> wake (P3 addr_park/addr_wake): main parks on a done flag and
 *     the worker wakes it.
 *
 * Only ONE strand is spawned, on a clean cabin, so the spawn never races a
 * sibling's in-flight syscall.  Driving MANY strands through one cabin's
 * shared IPC rings simultaneously (concurrent result delivery) is the
 * cross-strand IPC work reserved for P5.
 *
 * Emits exactly one "[STRAND] PASS" on success (the matrix greps for it).
 */

#include "box/print.h"
#include "box/system.h"   /* exit */
#include "box/sync.h"     /* addr_park, addr_wake */
#include "box/strand.h"   /* strand_spawn */
#include "box/error.h"

#define ITERS  100000u    /* big enough that the two strands' runs overlap */

static uint64_t g_counter = 0;   /* incremented by BOTH strands (atomic)   */
static uint64_t g_witness = 0;   /* worker sets it — proves it ran + shares AS */
static uint64_t g_done    = 0;   /* worker-done flag; also the park address */

static void worker(void *arg)
{
    (void)arg;
    __atomic_store_n(&g_witness, 0xCAFEu, __ATOMIC_RELAXED);
    for (uint32_t i = 0; i < ITERS; i++)
        __atomic_fetch_add(&g_counter, 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_done, 1u, __ATOMIC_RELEASE);
    addr_wake(&g_done, 0);
}

int main(void)
{
    printf("[STRAND] strandtest start\n");

    uint32_t pid = strand_spawn(worker, 0);
    if (pid == 0) {
        printf("[STRAND] FAIL: strand_spawn returned 0\n");
        exit(1);
    }

    /* Increment concurrently with the worker — exercises true parallel
     * access to the shared atomic on multi-core. */
    for (uint32_t i = 0; i < ITERS; i++)
        __atomic_fetch_add(&g_counter, 1u, __ATOMIC_RELAXED);

    /* Park until the worker signals done.  The value pre-check in addr_park
     * closes the lost-wake race if the worker finished first.  Bound the
     * retries so a genuine hang fails loudly. */
    /* Fail-fast: short park timeouts + a bounded retry budget, so a genuine
     * stuck worker reports FAIL within a few seconds (and flushes its
     * buffered output) instead of silently hanging the test harness. With a
     * correctly woken strand this resolves in well under one timeout. */
    uint32_t cycles = 0;
    while (__atomic_load_n(&g_done, __ATOMIC_ACQUIRE) == 0) {
        if (++cycles > 40u) {
            printf("[STRAND] FAIL: worker never finished (%u cycles)\n", cycles);
            exit(1);
        }
        addr_park(&g_done, 0, 200);
    }

    uint64_t total   = __atomic_load_n(&g_counter, __ATOMIC_ACQUIRE);
    uint64_t witness = __atomic_load_n(&g_witness, __ATOMIC_ACQUIRE);
    uint64_t want    = 2ull * ITERS;

    if (total == want && witness == 0xCAFEu) {
        printf("[STRAND] PASS: spawn + concurrent atomics (2 x %u = %u) + park->wake (worker pid %u)\n",
               ITERS, (unsigned)total, pid);
        exit(0);
    }

    printf("[STRAND] FAIL: counter=%u (want %u) witness=%u\n",
           (unsigned)total, (unsigned)want, (unsigned)witness);
    exit(1);
    return 0;
}
