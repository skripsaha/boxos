
#include "storage_deck.h"
#include "tagfs.h"
#include "use_context.h"
#include "klib.h"
#include "touch.h"

void storage_deck_init(void)
{
    kprintf("[Storage Deck] Initializing...\n");

    TagFSWatchSeats();

    error_t result = tagfs_init();

    TagFSBootMountSettled();

    if (result != 0) {
        kprintf("[Storage Deck] no volume yet (error=%d) — the machine will "
                "mount when its own medium arrives, and runs without one "
                "until then\n", result);

        TagFSAttendArrival();
        return;
    }

    UseContextRecall();
    kprintf("[Storage Deck] Initialization complete\n");

    uint8_t seat = tagfs_get_seat();
    TouchPublish("volume:mounted", &seat, sizeof(seat));
}