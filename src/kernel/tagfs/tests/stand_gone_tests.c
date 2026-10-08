#include "tests.h"
#include "stand_tests.h"
#include "amp.h"
#include "kcore.h"
#include "process.h"
#include "pid_allocator.h"
#include "touch.h"
#include "cpu_calibrate.h"
#include "vmm.h"
#include "op_registry.h"
#include "boxos_decks.h"
#include "boxos_manifest.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// A death is gone however late it is asked about
//
// process.gone asks whether one incarnation, (pid, generation), is gone. A
// program that exits is taken down after its exit, by the reaper of a K-Core:
// its record leaves the process table, and its seat in the pid allocator is
// given up only when the last holder of the record lets it go — once its cabin
// is down, and for as long as anything else still holds it. An asker who came
// in between used to find no record while the allocator still called the seat
// this incarnation's, and was answered BUSY: a program waiting for its child's
// end was told, once the child had ended, that it could not be asked about.
//
// The children here are records in the table, each a new incarnation of a
// seat, whose death is committed the way an exit commits it; the reaper that
// takes them down is a K-Core's own, on its own core, while this core asks.
// The test holds each record past its destroy, as a job still holding it
// would, so every child is asked about from inside that window as well as
// across both of its edges.
// ============================================================================

#define STAND_GONE_CHILDREN 32u     /* incarnations asked about, each taken down by the reaper */
#define STAND_GONE_HELD     16u     /* asks a child while it is out of the table, its seat held */
#define STAND_GONE_BOUND_MS 5000u   /* a reaper that has not come by then is the failure */

/* Where an incarnation stands, as the table and the allocator have it. */
typedef enum {
    STAND_GONE_LISTED = 0,          /* its record is in the process table */
    STAND_GONE_SEATED = 1,          /* out of the table, its seat still its own */
    STAND_GONE_FREED  = 2,          /* its seat given up */
} StandGoneStage;

static const char* const s_gone_stage_name[] = {
    "listed", "out of the table with its seat held", "with its seat given up",
};

/* The asks of the whole test, and the first answer that was not "gone". */
typedef struct {
    uint32_t       pid;
    uint32_t       generation;
    uint64_t       asked[3];        /* by the stage seen just before each ask */
    uint64_t       wrong;
    int            first_wrong;
    StandGoneStage first_before;
    StandGoneStage first_after;
    uint32_t       first_child;
} StandGoneTally;

static StandGoneTally s_gone;

/* The waiter as the op sees it: a record nobody runs. Every child's death is
 * committed before it is first asked about, so the op never parks it. */
static process_t s_gone_asker;

static uint8_t s_gone_op[sizeof(ManifestOp) + 2 * sizeof(uint32_t)] __attribute__((aligned(8)));

/* A K-Core other than this one, which runs the reaper in its own loop; or
 * total_cores when there is none. */
static uint8_t StandGoneReaperCore(void)
{
    uint8_t here = amp_get_core_index();
    for (uint8_t c = 0; c < g_amp.total_cores; c++)
        if (c != here && g_amp.cores[c].is_kcore && amp_core_online(&g_amp.cores[c]))
            return c;
    return g_amp.total_cores;
}

static StandGoneStage StandGoneStageNow(void)
{
    process_t* listed = process_find_ref(s_gone.pid);
    bool       ours   = listed && listed->generation == s_gone.generation;
    if (listed)
        process_ref_dec(listed);
    if (ours)
        return STAND_GONE_LISTED;
    return pid_validate(s_gone.pid) && pid_generation(s_gone.pid) == s_gone.generation
               ? STAND_GONE_SEATED
               : STAND_GONE_FREED;
}

/* process.gone of the child, asked as a program's pocket asks it; the stage
 * the child stood at after the answer. */
static StandGoneStage StandGoneAsk(const OpRegistration* reg, uint32_t child)
{
    ManifestOp* op = (ManifestOp*)s_gone_op;
    OpContext   ctx;

    memset(s_gone_op, 0, sizeof(s_gone_op));
    op->op_kind    = OP_KIND(DECK_SYSTEM, SYSTEM_OP_PROCESS_GONE);
    op->param_size = 2 * sizeof(uint32_t);
    op->in_crate   = CRATE_INDEX_NONE;
    op->out_crate  = CRATE_INDEX_NONE;
    memcpy(op->params, &s_gone.pid, sizeof(uint32_t));
    memcpy(op->params + sizeof(uint32_t), &s_gone.generation, sizeof(uint32_t));
    memset(&ctx, 0, sizeof(ctx));
    ctx.proc = &s_gone_asker;

    /* The startup tests run with interrupts off, and the reaper's teardown
     * shoots TLB entries down on every core: this core answers them by hand
     * between its asks, as a core spinning for a lock does. */
    vmm_tlb_shootdown_poll();
    StandGoneStage before = StandGoneStageNow();
    int            rc     = reg->handler(op, NULL, 0, &ctx);
    StandGoneStage after  = StandGoneStageNow();

    s_gone.asked[before]++;
    if (rc != OK && s_gone.wrong++ == 0) {
        s_gone.first_wrong  = rc;
        s_gone.first_before = before;
        s_gone.first_after  = after;
        s_gone.first_child  = child;
    }
    return after;
}

