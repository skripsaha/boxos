#ifndef KERNEL_CONFIG_H
#define KERNEL_CONFIG_H

#include "boxos_limits.h"

#define CONFIG_KERNEL_PHYS_ADDR 0x100000ULL            // 1MB - physical load address
#define CONFIG_KERNEL_VMA_OFFSET 0xFFFFFFFF80000000ULL // Higher-half base (VMA - phys = offset)
// PAGE_TABLE_BASE and KERNEL_STACK_BASE are now DYNAMIC — placed after kernel_end
// by the bootloader. Available at runtime via boot_info_t (boot_info.h).

#define CONFIG_PAGE_SIZE 4096

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

#define CONFIG_PROC_MAX_BINARY_SIZE BOXOS_PROC_MAX_BINARY_SIZE

// Ring buffer capacities are defined at their source of truth:
// - PocketRing: boxos_sizes.h (POCKET_RING_CAPACITY)
// - ResultRing: boxos_sizes.h (RESULT_RING_CAPACITY)
// - ReadyQueue: boxos_sizes.h (READY_QUEUE_CAPACITY)

#define CONFIG_SYSCALL_VECTOR 0x80 // INT 0x80 (kernel_notify)

#define CONFIG_ATA_TIMEOUT_MS 5000

/*
 * And how long the DRIVE is given to carry a command out, which is a different
 * question from how long a register handshake may take.
 *
 * A drive that meets a marginal sector retries the head internally before it
 * answers, and how long it may spend doing that belongs to the drive: seven
 * seconds is ordinary on a desktop disk without configurable error recovery,
 * and the standard sets no ceiling at all. Under the handshake's five seconds
 * every one of those reads was a timeout on a disk that would have answered.
 *
 * Thirty seconds is what every host stack gives a disk command — Linux's SCSI
 * layer uses exactly this number (drivers/scsi/sd.h, SD_TIMEOUT) — and it is
 * the LAST RESORT: the status register's own bits end the wait first, and a
 * bus with nothing on it reads 0xFF and ends it immediately.
 */
#define CONFIG_ATA_IO_TIMEOUT_MS 30000

#define CONFIG_ATA_MAX_RETRIES 3

/* BMIDE watchdog liveness-of-last-resort bound (bmide_watchdog_scan TIER 2b).
 * NOT an I/O deadline: the watchdog recovers every OTHER wedge purely by
 * hardware event (latched INTRQ, engine-gone-idle, 0xFF device-gone). The one
 * software-unobservable case — a DMA engine frozen mid-transfer with ACTIVE
 * stuck =1, no INTRQ, no error — is indistinguishable from a healthy in-flight
 * transfer without a clock. Every BMIDE command is <=ATA_ASYNC_MAX_SECTORS
 * (4 KB), so a healthy transfer completes in <1 ms even at MWDMA0; a command
 * whose engine still claims ACTIVE after this bound (5000x margin) is a frozen
 * drive -> SRST. Bounds hardware silence, never I/O duration. */
#define CONFIG_ATA_LIVENESS_MS 5000

/* On-demand BMIDE watchdog TIER-2 diagnostic (bmide_wedge_selftest). OFF by
 * default: it drives a real SRST of the boot drive, which must never run on a
 * production boot. Enable for a verification build with `make WEDGETEST=on`. */
#ifndef CONFIG_BMIDE_WEDGE_SELFTEST
#define CONFIG_BMIDE_WEDGE_SELFTEST 0
#endif

#define CONFIG_IRQ_DEFER_INITIAL_CAPACITY    16U
#define CONFIG_IRQ_DEFER_MAX_CHUNK_CAPACITY  1024U
#define CONFIG_IRQ_DEFER_GROWTH_FACTOR       2U   // each new chunk = prev × this, up to MAX_CHUNK_CAPACITY
#define CONFIG_IRQ_DEFER_PRODUCER_RETRIES    4    // bounded chunk-advance retries per irq_defer() call

