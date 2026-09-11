
#include "box/print.h"
#include "box/debug.h"
#include "box/strand.h"
#include "box/sync.h"

#define ROLLCALL_STRANDS 4u
#define ROLLCALL_LINES   400u

typedef struct Talker {
    uint32_t index;
} Talker;

static volatile uint64_t g_finished;

static void Talk(void *arg)
{
    Talker *t = (Talker *)arg;
    for (uint32_t n = 0; n < ROLLCALL_LINES; n++) {
        if (t->index < ROLLCALL_STRANDS / 2)
            printf("[ROLLCALL] s=%u n=%u via=printf\n", t->index, n);
        else
            kdbg_print("[ROLLCALL] s=%u n=%u via=kdbg", t->index, n);
    }
    __atomic_add_fetch(&g_finished, 1, __ATOMIC_ACQ_REL);
    addr_wake(&g_finished, 1);
}

int main(void)
{
    static Talker talkers[ROLLCALL_STRANDS];

    printf("[ROLLCALL] %u strands x %u lines, printf and kdbg\n",
           ROLLCALL_STRANDS, ROLLCALL_LINES);

    uint32_t spawned = 0;
    for (uint32_t i = 0; i < ROLLCALL_STRANDS; i++) {
        talkers[i].index = i;
        if (strand_spawn(Talk, &talkers[i]) != 0) spawned++;
        else printf("[ROLLCALL] FAILED: strand %u did not start\n", i);
    }

    for (;;) {
        uint64_t seen = __atomic_load_n(&g_finished, __ATOMIC_ACQUIRE);
        if (seen >= spawned) break;
        addr_park(&g_finished, seen, 0);
    }

    printf("[ROLLCALL] done: %u strands x %u lines\n",
           spawned, ROLLCALL_LINES);
    return spawned == ROLLCALL_STRANDS ? 0 : 1;
}