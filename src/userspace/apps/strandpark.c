/* strandpark — root-cause probe for the currentexec A+B flake, and the
 * oracle of one wake for a brigade.
 *
 * Part 1 (T1-T3) proved addr_park (global + stack) and cpu_tsc math WORK from a
 * spawned strand in ISOLATION. Part 2 (T4) tests the real hypothesis: do
 * CONCURRENT blocking syscalls from MULTIPLE strands at once race the shared
 * per-process IPC/Result ring (the known P5 gap)? Several strands + main each
 * hammer a tight addr_park loop simultaneously; a lost/mis-delivered completion
 * shows up as a park that never returns → a strand stalls → watchdog FAIL.
 *
 * Part 3 (T5, the brigade): BRIGADE strands park on ONE word with no deadline,
 * main learns of every park from the kernel's own word for it (the
 * strand:parked Touch), then changes the word and wakes all parked on it —
 * once. Every sleeper must come back. SysAddrWake used to claim its waiters
 * into a stack tray of 256 and stop when the tray was full: a notify_all with
 * more sleepers than that woke 256 and left the rest asleep for ever, and
 * nothing said so. BRIGADE is more than that tray was.
 *
 * kdbg = immediate serial. Emits [SPK] PASS / [SPK] FAIL.
 */
#include "box/print.h"
#include "box/debug.h"
#include "box/system.h"
#include "box/sync.h"
#include "box/strand.h"
#include "box/touch.h"
#include "box/string.h"
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

/* ---- Part 3: one wake for a brigade ------------------------------------- */

#define BRIGADE 300

static volatile uint64_t g_word;   /* parked on; 0 until the one wake */
static volatile uint64_t g_woke;   /* sleepers back from that wake */

static void sleeper(void *arg)
{
    (void)arg;
    /* No deadline: only the wake ends this park. A value already changed is
     * the same wake, seen a moment early. */
    error_t rc = addr_park((void *)&g_word, 0, 0);
    if (rc == OK || rc == ERR_ADDR_VALUE_MISMATCH)
        __atomic_add_fetch(&g_woke, 1u, __ATOMIC_RELEASE);
    else
        printf("[SPK] FAIL: brigade sleeper: park said %d\n", (int)rc);
}

static int brigade(void)
{
    static uint32_t pids[BRIGADE];

    /* The kernel says "strand:parked" for every park it lets through; that
     * word, not a guess about timing, is how main knows the whole brigade is
     * on the chain before the one wake. Claimed BEFORE the first spawn. */
    TouchTag parked = touch_pair_choose(touch_intern("strand:parked"));
    if (parked == TOUCH_TAG_INVALID || touch_claim(parked, TOUCH_REST, 0, 0) != OK) {
        printf("[SPK] FAIL: brigade: cannot claim strand:parked\n");
        return 1;
    }

    g_word = 0; g_woke = 0;
    for (int i = 0; i < BRIGADE; i++) {
        pids[i] = strand_spawn(sleeper, NULL);
        if (!pids[i]) { printf("[SPK] FAIL: brigade: spawn %d returned 0\n", i); return 1; }
    }

    uint32_t on_chain = 0;
    while (on_chain < BRIGADE) {
        Touch t;
        if (!touch_wait_tag(parked, &t, 30000)) {
            printf("[SPK] FAIL: brigade: %u of %d parked, then silence\n", on_chain, BRIGADE);
            return 1;
        }
        if (t.payload_len < 12) continue;          /* [u64 phys][u32 pid] */
        uint32_t pid;
        memcpy(&pid, t.payload + 8, sizeof(pid));
        for (int i = 0; i < BRIGADE; i++) {
            if (pids[i] == pid) { on_chain++; break; }
        }
    }
    touch_release(parked);

    __atomic_store_n(&g_word, 1u, __ATOMIC_RELEASE);
    error_t rc = addr_wake((void *)&g_word, 0);    /* everyone parked on it, once */
    if (rc != OK) { printf("[SPK] FAIL: brigade: wake said %d\n", (int)rc); return 1; }

    /* The test's own watch: a look every 5 ms, 600 looks in all. A sleeper
     * the wake never reached stays parked past any of them. */
    uint32_t cyc = 0;
    for (;;) {
        uint64_t woke = __atomic_load_n(&g_woke, __ATOMIC_ACQUIRE);
        if (woke >= BRIGADE) break;
        if (++cyc > 600u) {
            printf("[SPK] FAIL: brigade: one wake reached %lu of %d sleepers\n",
                   (unsigned long)woke, BRIGADE);
            return 1;
        }
        addr_park((void *)&g_woke, woke, 5);
    }
    printf("[SPK] PASS: brigade of %d parked, one wake, %d back\n", BRIGADE, BRIGADE);
    return 0;
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
    exit(brigade());
}
