/*
 * journey.c — the kernel's part of the journey, and SYSTEM_OP_JOURNEY.
 * What is answered: boxos_journey.h. Who marks what: journey.h.
 *
 * Lock ordering: g_journey_lock is a leaf. JourneyMark takes it from wherever
 * a step is reached — a Touch watch on the publisher's stack, ScreenClaim
 * under s_screen_turn — and nothing is taken while it is held.
 */

#include "journey.h"
#include "boarding.h"
#include "system_deck.h"
#include "boxos_decks.h"
#include "crate_io.h"
#include "op_registry.h"
#include "manifest_auth.h"
#include "touch.h"
#include "logbook.h"
#include "atomics.h"         /* rdtsc */
#include "cpuid.h"           /* g_cpu_caps.has_hypervisor */
#include "cpu_calibrate.h"   /* cpu_get_tsc_freq_khz */
#include "klib.h"

_Static_assert(sizeof(JourneyMoment) == sizeof(BoardingMoment) &&
               JOURNEY_WORD == BOARDING_MOMENT_WORD,
               "a loader's moment is answered as it was stamped, byte for byte");

_Static_assert(BOARDING_PASS_BYTES / sizeof(BoardingMoment) <= JOURNEY_LOADER_MAX,
               "every moment a pass can hold fits an answer");

static JourneyMoment g_marks[JOURNEY_KERNEL_MAX];
static uint16_t      g_marked = 0;
static spinlock_t    g_journey_lock;   /* BSS-zero is unlocked */

static bool MarkIs(const JourneyMoment *m, const char *what)
{
    for (unsigned c = 0; c < JOURNEY_WORD; c++) {
        if (m->what[c] != what[c]) return false;
        if (!what[c]) return true;
    }
    return true;
}

void JourneyMark(const char *what)
{
    uint64_t now = rdtsc();
    spin_lock(&g_journey_lock);
    bool seen = false;
    for (uint16_t i = 0; i < g_marked && !seen; i++) seen = MarkIs(&g_marks[i], what);
    if (!seen && g_marked < JOURNEY_KERNEL_MAX) {
        JourneyMoment *m = &g_marks[g_marked++];
        memset(m->what, 0, JOURNEY_WORD);
        for (unsigned c = 0; c < JOURNEY_WORD && what[c]; c++) m->what[c] = what[c];
        m->tsc = now;
    }
    spin_unlock(&g_journey_lock);
}

static void JourneyVolumeMounted(TouchTag tag, const void *payload, uint32_t plen,
                                 uint32_t source_pid, void *ctx)
{
    (void)tag; (void)payload; (void)plen; (void)source_pid; (void)ctx;
    JourneyMark("volume");
}

void JourneyWatch(void)
{
    TouchTag full = TOUCH_TAG_INVALID, bare = TOUCH_TAG_INVALID;
    TouchLogbookResolve("volume:mounted", &full, &bare);
    if (!TouchWatchSet(full, JourneyVolumeMounted, NULL)) {
        kprintf("[Journey] could not listen for a volume — the journey will not say "
                "when one was mounted\n");
    }
}

/* SYSTEM_OP_JOURNEY: the head, the loader's moments off the pass, then the
 * kernel's — the whole answer or none. */
static int SysJourney(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc || op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    Crate *out = &crates[op->out_crate];

    uint16_t              stamped = 0;
    const BoardingMoment *loader  =
        (const BoardingMoment *)BoardingPassStamp(BOARDING_STAMP_MOMENTS, &stamped);
    uint16_t from_loader = loader ? (uint16_t)(stamped / sizeof(BoardingMoment)) : 0;

    JourneyMoment own[JOURNEY_KERNEL_MAX];
    spin_lock(&g_journey_lock);
    uint16_t marked = g_marked;
    memcpy(own, g_marks, marked * sizeof(JourneyMoment));
    spin_unlock(&g_journey_lock);

    uint64_t size = sizeof(JourneyHead) + (uint64_t)(from_loader + marked) * sizeof(JourneyMoment);
    if (out->capacity < size) return ERR_BUFFER_TOO_SMALL;
    uint8_t *kp = crate_out_alloc(out, size);
    if (!kp) return ERR_INVALID_ADDRESS;

    /* A pass holds at most JOURNEY_LOADER_MAX moments, so the loader's count
     * fits its byte. */
    JourneyHead head = {
        .tsc_khz     = (uint32_t)cpu_get_tsc_freq_khz(),
        .count       = (uint16_t)(from_loader + marked),
        .from_loader = (uint8_t)from_loader,
        .flags       = g_cpu_caps.has_hypervisor ? JOURNEY_UNDER_HYPERVISOR : 0u,
    };
    memcpy(kp, &head, sizeof(head));
    if (from_loader) memcpy(kp + sizeof(head), loader, from_loader * sizeof(JourneyMoment));
    memcpy(kp + sizeof(head) + from_loader * sizeof(JourneyMoment), own,
           marked * sizeof(JourneyMoment));

    int rc = crate_out_commit(out, ctx, kp, size);
    crate_buf_free(kp);
    if (rc != OK) return rc;
    out->size = size;
    return OK;
}

error_t JourneyOpsRegister(void)
{
    error_t rc = OpRegistryRegister(OP_KIND(DECK_SYSTEM, SYSTEM_OP_JOURNEY), SysJourney, OP_AUTH_NONE,
                                    "system.journey");
    if (rc != OK && rc != ERR_ALREADY_EXISTS) {
        kprintf("[Journey] register system.journey failed: %s\n", ErrorString(rc));
        return rc;
    }
    return OK;
}
