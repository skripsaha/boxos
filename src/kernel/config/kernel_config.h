#ifndef KERNEL_CONFIG_H
#define KERNEL_CONFIG_H

#include "boxos_limits.h"

// ============================================================================
// BOXOS KERNEL CONFIGURATION
// ============================================================================
//
// Centralized configuration for all kernel subsystems.
// This file replaces hardcoded magic numbers throughout the codebase.
//
// Production deployment: Adjust these values based on target hardware
// and workload requirements.
//
// ============================================================================

// ============================================================================
// MEMORY LAYOUT
// ============================================================================

// Boot & Kernel addresses
#define CONFIG_KERNEL_LOAD_ADDR 0x10000ULL   // 64KB - kernel load address
#define CONFIG_PAGE_TABLE_BASE 0x820000ULL   // 8MB - page tables
#define CONFIG_KERNEL_STACK_BASE 0x900000ULL // 9MB - kernel stack

// User space layout (isolated from kernel identity mapping)
#define CONFIG_USER_CODE_BASE 0x20000000ULL  // 512MB - user code start
#define CONFIG_USER_STACK_BASE 0x20100000ULL // 513MB - user stack start
#define CONFIG_USER_RINGS_BASE 0x20200000ULL // 514MB - ring buffers start

// VMM memory regions
#define CONFIG_VMM_KERNEL_BASE 0xFFFF800000000000ULL
#define CONFIG_VMM_KERNEL_HEAP_SIZE (1ULL << 30)        // 1GB kernel heap
#define CONFIG_VMM_USER_BASE 0x0000000000400000ULL      // 4MB (ELF standard)
#define CONFIG_VMM_USER_STACK_TOP 0x00007FFFFFFFE000ULL // ~128TB

// PMM limits
#define CONFIG_PMM_MAX_MEMORY (128ULL * 1024 * 1024 * 1024) // 128GB

// Page size (MUST match hardware)
#define CONFIG_PAGE_SIZE 4096

// ============================================================================
// PROCESS MANAGEMENT
// ============================================================================

#define CONFIG_PROCESS_MAX_COUNT 256      // Maximum concurrent processes (match process.h)
#define CONFIG_KERNEL_STACK_PAGES 4       // 4 pages = 16KB kernel stack per process
#define CONFIG_KERNEL_STACK_GUARD_PAGES 1 // Kernel stack guard pages (unmapped, triggers page fault on overflow)
#define CONFIG_KERNEL_STACK_TOTAL_PAGES (CONFIG_KERNEL_STACK_PAGES + CONFIG_KERNEL_STACK_GUARD_PAGES)
#define CONFIG_USER_STACK_SIZE_KB 64 // Configurable: 64KB for shell stability
#define CONFIG_USER_STACK_SIZE (CONFIG_USER_STACK_SIZE_KB * 1024)
#define CONFIG_USER_STACK_PAGES (CONFIG_USER_STACK_SIZE / CONFIG_PAGE_SIZE) // Auto-calculated
#define CONFIG_USER_STACK_GUARD_PAGES 1                                     // Guard page below stack (4KB)
#define CONFIG_USER_STACK_TOTAL_PAGES (CONFIG_USER_STACK_PAGES + CONFIG_USER_STACK_GUARD_PAGES)

// Cabin heap configuration
#define CONFIG_USER_HEAP_INITIAL_SIZE (64 * 1024)    // 64KB initial heap
#define CONFIG_USER_HEAP_MAX_SIZE (16 * 1024 * 1024) // 16MB max heap per process
#define CONFIG_USER_HEAP_INITIAL_PAGES (CONFIG_USER_HEAP_INITIAL_SIZE / CONFIG_PAGE_SIZE)

#define CONFIG_USER_BSS_SIZE (16 * 1024)             // 16KB BSS allocation
#define CONFIG_PROCESS_MAX_MEMORY (16 * 1024 * 1024) // 16MB per process max
#define CONFIG_ENFORCE_PROCESS_LIMIT 1               // Enforce process limit (0=unlimited)

// Process binary and buffer limits
#define CONFIG_PROC_MAX_BINARY_SIZE (16 * 1024 * 1024) // 16MB max binary size
#define CONFIG_PROC_MAX_BUFFER_SIZE (16 * 1024 * 1024) // 16MB max buffer size

// ============================================================================
// WORKFLOW SYSTEM (Event-Driven Engine)
// ============================================================================

