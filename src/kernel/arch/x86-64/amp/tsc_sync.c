#include "tsc_sync.h"
#include "atomics.h"         // rdtsc, rdtsc_serialized, cpu_pause
#include "cpuid.h"           // g_cpu_caps.has_tsc_adjust
#include "cpu_calibrate.h"   // cpu_ms_to_tsc
#include "klib.h"

/* Intel SDM Vol 3A, "Time-Stamp Counter Adjustment": per logical processor;
 * RDTSC reads the counter plus it, and a write moves what RDTSC reads by the
 * change written. */
#define MSR_IA32_TSC_ADJUST 0x0000003Bu

/* A round is a fraction of a microsecond on a board; the waits only bound a
 * side that has gone — generous, because an emulator's virtual cores are
 * threads its host may set aside for milliseconds. */
#define TSC_MEET_ROUNDS   64u
#define TSC_MEET_WAIT_MS  200u  /* the AP, for the BSP to come to the meeting */
#define TSC_ROUND_WAIT_MS 20u   /* either side, for the other's half of a round */

/* The meeting place: one AP and the BSP, the AP's words on one line and the
 * BSP's on the other. A word handed over is (meeting << 32) | round, so a
 * word left from another meeting is never taken for this one's. */
typedef struct {
    /* Written by the AP. */
    volatile uint64_t ask;      /* (meeting << 32) | (core index + 1) */
    volatile uint64_t pong;     /* the round answered */
    volatile uint64_t ap_tsc;   /* its TSC as it saw the round asked */
    uint8_t           _pad_ap[40];
    /* Written by the BSP. */
    volatile uint64_t ping;     /* the round asked */
    volatile uint64_t answered; /* the meeting its result is for */
    volatile int64_t  offset;   /* the AP's TSC less the BSP's, by the soonest round */
    volatile uint64_t trip;     /* that round's length; UINT64_MAX for no round */
    uint8_t           _pad_bsp[32];
} __attribute__((aligned(64))) TscMeeting;

static TscMeeting        g_meeting;
static volatile uint32_t g_meetings;    /* meetings asked for, counted */
static uint64_t          g_bsp_adjust;  /* the BSP's IA32_TSC_ADJUST */

static inline uint64_t tsc_rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void tsc_wrmsr(uint32_t msr, uint64_t value)
{
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)));
}

static inline uint64_t tsc_magnitude(int64_t v)
{
    return v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
}

/* Until `*word` reads `want`, or `limit` cycles of this core's own TSC. */
static bool tsc_meet_wait(volatile uint64_t *word, uint64_t want, uint64_t limit)
{
    const uint64_t began = rdtsc();
    while (__atomic_load_n(word, __ATOMIC_ACQUIRE) != want) {
        if (rdtsc() - began > limit)
            return false;
        cpu_pause();
    }
    return true;
}

void TscSyncRecordBsp(void)
{
    if (g_cpu_caps.has_tsc_adjust)
        g_bsp_adjust = tsc_rdmsr(MSR_IA32_TSC_ADJUST);
}

/* Each round: the BSP reads its TSC (t0) and asks; the AP reads its TSC as it
 * sees the asking (t1) and answers; the BSP reads its TSC as it sees the
 * answer (t2). Were the two ways as long, t1 was read at (t0 + t2) / 2 on the
 * BSP; however they differ, within half the round of it. The soonest round
 * bounds that the closest. */
