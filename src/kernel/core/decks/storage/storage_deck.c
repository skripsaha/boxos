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
#include "tagfs_context.h"
#include "klib.h"

void storage_deck_init(void)
{
    kprintf("[Storage Deck] Initializing...\n");

    error_t result = tagfs_init();

    /* The boot mount has been tried, whatever came of it. Until this is said,
     * a medium arriving belongs to the boot that is still happening and the
     * guide loop leaves it alone — otherwise a K-Core and this one mount the
     * same volume at the same time. */
    TagFSBootMountSettled();

    if (result != 0) {
        kprintf("[Storage Deck] ERROR: Failed to initialize TagFS (error=%d)\n", result);
        TagFSState *fs = tagfs_get_state();
        kprintf("[Storage Deck] TagFS state: %s\n",
                fs ? (fs->initialized ? "initialized" : "NOT initialized") : "NULL");
        return;
    }

    tagfs_context_init();
    kprintf("[Storage Deck] Initialization complete\n");
}