// Ring buffer sizes (MUST be power of 2)
#define CONFIG_EVENT_RING_SIZE 2048 // Submission queue depth (655 KB, 161 pages, 2047 usable slots)
#define CONFIG_RESULT_RING_SIZE 256 // Completion queue depth

// EventRing overflow protection
#define CONFIG_EVENT_RING_SYSTEM_RESERVED 256 // Reserved slots for system events
#define CONFIG_EVENT_RING_USER_MAX \
    (CONFIG_EVENT_RING_SIZE - CONFIG_EVENT_RING_SYSTEM_RESERVED) // 1792 user slots

// Timeout for blocked processes (milliseconds)
#define CONFIG_EVENTRING_BLOCK_TIMEOUT_MS 500

// ============================================================================
// DEPRECATED / RESERVED FOR FUTURE
// ============================================================================
#if 0 // Workflow Engine and Routing (NOT IMPLEMENTED)
#define CONFIG_MAX_ROUTING_STEPS 8
#define CONFIG_ROUTING_TABLE_SIZE 64
#define CONFIG_DECK_QUEUE_SIZE 128
#define CONFIG_WORKFLOW_MAX_EVENTS 16
#define CONFIG_WORKFLOW_NAME_MAX 32
#define CONFIG_WORKFLOW_MAX_DEPENDENCIES 8
#endif

// Event sizes (authoritative definitions in events.h)
// CONFIG_EVENT_DATA_SIZE = 256 bytes
// CONFIG_RESPONSE_DATA_SIZE = 4064 bytes

// ============================================================================
// FILESYSTEM (TagFS)
// ============================================================================

#define CONFIG_TAGFS_VERSION 2
#define CONFIG_TAGFS_BLOCK_SIZE 4096                   // Must match PAGE_SIZE
#define CONFIG_TAGFS_MAX_FILES 65536                   // Maximum files
#define CONFIG_TAGFS_MAX_FILE_SIZE (4ULL << 30)        // 4GB per file
#define CONFIG_TAGFS_INODE_SIZE BOXOS_TAGFS_INODE_SIZE // Inode struct size

// Tag limits
#define CONFIG_TAGFS_MAX_TAGS_PER_FILE 32 // Tags per file
#define CONFIG_TAGFS_TAG_KEY_SIZE 32      // Tag key length
#define CONFIG_TAGFS_TAG_VALUE_SIZE 64    // Tag value length
#define CONFIG_TAGFS_MAX_TAG_INDEX 1024   // Unique tags
#define CONFIG_TAGFS_MAX_CONTEXT_TAGS 16  // User context tags

// File descriptor table
#define CONFIG_MAX_OPEN_FILES 256 // Open file handles

// ============================================================================
// TIMERS & INTERRUPTS
// ============================================================================

#define CONFIG_MAX_TIMERS 64         // Concurrent timers
#define CONFIG_PIT_FREQUENCY_HZ 100  // Timer frequency (100 Hz = 10ms tick)
#define CONFIG_PIT_BASE_FREQ 1193182 // PIT hardware frequency

// Interrupt vectors
#define CONFIG_SYSCALL_VECTOR 0x80 // INT 0x80 (kernel_notify)
#define CONFIG_IRQ_TIMER 32        // IRQ 0 → INT 0x20

// ============================================================================
// SCHEDULER
// ============================================================================

#define CONFIG_TIME_SLICE_TICKS 10         // 100ms at 100Hz (LARGE - workflow-driven!)
#define CONFIG_WATCHDOG_TIMEOUT_TICKS 1000 // 10 seconds at 100Hz
#define CONFIG_WATCHDOG_CHECK_INTERVAL 100 // 1 second at 100Hz

// ============================================================================
// DRIVERS
// ============================================================================

// ATA/IDE
#define CONFIG_ATA_TIMEOUT_MS 5000 // 5 second timeout
#define CONFIG_ATA_SECTOR_SIZE 512 // Hardware constant
#define CONFIG_ATA_DMA_DEBUG 0     // 0=production (errors only), 1=verbose debug
#define CONFIG_ATA_DMA_ASYNC 1     // Enable async DMA I/O (0 = sync PIO fallback)
#define CONFIG_ATA_MAX_RETRIES 3   // Disk I/O retry limit

// Async I/O dispatch tuning
#define CONFIG_ASYNC_DISPATCH_INTERVAL_MS 1      // How often Guide checks async queue
#define CONFIG_DMA_TIMEOUT_CHECK_INTERVAL_MS 100 // How often to check for DMA timeouts