void TscSyncServe(uint8_t core_index)
{
    const uint64_t ask = __atomic_load_n(&g_meeting.ask, __ATOMIC_ACQUIRE);
    if ((uint32_t)ask != (uint32_t)core_index + 1u)
        return;
    const uint64_t meeting = ask >> 32;
    if (meeting == g_meeting.answered)
        return;

    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags) :: "memory");

    const uint64_t limit = cpu_ms_to_tsc(TSC_ROUND_WAIT_MS);
    uint64_t best_trip   = UINT64_MAX;
    int64_t  best_offset = 0;
    for (uint32_t round = 1; round <= TSC_MEET_ROUNDS; round++) {
        const uint64_t word = (meeting << 32) | round;
        const uint64_t t0   = rdtsc_serialized();
        __atomic_store_n(&g_meeting.ping, word, __ATOMIC_RELEASE);
        if (!tsc_meet_wait(&g_meeting.pong, word, limit))
            break;
        const uint64_t t2   = rdtsc_serialized();
        const uint64_t t1   = g_meeting.ap_tsc;
        const uint64_t trip = t2 - t0;
        if (trip < best_trip) {
            best_trip   = trip;
            best_offset = (int64_t)(t1 - t0) - (int64_t)(trip / 2u);
        }
    }
    g_meeting.offset = best_offset;
    g_meeting.trip   = best_trip;
    __atomic_store_n(&g_meeting.answered, meeting, __ATOMIC_RELEASE);

    __asm__ volatile("push %0; popfq" :: "r"(rflags) : "memory", "cc");
}

/* One meeting with the BSP: how far this AP's TSC stands from the BSP's, and
 * the round that says so. False when the BSP did not come or left. */
static bool tsc_meet(uint8_t core_index, int64_t *offset, uint64_t *trip)
{
    const uint64_t meeting = __atomic_add_fetch(&g_meetings, 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_meeting.ask, (meeting << 32) | ((uint64_t)core_index + 1u),
                     __ATOMIC_RELEASE);

    const uint64_t first = cpu_ms_to_tsc(TSC_MEET_WAIT_MS);
    const uint64_t limit = cpu_ms_to_tsc(TSC_ROUND_WAIT_MS);
    for (uint32_t round = 1; round <= TSC_MEET_ROUNDS; round++) {
        const uint64_t word = (meeting << 32) | round;
        if (!tsc_meet_wait(&g_meeting.ping, word, round == 1u ? first : limit))
            return false;
        g_meeting.ap_tsc = rdtsc_serialized();
        __atomic_store_n(&g_meeting.pong, word, __ATOMIC_RELEASE);
    }
    if (!tsc_meet_wait(&g_meeting.answered, meeting, limit))
        return false;
    *offset = g_meeting.offset;
    *trip   = g_meeting.trip;
    return *trip != UINT64_MAX;
}

void TscSyncAp(uint8_t core_index)
{
    const bool adjustable = g_cpu_caps.has_tsc_adjust;
    if (adjustable) {
        const uint64_t mine = tsc_rdmsr(MSR_IA32_TSC_ADJUST);
        if (mine != g_bsp_adjust) {
            tsc_wrmsr(MSR_IA32_TSC_ADJUST, g_bsp_adjust);
            kprintf("[TSC] Core %u: IA32_TSC_ADJUST was %ld, the BSP's is %ld — taken\n",
                    (unsigned)core_index, (long)mine, (long)g_bsp_adjust);
        }
    }

    int64_t  offset = 0;
    uint64_t trip   = 0;
    if (!tsc_meet(core_index, &offset, &trip)) {
        kprintf("[TSC] Core %u: the BSP did not come to the meeting — this core's TSC "
                "is not held to the BSP's\n", (unsigned)core_index);
        return;
    }
    if (tsc_magnitude(offset) <= trip)
        return;

    if (!adjustable) {
        kprintf("[TSC] Core %u: TSC %ld cycles off the BSP's (to within %lu), and this "
                "processor has no IA32_TSC_ADJUST to move it by\n",
                (unsigned)core_index, (long)offset, (unsigned long)trip);
        return;
    }
    tsc_wrmsr(MSR_IA32_TSC_ADJUST, tsc_rdmsr(MSR_IA32_TSC_ADJUST) - (uint64_t)offset);

    int64_t  left      = 0;
    uint64_t left_trip = 0;
    if (tsc_meet(core_index, &left, &left_trip))
        kprintf("[TSC] Core %u: TSC %ld cycles off the BSP's (to within %lu) — moved; "
                "%ld left (to within %lu)\n",
                (unsigned)core_index, (long)offset, (unsigned long)trip,
                (long)left, (unsigned long)left_trip);
    else
        kprintf("[TSC] Core %u: TSC %ld cycles off the BSP's (to within %lu) — moved; "
                "the BSP did not come to see what is left\n",
                (unsigned)core_index, (long)offset, (unsigned long)trip);
}
