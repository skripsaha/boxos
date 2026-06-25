/* strandpark — root-cause probe for the currentexec A+B flake.
 *
 * Part 1 (T1-T3) proved addr_park (global + stack) and cpu_tsc math WORK from a
 * spawned strand in ISOLATION. Part 2 (T4) tests the real hypothesis: do
 * CONCURRENT blocking syscalls from MULTIPLE strands at once race the shared
 * per-process IPC/Result ring (the known P5 gap)? Several strands + main each
 * hammer a tight addr_park loop simultaneously; a lost/mis-delivered completion
 * shows up as a park that never returns → a strand stalls → watchdog FAIL.
 *
 * kdbg = immediate serial. Emits [SPK] PASS / [SPK] FAIL.
 */
#include "box/print.h"
#include "box/debug.h"
#include "box/system.h"
#include "box/sync.h"
#include "box/strand.h"
#include "box/cpu.h"
#include "box/error.h"

#define NWORKERS 3
#define ITERS    150

static volatile uint64_t g_never;                 /* park target, stays 0 */
static volatile uint64_t g_go;                    /* start barrier */
static volatile uint64_t g_progress[NWORKERS + 1];/* [0]=main, [1..N]=workers: parks completed */

static void park_hammer(int slot)
{
    while (__atomic_load_n(&g_go, __ATOMIC_ACQUIRE) == 0) yield();
    for (int i = 0; i < ITERS; i++) {
        /* 2ms timed park on a value that never changes → pure timeout. Run
         * CONCURRENTLY across all workers + main: overlapping async result_wait
         * completions on the shared ring are exactly the P5 race surface. */
        addr_park((void *)&g_never, 0, 2);
        __atomic_store_n(&g_progress[slot], (uint64_t)(i + 1), __ATOMIC_RELEASE);
    }
}

static void worker(void *arg)
{
    int slot = (int)(long)arg;
    park_hammer(slot);
    for (;;) yield();
}

int main(void)
{
    printf("[SPK] strandpark concurrent-park stress (%d workers + main x %d parks)\n",
           NWORKERS, ITERS);
    if (!cpu_has_fsgsbase()) { printf("[SPK] SKIP: strands require FSGSBASE (run under STRICT)\n"); exit(0); }

    g_never = 0; g_go = 0;
    for (int s = 0; s <= NWORKERS; s++) g_progress[s] = 0;

    for (int s = 1; s <= NWORKERS; s++) {
        uint32_t pid = strand_spawn(worker, (void *)(long)s);
        if (!pid) { printf("[SPK] FAIL: strand_spawn %d returned 0\n", s); exit(1); }
    }

    __atomic_store_n(&g_go, 1u, __ATOMIC_RELEASE);   /* release the barrier */
    park_hammer(0);                                  /* main hammers too */

    /* Watchdog: every worker must reach ITERS. A stalled park (lost completion)
     * leaves its slot below ITERS → we time out and FAIL with the slot map. */
    for (int w = 1; w <= NWORKERS; w++) {
        uint32_t cyc = 0;
        while (__atomic_load_n(&g_progress[w], __ATOMIC_ACQUIRE) < ITERS) {
            if (++cyc > 600u) {
                printf("[SPK] FAIL: worker %d stalled at %lu/%d parks "
                       "(concurrent multi-strand ring race)\n",
                       w, (unsigned long)__atomic_load_n(&g_progress[w], __ATOMIC_ACQUIRE), ITERS);
                exit(1);
            }
            addr_park((void *)&g_never, 0, 5);
        }
    }
    printf("[SPK] PASS: all %d workers + main completed %d concurrent parks (no ring race)\n",
           NWORKERS, ITERS);
    exit(0);
}