// AHCI (Advanced Host Controller Interface)
#define CONFIG_AHCI_DRIVER 1                // Enable AHCI driver (Phase 1: test mode)
#define CONFIG_AHCI_MAX_RETRIES 3           // AHCI command retry limit
#define CONFIG_AHCI_MAX_COMRESET_ATTEMPTS 3 // Port COMRESET retry limit

// Keyboard
#define CONFIG_KEYBOARD_BUFFER_SIZE 256 // Input line buffer

// Serial
#define CONFIG_SERIAL_BAUD_RATE 115200 // Serial port speed

// ACPI
#define CONFIG_ACPI_DEBUG 0         // Verbose ACPI logging
#define CONFIG_ACPI_USE_XSDT 1      // Prefer XSDT over RSDT
#define CONFIG_ACPI_FALLBACK_QEMU 1 // Enable QEMU fallback shutdown

// ============================================================================
// MEMORY ALLOCATION
// ============================================================================

#define CONFIG_KMALLOC_MIN_SIZE 16  // Minimum allocation
#define CONFIG_KMALLOC_ALIGNMENT 16 // Allocation alignment

// ============================================================================
// DEBUGGING & LOGGING
// ============================================================================

// Master debug switch: Set to 0 to disable ALL debug output
#ifndef CONFIG_DEBUG_ENABLED
#define CONFIG_DEBUG_ENABLED 1
#endif

// Set to 1 to enable debug output
#ifndef CONFIG_DEBUG_MODE
#define CONFIG_DEBUG_MODE 0
#endif

// Test configuration
#ifndef CONFIG_RUN_STARTUP_TESTS
#define CONFIG_RUN_STARTUP_TESTS 1 // Set to 1 for testing, 0 for production
#endif

#define CONFIG_DEBUG_PMM 0       // PMM debug prints
#define CONFIG_DEBUG_VMM 0       // VMM debug prints
#define CONFIG_DEBUG_PROCESS 0   // Process debug prints
#define CONFIG_DEBUG_SCHEDULER 0 // Scheduler debug prints
#define CONFIG_DEBUG_WORKFLOW 0  // Workflow debug prints
#define CONFIG_DEBUG_TAGFS 0     // TagFS debug prints

// ============================================================================
// PERFORMANCE TUNING
// ============================================================================

// Guide batch processing
#define CONFIG_GUIDE_BATCH_SIZE 16 // Buckets per scan

// Memory prefetching
#define CONFIG_PREFETCH_ENABLED 1 // Enable prefetch hints

// Cache line size
#define CONFIG_CACHE_LINE_SIZE 64 // x86-64 standard

// ============================================================================
// USERSPACE EXECUTION
// ============================================================================

// Set to 1 to start initial userspace process
#ifndef CONFIG_START_USERSPACE
#define CONFIG_START_USERSPACE 1
#endif

// ============================================================================
// VALIDATION MACROS
// ============================================================================

// Compile-time checks
_Static_assert((CONFIG_EVENT_RING_SIZE & (CONFIG_EVENT_RING_SIZE - 1)) == 0,
               "CONFIG_EVENT_RING_SIZE must be power of 2");
_Static_assert((CONFIG_RESULT_RING_SIZE & (CONFIG_RESULT_RING_SIZE - 1)) == 0,
               "CONFIG_RESULT_RING_SIZE must be power of 2");
_Static_assert(CONFIG_TAGFS_BLOCK_SIZE == CONFIG_PAGE_SIZE,
               "CONFIG_TAGFS_BLOCK_SIZE must match CONFIG_PAGE_SIZE");
_Static_assert((CONFIG_USER_STACK_SIZE % CONFIG_PAGE_SIZE) == 0,
               "CONFIG_USER_STACK_SIZE must be page-aligned");
_Static_assert(CONFIG_USER_STACK_SIZE >= 4096,
               "CONFIG_USER_STACK_SIZE must be at least 4KB");
_Static_assert((CONFIG_USER_HEAP_INITIAL_SIZE % CONFIG_PAGE_SIZE) == 0,
               "CONFIG_USER_HEAP_INITIAL_SIZE must be page-aligned");
_Static_assert(CONFIG_USER_HEAP_MAX_SIZE >= CONFIG_USER_HEAP_INITIAL_SIZE,
               "CONFIG_USER_HEAP_MAX_SIZE must be >= initial size");

#endif // KERNEL_CONFIG_H
