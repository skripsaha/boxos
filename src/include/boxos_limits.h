#ifndef BOXOS_LIMITS_H
#define BOXOS_LIMITS_H

#define MAX_PROCESSES      4096
#define MAX_OPEN_FILES     4096

// Per-process memory limits (single source of truth)
#define BOXOS_USER_HEAP_MAX_SIZE    (256 * 1024 * 1024)   // 256MB max heap per process
#define BOXOS_PROC_MAX_BINARY_SIZE  (64 * 1024 * 1024)    // 64MB max binary size
#define BOXOS_PROC_MAX_BUFFER_SIZE  (64 * 1024 * 1024)    // 64MB max buffer size

#define IDT_ENTRIES        256
#define ATA_SECTOR_SIZE    512

#define TAGFS_MAX_FILES    1024  // Default for formatting; runtime reads from superblock
#define TAGFS_MAX_TAG_INDEX 1024
#define TAGFS_INODE_SIZE   512
#define JOURNAL_ENTRY_COUNT 512

#define KEYBOARD_BUFFER_SIZE      256
#define KEYBOARD_LINE_BUFFER_SIZE 1024
#define INPUT_BUFFER_SIZE         1024

#define XHCI_MAX_SLOTS     256

#define AHCI_FIS_SIZE      256
#define AHCI_FIS_ALIGN     256
#define AHCI_CLB_SIZE      1024
#define AHCI_CLB_ALIGN     1024

#endif // BOXOS_LIMITS_H
