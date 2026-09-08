/*
 * Storage Deck — initialization (Phase 12: post-prefix-chain).
 *
 * The legacy prefix-chain dispatcher and its 192-byte request structs are
 * gone. All storage operations now flow through storage_ops.c via the
 * Manifest path. This file retains only the bring-up helper that wires
 * TagFS into the kernel.
 */

#include "storage_deck.h"
#include "tagfs.h"
#include "use_context.h"
#include "klib.h"
#include "touch.h"

void storage_deck_init(void)
{
    kprintf("[Storage Deck] Initializing...\n");

    /* Listening BEFORE looking. A medium that turns up while the boot mount is
     * running is announced to whoever is subscribed at that moment, so
     * subscribing afterwards is subscribing too late. */
    TagFSWatchSeats();

    error_t result = tagfs_init();

    /* The boot mount has been tried, whatever came of it. Until this is said,
     * a medium arriving belongs to the boot that is still happening and the
     * guide loop leaves it alone — otherwise a K-Core and this one mount the
     * same volume at the same time. */
    TagFSBootMountSettled();

    if (result != 0) {
        kprintf("[Storage Deck] no volume yet (error=%d) — the machine will "
                "mount when its own medium arrives, and runs without one "
                "until then\n", result);

        /* One catch-up look. An arrival announced while the boot mount was
         * still running reached a listener that was gated shut by the line
         * above, and nothing would have said it a second time. */
        TagFSAttendArrival();
        return;
    }

    /* The volume is up: give the Use Context the numbers this volume has for
     * its tags, so the scheduler's tier and TagFS's narrowing agree with it
     * from the first strand dispatched. Said on every road to a mounted
     * volume — this one, the late arrival, the return. */
    UseContextRebind();
    kprintf("[Storage Deck] Initialization complete\n");

    /* Said on both roads to a mounted volume, this one and the late one, so
     * anything waiting for a filesystem waits for one thing rather than
     * knowing which way it might arrive. */
    uint8_t seat = tagfs_get_seat();
    TouchPublish("volume:mounted", &seat, sizeof(seat));
}
