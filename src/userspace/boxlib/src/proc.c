#include "../include/box/proc.h"
#include "../include/box/notify.h"

void proc_exit(int exit_code) {
    // Prepare notify page
    notify_prepare();

    // Add System Deck PROC_TERMINATE prefix (0xFF02)
    notify_add_prefix(0xFF, 0x02);

    // Write exit code to data
    uint32_t code = (uint32_t)exit_code;
    notify_write_data(&code, sizeof(code));

    // Execute (will never return)
    notify_execute();

    // Should never reach here, but loop just in case
    while (1) {
        __asm__ volatile("pause");
    }
}
