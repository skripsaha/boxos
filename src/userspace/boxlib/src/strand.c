
#include "box/strand.h"
#include "box/core/manifest.h"
#include "box/print.h"
#include "box/memory.h"
#include "box/debug.h"
#include "box/touch.h"
#include "box/system.h"
#include "box/timeouts.h"
#include "box/error.h"
#include "boxos_decks.h"

struct strand_start {
    void (*fn)(void *arg);
    void  *arg;
};

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
    params[0] = (uint64_t)(uintptr_t)&strand_trampoline;
    params[1] = (uint64_t)(uintptr_t)s;
    params[2] = (uint64_t)joinable;

    uint32_t pid = 0;
    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_STRAND_SPAWN,
                     params, (uint16_t)sizeof(params),
                     NULL, 0,
                     &pid, (uint32_t)sizeof(pid), NULL,
                     0 , NULL);
    if (rc != 0) {
        kdbg_print("[strand] spawn refused rc=%d pid_out=%u", rc, pid);
        free(s);
        return 0;
    }
    return pid;
}

uint32_t strand_spawn(void (*fn)(void *arg), void *arg)
{
    return strand_spawn_impl(fn, arg, 0);
}

uint32_t strand_spawn_joinable(void (*fn)(void *arg), void *arg)
{
    return strand_spawn_impl(fn, arg, 1);
}

void strand_release(uint32_t pid)
{
    if (pid == 0) return;
    uint32_t p = pid;
    (void)MfCall1(DECK_SYSTEM, SYSTEM_OP_STRAND_RELEASE,
                  &p, (uint16_t)sizeof(p),
                  NULL, 0, NULL, 0, NULL,
                  BOX_ANSWER_GUARANTEED, NULL);
}

void strand_exit(void)
{
    io_flush();

    strand_pool_flush_self();

    touch_stash_free_self();
    result_stash_free_self();

    uint32_t target = 0;
    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_PROC_KILL,
                     &target, (uint16_t)sizeof(target),
                     NULL, 0, NULL, 0, NULL,
                     BOX_ANSWER_GUARANTEED, NULL);
    if (rc != 0)
        kdbg_print("[strand] self-kill refused rc=%d — kernel defect; strand parked for ever", rc);
    for (;;) yield();
}