#include "../include/box/proc.h"
#include "../include/box/notify.h"

void proc_exit(int exit_code) {
    // Prepare notify page
    box_notify_prepare();

    // Add System Deck PROC_TERMINATE prefix (0xFF02)
    box_notify_add_prefix(0xFF, 0x02);

    // Write exit code to data
    uint32_t code = (uint32_t)exit_code;
    box_notify_write_data(&code, sizeof(code));

    // Execute (will never return)
    box_notify_execute();

    // Should never reach here, but loop just in case
    while (1) {
        __asm__ volatile("pause");
    }
}
