#ifndef BOXOS_LIMITS_H
#define BOXOS_LIMITS_H

#define MAX_PROCESSES      4096

// Per-process memory limits (single source of truth)
#define BOXOS_USER_HEAP_MAX_SIZE    (256 * 1024 * 1024)   // 256MB max heap per process
#define BOXOS_PROC_MAX_BINARY_SIZE  (64 * 1024 * 1024)    // 64MB max binary size
#define BOXOS_PROC_MAX_BUFFER_SIZE  (64 * 1024 * 1024)    // 64MB max buffer size

// A user stack — the program's main one and every strand's — is a reserve of
// address space that fills in on first touch, so what a stack costs is what it
// has used, and what it may use is this. Below each reserve lies a gap that is
// never mapped: the stack running out lands in it and stops there, instead of
// in whatever the neighbour below happens to be.
#define BOXOS_USER_STACK_RESERVE    (8 * 1024 * 1024)     // 8MB a stack may grow to
#define BOXOS_USER_STACK_GAP        (1 * 1024 * 1024)     // 1MB never mapped below it

#define IDT_ENTRIES        256
#define ATA_SECTOR_SIZE    512

#define INPUT_BUFFER_SIZE         1024

#define XHCI_MAX_SLOTS     256

#define AHCI_FIS_SIZE      256
#define AHCI_FIS_ALIGN     256
#define AHCI_CLB_SIZE      1024
#define AHCI_CLB_ALIGN     1024

#endif // BOXOS_LIMITS_H
