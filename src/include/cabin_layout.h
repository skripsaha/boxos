#ifndef CABIN_LAYOUT_H
#define CABIN_LAYOUT_H

#include "boxos_limits.h"

/*
 * Fixed virtual address layout for every Cabin (per-process address space).
 *
 * Phase 11 redesign:
 *   - PocketRing/ResultRing each split into two regions: a 1-page header at the
 *     traditional low address (kept for ABI continuity) and a separately-mapped
 *     slot region at a high virtual address.
 *   - The slot region is *reserved* virtual but *lazily mapped* phys: the page
 *     fault handler allocates and maps slot pages on first touch (PocketRing
 *     producer is userspace; vmm_ensure_user_page handles ResultRing).
 *   - Indices in the header are monotonic 64-bit. Slot lookup uses modulo
 *     against slot_count_max — once a virtual slot page is mapped, it is reused
 *     forever via wrap-around.
 *   - Maximum physical use per ring is bounded by (slot_count_max * slot_size)
 *     rounded up to whole pages — i.e. the reservation. Idle processes only
 *     pay for the header pages.
 */

/* x86_64 canonical user-VA upper bound. Bit 47 must be 0 for low-half
 * canonical addresses; the first non-canonical address is 0x8000_0000_0000.
 * Every Cabin-side bounds check (Bay window, heap-prefault VA, slot
 * reservations) lives strictly below this value. */
#define CABIN_USER_VA_CANONICAL_END    0x0000800000000000ULL

#define CABIN_NULL_TRAP_START          0x0000000000000000ULL
#define CABIN_NULL_TRAP_END            0x0000000000000FFFULL
#define CABIN_NULL_TRAP_SIZE           4096

/* CabinInfo: read-only metadata */
#define CABIN_INFO_ADDR                0x0000000000001000ULL
#define CABIN_INFO_SIZE                0x1000
#define CABIN_INFO_PAGES               1

/* PocketRing header (head/tail/slots_base/slot_size/slot_count_max). */
#define CABIN_POCKET_RING_ADDR         0x0000000000002000ULL
#define CABIN_POCKET_RING_SIZE         0x1000
#define CABIN_POCKET_RING_PAGES        1

/* ResultRing header (same layout as PocketRing header). */
#define CABIN_RESULT_RING_ADDR         0x0000000000003000ULL
#define CABIN_RESULT_RING_SIZE         0x1000
#define CABIN_RESULT_RING_PAGES        1

/* TouchRing header — kernel→userspace channel dedicated to Touch events
 * (tag-multicast). Lives in the 0x5000-0xC000 gap that previously held the
 * legacy fixed-size ResultRing; one fixed page, slot region is high-VA
 * (CABIN_TOUCH_SLOTS_BASE) and lazily mapped.
 *
 * Why a separate ring (vs multiplexing through ResultRing):
 *   - Touch is a distinct semantic channel from syscall replies / IPC
 *     payloads. Multiplexing forces fan-out logic in consumers
 *     (boxlib result_pop_* split by KCTX) and two stash rings.
 *   - Touch payloads carry inline data; tying their lifetime to a
 *     dedicated ring's Vyukov seq generation eliminates the
 *     `buf_heap_next` unbounded growth that the multiplexed design
 *     required (see project memory `ipc_touchring_decision_2026_06_01`).
 *   - Per-channel optimization: separate cacheline budgets, separate
 *     wake gating (a Touch-only consumer can UMWAIT on TouchRing.tail
 *     without polling the syscall-reply ring).
 */
#define CABIN_TOUCH_RING_ADDR          0x0000000000005000ULL
#define CABIN_TOUCH_RING_SIZE          0x1000
#define CABIN_TOUCH_RING_PAGES         1

/* ClockBoard — read-only kernel-published wall/uptime clock.
 *
 * One physical page allocated at boot, mapped read-only into EVERY Cabin
 * at this fixed VA. The PIT IRQ (BSP only) writes uptime_us / uptime_ms /
 * tick_count per tick; static fields (tsc_freq_khz, boot_unix_secs) are
 * populated once during early boot. Userspace reads via plain pointer
 * load — no syscall, no manifest, no ring round-trip.
 *
 * Replaces `time_uptime_ms` (a 46 µs HW-deck call) for the hot uptime
 * read with a ~10 ns memory load. The dynamic-PIT-freq architecture is
 * preserved: ClockBoard inherits monotonicity from pit_uptime_us which
 * already adds 1_000_000/current_freq µs per tick. */
