#include "return_fail.h"
#include "boardroom.h"
#include "atomics.h"
#include "cpu_calibrate.h"

/*
 * RETURNFAIL=on — the seat that says it answers again, which QEMU's stick
 * never does.
 *
 * ‼ A REPRODUCTION, NOT A FEATURE. Compiled in by `make RETURNFAIL=on` and by
 * nothing else.
 *
 * RETURNFAIL fails the first re-mount as a read the medium refused (tagfs.c),
 * and a mount refused that way is owed until `seat:ready` names its seat —
 * asked again then, never on a clock (the note over g_mount_owed). On a board
 * that event is the driver's: AHCI after a port recovery, USB once a unit that
 * answered with a state change commits its cache again. QEMU's stick refuses
 * nothing and recovers from nothing, so it never says it, and every run would
 * end with the mount owed and nothing to ask for it.
 *
 * So this build says it in the driver's place, through the one call a driver
 * makes (BoardroomNoteSeatReady), TAGFS_RETURN_FAIL_READY_MS after the
 * refusal — the time a recovery takes, and the reason it is not said at once:
 * a machine that asks again by itself, on a clock or straight away, asks
 * BEFORE this line, where the scenario sees it (logcheck returnfail).
 */
#if CONFIG_TAGFS_RETURN_FAIL_ONCE

#define TAGFS_RETURN_FAIL_READY_MS 2000u

static volatile uint32_t g_ready_owed;  /* the re-mount was refused, seat:ready not yet said */
static uint8_t           g_ready_seat;
static uint64_t          g_ready_at;

void TagFSReturnFailTripped(uint8_t seat)
{
    g_ready_seat = seat;
    g_ready_at   = rdtsc() + cpu_ms_to_tsc(TAGFS_RETURN_FAIL_READY_MS);
    __atomic_store_n(&g_ready_owed, 1u, __ATOMIC_RELEASE);
}

/* Every core's service pass comes here; one of them says it. */
void TagFSReturnFailSeatReady(void)
{
    if (__atomic_load_n(&g_ready_owed, __ATOMIC_ACQUIRE) == 0 ||
        (int64_t)(rdtsc() - g_ready_at) < 0) {
        return;
    }
    if (__atomic_exchange_n(&g_ready_owed, 0u, __ATOMIC_ACQ_REL) == 0) {
        return;
    }
    kprintf("[TagFS RETURNFAIL] seat %u: its driver says the medium answers again "
            "(seat:ready) — said by this build %u ms after the refusal, in place of "
            "a recovery QEMU's stick never makes\n",
            g_ready_seat, TAGFS_RETURN_FAIL_READY_MS);
    BoardroomNoteSeatReady(BoardroomSeatKind(g_ready_seat), BoardroomSeatIndex(g_ready_seat));
}

#endif  /* CONFIG_TAGFS_RETURN_FAIL_ONCE */
