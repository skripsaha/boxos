/*
 * strand.c — userspace strand spawn/exit wrappers.
 *
 * strand_spawn issues SYSTEM_OP_STRAND_SPAWN, which creates a second+
 * execution context sharing the caller's cabin (address space).  The
 * kernel starts the strand at a given entry VA with one argument in rdi;
 * we use a small trampoline so we can carry BOTH the user function and its
 * argument through that single slot and run strand_exit when fn returns.
 */

#include "box/strand.h"
#include "box/core/manifest.h"   /* MfCall1 */
#include "box/memory.h"          /* malloc / free */
#include "box/touch.h"           /* touch_stash_free_self */
#include "box/system.h"          /* yield */
#include "box/timeouts.h"        /* BOX_TIMEOUT_IPC_MS */
#include "box/error.h"
#include "boxos_decks.h"         /* DECK_SYSTEM, SYSTEM_OP_STRAND_SPAWN, SYSTEM_OP_PROC_KILL */

/* Start record: the kernel passes the strand a single argument (rdi); we
 * use it to carry both the user function and its argument through the
 * trampoline.  Freed by the trampoline once unpacked. */
struct strand_start {
    void (*fn)(void *arg);
    void  *arg;
};

/* First instruction every spawned strand runs.  Entered by the kernel with
 * the start-record pointer in rdi; unpacks it, runs the user function, then
 * terminates the strand.  MUST NOT return — a fresh strand stack has no
 * caller frame to return into. */
static void strand_trampoline(void *p)
{
    struct strand_start *s = (struct strand_start *)p;
    void (*fn)(void *arg) = s->fn;
    void *arg             = s->arg;
    free(s);
    fn(arg);
    strand_exit();
}

static uint32_t strand_spawn_impl(void (*fn)(void *arg), void *arg, uint8_t joinable)
{
    if (!fn) return 0;

    struct strand_start *s = (struct strand_start *)malloc(sizeof(*s));
    if (!s) return 0;
    s->fn  = fn;
    s->arg = arg;

    uint64_t params[3];
    params[0] = (uint64_t)(uintptr_t)&strand_trampoline;  /* entry_va             */
    params[1] = (uint64_t)(uintptr_t)s;                   /* arg -> rdi           */
    params[2] = (uint64_t)joinable;                       /* 1 = zombie-until-join */

    uint32_t pid = 0;
    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_STRAND_SPAWN,
                     params, (uint16_t)sizeof(params),
                     NULL, 0,
                     &pid, (uint32_t)sizeof(pid), NULL,
                     BOX_TIMEOUT_IPC_MS, NULL);
    if (rc != 0) {
        /* Spawn failed: the kernel never ran the trampoline, so the start
         * record is still ours to reclaim. */
        free(s);
        return 0;
    }
    return pid;
}

uint32_t strand_spawn(void (*fn)(void *arg), void *arg)
{
    return strand_spawn_impl(fn, arg, 0);   /* eager-reap worker (raw) */
}

uint32_t strand_spawn_joinable(void (*fn)(void *arg), void *arg)
{
    return strand_spawn_impl(fn, arg, 1);   /* zombie-until-join (std::thread) */
}

void strand_release(uint32_t pid)
{
    if (pid == 0) return;
    uint32_t p = pid;
    (void)MfCall1(DECK_SYSTEM, SYSTEM_OP_STRAND_RELEASE,
                  &p, (uint16_t)sizeof(p),
                  NULL, 0, NULL, 0, NULL,
                  BOX_TIMEOUT_IPC_MS, NULL);
}

void strand_exit(void)
{
    /* Ф20e — return this strand's StrandPool cache to the global heap and free
     * its slab slot BEFORE we ask the kernel to terminate the strand. The slot's
     * generation is bumped here, so even if the kernel were to race the orphan
     * death-stamp it would miss; an orderly exit never leaves an ORPHANED slot. */
    strand_pool_flush_self();

    /* Ф21 — release this strand's per-strand Touch tag-filter stash back to the
     * cabin heap (order: flush pool → free stash → kill). Idempotent; a strand
     * that never consumed a tag is a no-op. */
    touch_stash_free_self();

    /* Terminate just this strand: SYS_PROC_KILL(target == 0) means self.
     * No __box_runtime_fini / io_flush / spawner-notify — those are exit()'s
     * job for the whole cabin and would wrongly run global teardown on a
     * per-strand exit.  Retry briefly, then park forever; must never fall
     * through to a return on the trampoline's caller-less stack. */
    uint32_t target = 0;
    for (int attempt = 0; attempt < 3; attempt++) {
        int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_PROC_KILL,
                         &target, (uint16_t)sizeof(target),
                         NULL, 0, NULL, 0, NULL,
                         BOX_TIMEOUT_IPC_MS, NULL);
        if (rc == 0) break;
        yield();
    }
    for (;;) yield();
}