#define CABIN_CLOCKBOARD_ADDR          0x0000000000004000ULL
#define CABIN_CLOCKBOARD_SIZE          0x1000
#define CABIN_CLOCKBOARD_PAGES         1

/* CpuCaps — read-only kernel-published CPU capability snapshot.
 *
 * One physical page, mapped read-only into every Cabin at this fixed
 * high-low VA. Boxlib's cpu.h reads it directly (cpu_has_waitpkg,
 * cpu_get_tsc_freq_khz) so the hot WAITPKG/TSC paths spend zero syscalls.
 *
 * Placed near the top of the 32-bit canonical user-VA window (well
 * clear of the heap and buf-heap regions, well below the 2 TiB ASLR
 * span used for slot reservations). This used to be hardcoded inside
 * box/cpu.h; lifting it here keeps cabin layout owned in one place. */
#define CABIN_CPU_CAPS_ADDR            0x000000007FFFF000ULL
#define CABIN_CPU_CAPS_SIZE            0x1000
#define CABIN_CPU_CAPS_PAGES           1

/* Code starts after the ring headers + clockboard. The 0x5000-0xC000 gap
 * (formerly part of the legacy fixed-size ResultRing) is left unmapped — it
 * is recoverable in a later phase but for now we keep CABIN_CODE_START_ADDR
 * at the same value so existing user ELFs (linked at 0xC000) load unchanged. */
#define CABIN_CODE_START_ADDR          0x000000000000C000ULL

/* High-VA slot regions. 64 TiB into user space — well clear of the heap and
 * buffer-heap regions below, AND well below the 128 TiB canonical-address
 * boundary (bit 47). Crossing 0x0000_8000_0000_0000 produces non-canonical
 * addresses, which fault with #GP regardless of the page tables. Reserved
 * virtually; physical pages are allocated lazily on first touch. */
#define CABIN_POCKET_SLOTS_BASE        0x0000400000000000ULL
#define CABIN_POCKET_SLOTS_PAGES       256                                /* 1 MiB */
#define CABIN_POCKET_SLOTS_SIZE        (CABIN_POCKET_SLOTS_PAGES * 0x1000ULL)
#define CABIN_POCKET_SLOTS_END         (CABIN_POCKET_SLOTS_BASE + CABIN_POCKET_SLOTS_SIZE)

#define CABIN_RESULT_SLOTS_BASE        0x0000400000100000ULL
#define CABIN_RESULT_SLOTS_PAGES       256                                /* 1 MiB */
#define CABIN_RESULT_SLOTS_SIZE        (CABIN_RESULT_SLOTS_PAGES * 0x1000ULL)
#define CABIN_RESULT_SLOTS_END         (CABIN_RESULT_SLOTS_BASE + CABIN_RESULT_SLOTS_SIZE)

/* TouchRing slots — kernel→userspace Touch event payload. Lives just above
 * the ResultRing slot reservation. 1 MiB reserved virtually, lazily mapped.
 * Indexed by `pos % TOUCH_RING_SLOT_MAX` and protected by a per-slot Vyukov
 * generation counter (see TouchSlot.seq in touch_ring.h). */
#define CABIN_TOUCH_SLOTS_BASE         0x0000400000200000ULL
#define CABIN_TOUCH_SLOTS_PAGES        256                                /* 1 MiB */
#define CABIN_TOUCH_SLOTS_SIZE         (CABIN_TOUCH_SLOTS_PAGES * 0x1000ULL)
#define CABIN_TOUCH_SLOTS_END          (CABIN_TOUCH_SLOTS_BASE + CABIN_TOUCH_SLOTS_SIZE)

/* Slot strides. POCKET_SLOT_SIZE matches sizeof(Pocket) — 64 bytes after the
 * Phase-12 envelope shrink, which also doubles PocketRing capacity within the
 * same 1 MiB reservation. RESULT_SLOT_SIZE is rounded up from sizeof(Result)=24
 * to the next power of two so a slot never straddles a page boundary.
 * TOUCH_SLOT_SIZE is 128 bytes — enough to inline 96 bytes of Touch payload
 * alongside the 24-byte metadata header and the 8-byte seq counter, with 128
 * dividing 4 KiB evenly for clean page alignment. */