/* Cross-core TLB shootdown ACK wait (vmm.c shootdown_wait_acks). The M1 fix is
 * in spin_lock(): a core spinning for an unrelated spinlock services shootdowns
 * inline, so the initiator never waits on a spinning target. These only bound
 * the genuine-deadlock detector.
 *   PANIC_MS         — ceiling before declaring a real cross-core deadlock.
 *                      Huge vs the us-scale IPI round-trip; a trip is a fault.
 *   SPIN_BACKSTOP    — spin-count guard for a frozen TSC (then the TSC ceiling
 *                      never grows); a broken clock still can't wedge the loop.
 *   TSC_FALLBACK_MHZ — assumed core frequency before TSC calibration completes. */
#define CONFIG_TLB_SHOOTDOWN_PANIC_MS         5000U
#define CONFIG_TLB_SHOOTDOWN_SPIN_BACKSTOP    2000000000ULL
#define CONFIG_TLB_SHOOTDOWN_TSC_FALLBACK_MHZ 1000ULL

/* Static MPSC ring of slots used by TouchPublishIrqPair to hand off a
 * Touch event from IRQ context to a K-Core for the actual publish work.
 * Power-of-2. Capacity sized to hold a worst-case IRQ burst from a fast
 * key-repeat + USB hot-plug + ACPI GPE storm without dropping events;
 * actual drop is silent (circular overwrite) by design. */
#define CONFIG_TOUCH_IRQ_RING_SIZE           64U

/* GEOMETRY-UNKNOWN FALLBACK bound for synchronous REACT-delivery nesting on one
 * core's kernel stack (touch_react_deliver → ManifestExecute → op publishes →
 * touch_react_deliver …). The PRIMARY guard is real per-core stack headroom
 * (CONFIG_TOUCH_REACT_STACK_MARGIN below); this depth count only applies when
 * the per-core stack geometry isn't recorded yet (early boot / boot stack).
 * Beyond this the delivery is dropped (graceful, never panic) to prevent
 * unbounded kernel-stack recursion / triple-fault. 16 KiB stack
 * (CONFIG_KERNEL_STACK_PAGES=4) ÷ ≈2.5 KiB per level ⇒ 4 keeps ~3 KiB margin. */
#define CONFIG_TOUCH_REACT_DEPTH_MAX 4U
/* Min kernel-stack headroom (bytes) that must remain BELOW the current RSP for
 * a REACT delivery to proceed. Must exceed the max stack growth between two
 * consecutive touch_react_deliver checks — one full level (ManifestExecute +
 * deepest publishing op ~2.2 KiB [SysAddrWake] + TouchPublishId ~1.7 KiB incl.
 * inlined deliver_one + TouchSnap[64]) ≈ 4 KiB, plus a nested IRQ/#exception
 * buffer ≈ 2 KiB (K-cores run REACT with IF=1) ⇒ ~6 KiB + slack. */
#define CONFIG_TOUCH_REACT_STACK_MARGIN 8192U
/* The depth-count fallback (used only when per-core stack geometry isn't yet
 * recorded) must fit even the smallest REACT-capable kernel stack: depth levels
 * × ~2.5 KiB per level ≤ the usable data area. */
_Static_assert(CONFIG_KERNEL_STACK_PAGES * CONFIG_PAGE_SIZE >=
               CONFIG_TOUCH_REACT_DEPTH_MAX * 2560,
               "REACT depth-count fallback must fit the smallest REACT-capable kernel stack");

#define CONFIG_PHYS_ZONE_DMA32_END 0x40000000ULL  // 1GB — DMA32 safe boundary
#define CONFIG_PHYS_ZONE_USER_END  0x100000000ULL // 4GB — identity map limit

#define CONFIG_AHCI_DRIVER 1
#define CONFIG_AHCI_MAX_RETRIES 3
#define CONFIG_AHCI_MAX_COMRESET_ATTEMPTS 3
#define CONFIG_AHCI_CMD_TIMEOUT_MS 2000
/* Ф26 M1 — async-completion watchdog deadline. A safety BACKSTOP, not the
 * delivery path: the MSI edge is the normal completion signal and Tier-1
 * lost-edge reconcile recovers a merely-lost interrupt every PIT tick, so this
 * bound is reached ONLY by a genuinely non-completing command. 30 s matches the
 * industry-standard disk command deadline (Linux SD_TIMEOUT) — generous enough
 * never to false-positive on a drive in multi-second internal error recovery,
 * bounded so a wedge recovers instead of hanging a waiter forever. */
