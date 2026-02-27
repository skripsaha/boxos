#ifndef BOXOS_LIMITS_H
#define BOXOS_LIMITS_H

// ============================================================================
// BOXOS SYSTEM LIMITS
// ============================================================================

// Process limits
#define BOXOS_MAX_PROCESSES      256
#define BOXOS_MAX_OPEN_FILES     256

// Event system limits
#define BOXOS_EVENT_RING_SIZE    2048  // Power of 2
#define BOXOS_MAX_PENDING_RESULTS 256

// Hardware limits
#define BOXOS_IDT_ENTRIES        256
#define BOXOS_ATA_SECTOR_SIZE    512
#define BOXOS_PAGE_SIZE          4096

// TagFS limits
#define BOXOS_TAGFS_MAX_FILES    1024  // Phase 1 limit
#define BOXOS_TAGFS_MAX_TAG_INDEX 1024
#define BOXOS_TAGFS_INODE_SIZE   512
#define BOXOS_JOURNAL_ENTRY_COUNT 512

// Keyboard/input limits
#define BOXOS_KEYBOARD_BUFFER_SIZE    256
#define BOXOS_KEYBOARD_LINE_BUFFER_SIZE 256
#define BOXOS_INPUT_BUFFER_SIZE       256

// USB limits
#define BOXOS_XHCI_MAX_SLOTS     256

// AHCI limits
#define BOXOS_AHCI_FIS_SIZE      256
#define BOXOS_AHCI_FIS_ALIGN     256
#define BOXOS_AHCI_CLB_SIZE      1024
#define BOXOS_AHCI_CLB_ALIGN     1024

#endif // BOXOS_LIMITS_H