#define POCKET_SLOT_SIZE               64
#define RESULT_SLOT_SIZE               32
#define TOUCH_SLOT_SIZE                128

/* Maximum inline Touch payload — fits within TOUCH_SLOT_SIZE alongside the
 * Touch metadata header and the Vyukov seq counter:
 *
 *   TouchSlot = [hdr: 24 B] + [payload: 96 B] + [seq: 8 B] = 128 B
 *
 * Existing in-kernel publishers fit comfortably: keyboard event 3 B, USB
 * port event 8 B, ACPI sts 2 B, ATA error 24 B. Larger payloads must use
 * Pocket IPC (Touch is a tag-multicast event channel, not a bulk-data path). */
#define BOXOS_TOUCH_PAYLOAD_MAX        96u

/* Capacity computed from reservation / stride. */
#define POCKET_RING_SLOT_MAX           (CABIN_POCKET_SLOTS_SIZE / POCKET_SLOT_SIZE)  /* 16384 */
#define RESULT_RING_SLOT_MAX           (CABIN_RESULT_SLOTS_SIZE / RESULT_SLOT_SIZE)  /* 32768 */
#define TOUCH_RING_SLOT_MAX            (CABIN_TOUCH_SLOTS_SIZE / TOUCH_SLOT_SIZE)    /* 8192 */

/* Ring header magic comes from boxos_magic.h — POCKET_RING_MAGIC / RESULT_RING_MAGIC. */

/* Nominal base addresses (actual per-process addresses are ASLR randomized). */
#define CABIN_HEAP_BASE                0x0000000010000000ULL
#define CABIN_BUF_HEAP_START           0x0000000040000000ULL

#define CABIN_HEAP_MAX_SIZE            BOXOS_USER_HEAP_MAX_SIZE

/* Bay window — per-cabin VA range for cross-cabin shared memory mapped
 * via bay_open. Anchored well above the slot reservations (which top out
 * at CABIN_TOUCH_SLOTS_END = 0x4000_0030_0000) and well below the bit-47
 * canonical-address ceiling (0x8000_0000_0000). The 32 TiB window holds
 * the largest realistic dataset budget on a single host without
 * colliding with any other Cabin region.
 *
 * The kernel bay subsystem bump-allocates per-cabin VA inside this
 * window; physical backing comes from PMM in 4 KB or 2 MB chunks via
 * vmm_map_page / vmm_map_huge_2m. Aligned to 2 MB so 2 MB mappings
 * always land on a PDE boundary. */
#define CABIN_BAY_BASE                 0x0000400100000000ULL
#define CABIN_BAY_END                  0x0000600000000000ULL
#define CABIN_BAY_SIZE                 (CABIN_BAY_END - CABIN_BAY_BASE)

/* Brook window — per-cabin VA range for SPSC streaming channels mapped
 * via brook_open. Anchored above the Bay window (CABIN_BAY_END) and
 * below the bit-47 canonical-address ceiling (0x8000_0000_0000), leaving
 * 31 TiB which is more than enough for any realistic concurrent-channel
 * count (8192-frame × 64 KiB × thousands of brooks).
 *
 * Each Brook open consumes:
 *   - 1 header page (4 KiB, BrookHeader: head/tail/peer-state/wake-flags)
 *   - frame_size × frame_count slot region, rounded up to page granularity
 *     (or 2 MiB if total ≥ 2 MiB — same implicit huge-page policy as Bay).
 *
 * The kernel brook subsystem bump-allocates per-cabin VA inside this
 * window via proc->brook_va_next. Aligned to 2 MiB so 2 MiB mappings
 * always land on a PDE boundary. Header page lives at the bottom of the
 * per-claim window, slot region immediately after. */
#define CABIN_BROOK_BASE               0x0000600000000000ULL
#define CABIN_BROOK_END                0x00007F0000000000ULL
#define CABIN_BROOK_SIZE               (CABIN_BROOK_END - CABIN_BROOK_BASE)

#define USER_CODE_ENTRY_POINT          CABIN_CODE_START_ADDR

#endif /* CABIN_LAYOUT_H */