#define CONFIG_AHCI_IO_TIMEOUT_MS 30000

/* AHCI boot-time self-test: a one-sector READ probe issued in ahci_init.
 * Useful on QEMU but on real HW it stalls boot if the device is slow or
 * the LBA happens to be unreadable. Disabled in production. */
#ifndef CONFIG_AHCI_SELFTEST
#define CONFIG_AHCI_SELFTEST 0
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
/* Unpark a parked core from scheduler_recalc as soon as it holds ANY
 * runnable task (was 2 — which let a single enqueued strand languish on a
 * parked core until the enqueue-IPI; 1 is the defense-in-depth complement
 * to the directed reschedule IPI in sched_enqueue). */
#define CONFIG_SCHED_UNPARK_LOAD_THRESH 1

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
#define CONFIG_STRAND_REAP_BATCH      16    /* P5b: reap at most N exited strands per tick */
#define CONFIG_PROCESS_POISON_MAGIC   0xDEADDEADu

/* Software typematic timing. Override at build with -DCONFIG_KB_REPEAT_*.
 *
 * Defaults are deliberately conservative — typing a key for under one
 * second should NEVER produce more than one character. Past values
 * (500 ms delay / 33 ms rate ≈ Windows-style aggressive) caused
 * complaints on slower-emulated targets (Bochs ips=50M) where a
 * sub-second hold yielded 5-10 chars.
 *
 *   CONFIG_KB_REPEAT_DELAY_MS  initial delay before repeat starts
 *   CONFIG_KB_REPEAT_RATE_MS   interval between repeats while held
 *
 * Both are wall-clock ms (converted to ticks at runtime against the
 * actual timer frequency, so they stay correct even after periodic
 * TSC recalibration changes timing). */
#ifndef CONFIG_KB_REPEAT_DELAY_MS
#define CONFIG_KB_REPEAT_DELAY_MS  500   /* Windows-class initial delay */
#endif
#ifndef CONFIG_KB_REPEAT_RATE_MS
#define CONFIG_KB_REPEAT_RATE_MS    33   /* ~30 chars/sec while held */
#endif

/* ACPI 6.5 §5.2.5.3: when RSDP revision >= 2 and XsdtAddress != 0 the OS
 * MUST use XSDT (32-bit RSDT may be stale on those firmwares). Selection
 * is runtime, never a compile-time switch. CONFIG_ACPI_FALLBACK_QEMU was
 * removed: emulator-targeted defaults are not part of the spec. */
#define CONFIG_ACPI_DEBUG 0

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

#define CONFIG_PERF_TRACE 0 // 1=in-memory ring buffer trace (fast: ~15 cycles, no serial I/O)

#define CONFIG_CACHE_LINE_SIZE 64 // x86-64 standard

#ifndef CONFIG_START_USERSPACE
#define CONFIG_START_USERSPACE 1
#endif

// AMP Configuration
#define CONFIG_MAX_CORES 256

_Static_assert((CONFIG_USER_STACK_SIZE % CONFIG_PAGE_SIZE) == 0,
               "CONFIG_USER_STACK_SIZE must be page-aligned");
_Static_assert(CONFIG_USER_STACK_SIZE >= 4096,
               "CONFIG_USER_STACK_SIZE must be at least 4KB");
_Static_assert((CONFIG_USER_HEAP_INITIAL_SIZE % CONFIG_PAGE_SIZE) == 0,
               "CONFIG_USER_HEAP_INITIAL_SIZE must be page-aligned");
_Static_assert(CONFIG_USER_HEAP_MAX_SIZE >= CONFIG_USER_HEAP_INITIAL_SIZE,
               "CONFIG_USER_HEAP_MAX_SIZE must be >= initial size");

#endif // KERNEL_CONFIG_H
