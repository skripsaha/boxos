#ifndef KERNEL_CONFIG_H
#define KERNEL_CONFIG_H

#include "boxos_limits.h"

#define CONFIG_KERNEL_LOAD_ADDR 0xFFFFFFFF80100000ULL  // Higher-half kernel VMA (must match linker.ld)
#define CONFIG_KERNEL_PHYS_ADDR 0x100000ULL            // 1MB - physical load address
#define CONFIG_KERNEL_VMA_OFFSET 0xFFFFFFFF80000000ULL // Higher-half base (VMA - phys = offset)
// PAGE_TABLE_BASE and KERNEL_STACK_BASE are now DYNAMIC — placed after kernel_end
// by the bootloader. Available at runtime via boot_info_t (boot_info.h).

#define CONFIG_USER_CODE_BASE 0x20000000ULL  // 512MB - user code start
#define CONFIG_USER_STACK_BASE 0x20100000ULL // 513MB - user stack start
#define CONFIG_USER_RINGS_BASE 0x20200000ULL // 514MB - ring buffers start

#define CONFIG_VMM_KERNEL_BASE 0xFFFF800000000000ULL
#define CONFIG_VMM_KERNEL_HEAP_SIZE (1ULL << 30)        // 1GB kernel heap
#define CONFIG_VMM_USER_BASE 0x0000000000400000ULL      // 4MB (ELF standard)
#define CONFIG_VMM_USER_STACK_TOP 0x00007FFFFFFFE000ULL // ~128TB

#define CONFIG_PAGE_SIZE 4096

#define CONFIG_PROCESS_MAX_COUNT MAX_PROCESSES
#define CONFIG_KERNEL_STACK_PAGES 4
#define CONFIG_KERNEL_STACK_GUARD_PAGES 1
#define CONFIG_KERNEL_STACK_TOTAL_PAGES (CONFIG_KERNEL_STACK_PAGES + CONFIG_KERNEL_STACK_GUARD_PAGES)
#define CONFIG_USER_STACK_SIZE_KB 64 // 64KB for shell stability
#define CONFIG_USER_STACK_SIZE (CONFIG_USER_STACK_SIZE_KB * 1024)
#define CONFIG_USER_STACK_PAGES (CONFIG_USER_STACK_SIZE / CONFIG_PAGE_SIZE)
#define CONFIG_USER_STACK_GUARD_PAGES 1
#define CONFIG_USER_STACK_TOTAL_PAGES (CONFIG_USER_STACK_PAGES + CONFIG_USER_STACK_GUARD_PAGES)

#define CONFIG_USER_HEAP_INITIAL_SIZE (64 * 1024) // 64KB initial heap
#define CONFIG_USER_HEAP_MAX_SIZE BOXOS_USER_HEAP_MAX_SIZE
#define CONFIG_USER_HEAP_INITIAL_PAGES (CONFIG_USER_HEAP_INITIAL_SIZE / CONFIG_PAGE_SIZE)

#define CONFIG_USER_BSS_SIZE (16 * 1024)

#define CONFIG_PROC_MAX_BINARY_SIZE BOXOS_PROC_MAX_BINARY_SIZE
#define CONFIG_PROC_MAX_BUFFER_SIZE BOXOS_PROC_MAX_BUFFER_SIZE

// Ring buffer capacities are defined at their source of truth:
// - PocketRing: boxos_sizes.h (POCKET_RING_CAPACITY)
// - ResultRing: boxos_sizes.h (RESULT_RING_CAPACITY)
// - ReadyQueue: boxos_sizes.h (READY_QUEUE_CAPACITY)

#define CONFIG_TAGFS_VERSION 2
#define CONFIG_TAGFS_BLOCK_SIZE 4096
#define CONFIG_TAGFS_MAX_FILES 65536
#define CONFIG_TAGFS_MAX_FILE_SIZE (4ULL << 30) // 4GB per file
#define CONFIG_TAGFS_INODE_SIZE TAGFS_INODE_SIZE

#define CONFIG_TAGFS_MAX_TAGS_PER_FILE 32
#define CONFIG_TAGFS_TAG_KEY_SIZE 32
#define CONFIG_TAGFS_TAG_VALUE_SIZE 64
#define CONFIG_TAGFS_MAX_TAG_INDEX 1024
#define CONFIG_TAGFS_MAX_CONTEXT_TAGS 16

