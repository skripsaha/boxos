#include "read_pace.h"
#include "boardroom.h"
#include "block_fail.h"
#include "cpu_calibrate.h"

/* The seat watched, + 1 (0: none), and its atom in bytes. */
static volatile uint32_t s_seat;
static volatile uint32_t s_atom;

/*
 * The period since the last S0: the least rate of a read of an atom or more
 * (UINT64_MAX: none yet), and of the reads shorter than an atom the longest
 * and the fewest sectors one carried — ONE word, so a short read is folded
 * into one period whole and S0 takes both halves of it in one exchange: two
 * words exchanged one after the other let a read land its time in one period
 * and its sectors in the next, and a period with a time and no sectors would
 * price a seek by nothing (§16.5 п.2 — a race errs only to a flush more).
 * The time in the high bits (0: no short read), the sectors in the low ones
 * (READ_PACE_SECTORS_NONE: none) — a read shorter than an atom carries fewer
 * sectors than an atom holds, and the longest of them is held to the bits it
 * has, which only ever errs to a seek dearer.
 */
#define READ_PACE_SECTOR_BITS   24u
#define READ_PACE_SECTORS_NONE  ((1ull << READ_PACE_SECTOR_BITS) - 1u)
#define READ_PACE_US_MOST       ((1ull << (64u - READ_PACE_SECTOR_BITS)) - 1u)
#define READ_PACE_SHORT_NONE    READ_PACE_SECTORS_NONE

static volatile uint64_t s_rate_least;
static volatile uint64_t s_short;

/* The measure the last S0 handed out: the Settle's holder's alone. */
static ReadPaceSeen s_held;

static void ReadPaceLower(volatile uint64_t* at, uint64_t v)
{
    uint64_t seen = __atomic_load_n(at, __ATOMIC_RELAXED);
    while (v < seen && !__atomic_compare_exchange_n(at, &seen, v, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
        ;
}

/* A short read of `us` and `sectors` folded into the period's word: its time
 * raised, its sectors lowered, in one compare-and-swap. */
static void ReadPaceShort(uint64_t us, uint64_t sectors)
{
    uint64_t seen = __atomic_load_n(&s_short, __ATOMIC_RELAXED);
    us      = us < READ_PACE_US_MOST ? us : READ_PACE_US_MOST;
    sectors = sectors < READ_PACE_SECTORS_NONE ? sectors : READ_PACE_SECTORS_NONE - 1u;
    for (;;) {
        uint64_t most  = seen >> READ_PACE_SECTOR_BITS;
        uint64_t least = seen & READ_PACE_SECTORS_NONE;
        uint64_t want  = (us > most ? us : most) << READ_PACE_SECTOR_BITS | (sectors < least ? sectors : least);
        if (want == seen ||
            __atomic_compare_exchange_n(&s_short, &seen, want, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
            return;
    }
}

void ReadPaceWatch(uint8_t seat)
{
    uint32_t physical = BoardroomSeatPhysicalBytes(seat);
    __atomic_store_n(&s_seat, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&s_rate_least, UINT64_MAX, __ATOMIC_RELAXED);
    __atomic_store_n(&s_short, READ_PACE_SHORT_NONE, __ATOMIC_RELAXED);
    s_held = (ReadPaceSeen){ 0, 0 };
    __atomic_store_n(&s_atom, physical > 4096u ? physical : 4096u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_seat, (uint32_t)seat + 1u, __ATOMIC_RELEASE);
}

void ReadPaceRead(uint8_t seat, uint32_t sectors, uint64_t tsc)
{
    if (__atomic_load_n(&s_seat, __ATOMIC_ACQUIRE) != (uint32_t)seat + 1u || !sectors)
        return;
    uint64_t us    = cpu_tsc_to_us(tsc);
    uint64_t bytes = (uint64_t)sectors * BOARDROOM_SECTOR_BYTES;
    us = us ? us : 1u;
    if (bytes >= __atomic_load_n(&s_atom, __ATOMIC_RELAXED))
        ReadPaceLower(&s_rate_least, bytes * 1000000ull / us);
    else
        ReadPaceShort(us, sectors);
}

void ReadPaceTake(ReadPaceSeen* out)
{
    uint64_t rate   = __atomic_exchange_n(&s_rate_least, UINT64_MAX, __ATOMIC_ACQ_REL);
    uint64_t shorts = __atomic_exchange_n(&s_short, READ_PACE_SHORT_NONE, __ATOMIC_ACQ_REL);
    uint64_t most   = shorts >> READ_PACE_SECTOR_BITS;
    uint64_t least  = (shorts & READ_PACE_SECTORS_NONE) * BOARDROOM_SECTOR_BYTES;
    if (rate != UINT64_MAX)
        s_held.rate = rate ? rate : 1u;
    if (most && s_held.rate) {
        uint64_t moved = least * 1000000ull / s_held.rate;
        s_held.seek_us = most > moved ? most - moved : 0;
    }
    *out = s_held;
#ifdef CONFIG_VOLUME_TESTS
    ReadPaceSeen named;
    if (BlockFailReadPaced(&named.rate, &named.seek_us))
        *out = named;
#endif
}