/* Asked, with the reaper's core woken between the asks, until the child has
 * left `stage`; false when the reaper did not take it on within the bound. */
static bool StandGoneAskPast(const OpRegistration* reg, uint8_t reaper, uint32_t child,
                             StandGoneStage stage)
{
    uint64_t bound = rdtsc() + cpu_ms_to_tsc(STAND_GONE_BOUND_MS);
    while (StandGoneAsk(reg, child) == stage) {
        if (rdtsc() >= bound)
            return false;
        kcore_wake(reaper);
    }
    return true;
}

/*
 * Thirty-two children, each asked about while its record is in the table,
 * while the reaper takes it out, sixteen times while the record is out of the
 * table and its seat still held, while its last holder's letting go is carried
 * to the end of its seat, and once after: every answer is "gone". Red before,
 * measured on UEFI 4c: of 4267 asks made with the record out of the table and
 * its seat held, 4255 were answered ERR_BUSY — the other twelve met the seat
 * given up on their way.
 */
TestResult TestStandGoneThroughTheTeardown(void)
{
    TestResult            verdict = TEST_PASS;
    const OpRegistration* reg     = OpRegistryLookup(OP_KIND(DECK_SYSTEM, SYSTEM_OP_PROCESS_GONE));
    uint8_t               reaper  = StandGoneReaperCore();
    process_t*            held    = NULL;

    if (reaper == g_amp.total_cores)
        return test_steps_aside("no K-Core but this one runs the reaper");
    TEST_ASSERT(reg != NULL, "process.gone is registered");
    memset(&s_gone, 0, sizeof(s_gone));
    memset(&s_gone_asker, 0, sizeof(s_gone_asker));

    for (uint32_t c = 0; c < STAND_GONE_CHILDREN; c++) {
        process_t* child = process_create(NULL);
        TEST_ASSERT(child != NULL, "a child's record is made in the table");
        s_gone.pid        = child->pid;
        s_gone.generation = child->generation;

        /* The holder's ref before the reaper can see a corpse, and the death
         * committed as SysProcKill commits an exit: the claim and its answers
         * first, the state the reaper looks for after. */
        process_ref_inc(child);
        held = child;
        TouchCleanupProcess(child, (int32_t)c);
        process_set_state(child, PROC_DONE);

        TEST_ASSERT(StandGoneAskPast(reg, reaper, c, STAND_GONE_LISTED),
                    "the reaper takes the child out of the table");
        for (uint32_t h = 0; h < STAND_GONE_HELD; h++)
            StandGoneAsk(reg, c);

        held = NULL;
        process_ref_dec(child);
        TEST_ASSERT(StandGoneAskPast(reg, reaper, c, STAND_GONE_SEATED),
                    "the last holder letting go gives the child's seat up");
        StandGoneAsk(reg, c);
    }

    kprintf("[STAND] gone: %u children asked about %llu time(s) — %llu listed, %llu out of the "
            "table with the seat held, %llu with the seat given up; %llu answered other than "
            "gone\n", STAND_GONE_CHILDREN,
            (unsigned long long)(s_gone.asked[0] + s_gone.asked[1] + s_gone.asked[2]),
            (unsigned long long)s_gone.asked[STAND_GONE_LISTED],
            (unsigned long long)s_gone.asked[STAND_GONE_SEATED],
            (unsigned long long)s_gone.asked[STAND_GONE_FREED], (unsigned long long)s_gone.wrong);
    if (s_gone.wrong)
        kprintf("[STAND] gone: the first, child %u, answered %d (%s) asked %s and answered %s\n",
                s_gone.first_child, s_gone.first_wrong, ErrorString(s_gone.first_wrong),
                s_gone_stage_name[s_gone.first_before], s_gone_stage_name[s_gone.first_after]);
    TEST_ASSERT(s_gone.asked[STAND_GONE_SEATED] >= (uint64_t)STAND_GONE_CHILDREN * STAND_GONE_HELD,
                "every child was asked about inside the window its holder kept open");
    TEST_ASSERT_EQ(s_gone.wrong, 0, "every ask is answered gone");
out:
    if (held)
        process_ref_dec(held);
    return verdict;
}

#endif /* CONFIG_VOLUME_TESTS */