#define CONFIG_MAX_OPEN_FILES MAX_OPEN_FILES

#define CONFIG_MAX_TIMERS 64
#define CONFIG_PIT_FREQUENCY_HZ 100
#define CONFIG_PIT_BASE_FREQ 1193182 // PIT hardware frequency

#define CONFIG_SYSCALL_VECTOR 0x80 // INT 0x80 (kernel_notify)
#define CONFIG_IRQ_TIMER 32        // IRQ 0 -> INT 0x20

#define CONFIG_TIME_SLICE_TICKS 10         // 100ms at 100Hz (LARGE - workflow-driven!)
#define CONFIG_WATCHDOG_TIMEOUT_TICKS 1000 // 10 seconds at 100Hz
#define CONFIG_WATCHDOG_CHECK_INTERVAL 100 // 1 second at 100Hz

#define CONFIG_ATA_TIMEOUT_MS 5000
#define CONFIG_ATA_SECTOR_SIZE 512 // Hardware constant
#define CONFIG_ATA_DMA_DEBUG 0     // 0=production (errors only), 1=verbose debug
#define CONFIG_ATA_DMA_ASYNC 1
#define CONFIG_ATA_MAX_RETRIES 3

#define CONFIG_ASYNC_DISPATCH_INTERVAL_MS 1
#define CONFIG_DMA_TIMEOUT_CHECK_INTERVAL_MS 100
#define CONFIG_ASYNC_IO_QUEUE_TIMEOUT_MS 5000 // 5s timeout for pending I/O in queue
#define CONFIG_ASYNC_IO_BGND_SERVE_INTERVAL 8 // dequeue 1 BGND per N DATA dequeues
#define CONFIG_FRIEND_ZONE_CACHE_MAX_PAGES 64 // max pages cached per Friend zone
#define CONFIG_LISTEN_TABLE_MAX_ENTRIES 1024  // safety limit on total listeners
#define CONFIG_PHYS_ZONE_DMA32_END 0x40000000ULL  // 1GB — DMA32 safe boundary
#define CONFIG_PHYS_ZONE_USER_END  0x100000000ULL // 4GB — identity map limit
#define CONFIG_KCORE_POP_SPIN_LIMIT 1000000

#define CONFIG_AHCI_DRIVER 1
#define CONFIG_AHCI_MAX_RETRIES 3
#define CONFIG_AHCI_MAX_COMRESET_ATTEMPTS 3
#define CONFIG_AHCI_CMD_TIMEOUT_MS 2000
#define CONFIG_AHCI_MAX_PORTS 32
#define CONFIG_AHCI_MAX_SLOTS 32

/* AHCI boot-time self-test: a one-sector READ probe issued in ahci_init.
 * Useful on QEMU but on real HW it stalls boot if the device is slow or
 * the LBA happens to be unreadable. Disabled in production. */
#ifndef CONFIG_AHCI_SELFTEST
#define CONFIG_AHCI_SELFTEST 0
#endif

/* Mirror every Video* user-side print to COM1 so headless QEMU runs leave
 * a complete trace of shell output in build/serial.log. Adds one
 * `serial_putchar` per VGA char — bounded by the serial transmit FIFO so
 * worst-case latency is ~87us/char at 115200 baud. Default ON in dev/QEMU,
 * turn OFF for production where COM1 may not be wired. */
#ifndef CONFIG_VIDEO_SERIAL_MIRROR
#define CONFIG_VIDEO_SERIAL_MIRROR 1
#endif

/* ============================================================================
 *  Scheduler / runqueue / process / IPC tunables
 * ============================================================================ */

/* Adaptive tick-rate bounds (Hz). */
#define CONFIG_SCHED_MIN_TICK_HZ      10
#define CONFIG_SCHED_MAX_TICK_HZ      500
#define CONFIG_SCHED_DEFAULT_TICK_HZ  250

/* Core parking. */
#define CONFIG_SCHED_PARK_IDLE_TICKS    100
#define CONFIG_SCHED_UNPARK_LOAD_THRESH 2

/* Affinity: ticks since last run after which cache is considered cold. */
#define CONFIG_SCHED_AFFINITY_WARM_TICKS 5

/* How often (in global ticks) scheduler_recalc_parameters() runs. */
#define CONFIG_SCHED_RECALC_INTERVAL  10

/* Fairness and starvation limits. */
#define CONFIG_SCHED_MAX_CONSECUTIVE_RUNS 5
#define CONFIG_SCHED_MIN_FAIRNESS         2
#define CONFIG_SCHED_MAX_FAIRNESS         20
#define CONFIG_SCHED_MIN_STARVATION       10
#define CONFIG_SCHED_MAX_STARVATION       100

/* Per-core runqueue capacity. Grows from INITIAL_CAP up to MAX_CAP via
 * runqueue_grow. */
#define CONFIG_RUNQUEUE_INITIAL_CAP   64
#define CONFIG_RUNQUEUE_MAX_CAP       4096

/* Process bookkeeping. */
#define CONFIG_PROCESS_HASH_SIZE      256   /* power-of-two for mask hashing */
#define CONFIG_PROCESS_CLEANUP_BATCH  8     /* drain at most N corpses per tick */
#define CONFIG_PROCESS_POISON_MAGIC   0xDEADDEADu
#define CONFIG_IDLE_PID               0

/* IPC routing limits. */
#define CONFIG_BUF_MAX_COUNT          64    /* legacy buffer registry capacity */
#define CONFIG_BROADCAST_TAG_MAX      64    /* max tag chars in a broadcast() */
#define CONFIG_BROADCAST_TARGETS_MAX  256   /* max recipients per broadcast */

#define CONFIG_KEYBOARD_BUFFER_SIZE 256

#define CONFIG_SERIAL_BAUD_RATE 115200

/* ACPI 6.5 §5.2.5.3: when RSDP revision >= 2 and XsdtAddress != 0 the OS
 * MUST use XSDT (32-bit RSDT may be stale on those firmwares). Selection
 * is runtime, never a compile-time switch. CONFIG_ACPI_FALLBACK_QEMU was
 * removed: emulator-targeted defaults are not part of the spec. */
#define CONFIG_ACPI_DEBUG 0

#define CONFIG_KMALLOC_MIN_SIZE 16
#define CONFIG_KMALLOC_ALIGNMENT 16

/* TagFS BCDC compressor — runtime-tuneable. The on-disk header still pins
 * `dictionary_id` to a single byte (ID 0..255), so MAX_DICTS keeps that
 * upper bound, but everything else is dynamic. Unused dicts cost zero
 * memory: each struct is kmalloc'd on first BcdcCreateDictionary() and
 * holds its own spinlock. Policies live in a growable list. */
#define CONFIG_BCDC_MAX_DICTS         256   /* hard cap from on-disk uint8_t */
#define CONFIG_BCDC_DEFAULT_DICT_SIZE 8192  /* bytes per dictionary buffer */
#define CONFIG_BCDC_POLICY_INITIAL    16    /* policy table starts here */
#define CONFIG_BCDC_POLICY_MAX        4096  /* policy table hard ceiling */
#define CONFIG_BCDC_LZ_HASH_BITS      12    /* 4096-entry hash chain head[] */

#ifndef CONFIG_DEBUG_ENABLED
#define CONFIG_DEBUG_ENABLED 0
#endif

#ifndef CONFIG_DEBUG_MODE
#define CONFIG_DEBUG_MODE 0
#endif

#ifndef CONFIG_RUN_STARTUP_TESTS
#define CONFIG_RUN_STARTUP_TESTS 1
#endif

#define CONFIG_DEBUG_PMM 0
#define CONFIG_DEBUG_VMM 0
#define CONFIG_DEBUG_PROCESS 0
#define CONFIG_DEBUG_SCHEDULER 1
#define CONFIG_DEBUG_USE_CONTEXT 1
#define CONFIG_DEBUG_WORKFLOW 0
#define CONFIG_DEBUG_TAGFS 0

#define CONFIG_PERF_TRACE 0 // 1=in-memory ring buffer trace (fast: ~15 cycles, no serial I/O)
#define CONFIG_GUIDE_BATCH_SIZE 16
#define CONFIG_PREFETCH_ENABLED 1
#define CONFIG_CACHE_LINE_SIZE 64 // x86-64 standard

#ifndef CONFIG_START_USERSPACE
#define CONFIG_START_USERSPACE 1
#endif

// AMP Configuration
#define CONFIG_MAX_CORES 256
#define CONFIG_AP_TRAMPOLINE_PHYS 0x8000
#define CONFIG_AP_STACK_PAGES 4

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
