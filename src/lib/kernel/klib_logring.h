/* klib_logring.h — the log ring: what the kernel said, kept where it can be
 * asked for afterwards.
 *
 * A machine on a desk has a serial cable and `build/serial.log`. A machine on
 * a bench does not: its whole account of itself is whatever is still on the
 * screen, and the lines that matter most — the earliest ones — are exactly the
 * lines that scrolled away. Photographing the screen recovers a frame, not a
 * chronology.
 *
 * So when the build is asked for it, every byte `kputchar()` emits is also
 * kept here, in kernel memory, and `logsave` writes it to the volume through
 * Current. Three reasons this is a ring in memory and not a file written as it
 * goes, all three measured rather than assumed:
 *
 *   - kprintf is called from interrupt context, and a disk is not something an
 *     interrupt handler may touch;
 *   - a plain boot writes ZERO sectors of the volume (f83c7bb, re-measured:
 *     0 of 122326), and a logger writing as it goes destroys that property;
 *   - the failures worth catching are the ones where the MEDIUM stops
 *     answering, and a log written to that medium is missing exactly the
 *     moment it exists for.
 *
 * Off by default. `make PRINTTOFILE=on` compiles it in; without it LogRingPut
 * is an empty inline and the kernel carries neither the buffer nor the branch.
 *
 * Where the account begins, and where it ends, said here so the file does not
 * promise more than it carries:
 *
 *   - it begins at the first byte the C kernel says. The single letters
 *     kernel_entry.asm puts on the serial port before that ("KSHMZ", measured)
 *     are written by assembly to a UART, at a moment when BSS has not been
 *     zeroed and there is no C world to keep anything in;
 *   - it holds what was SAID, not what a serial line carried: no '\r' is added
 *     here, because none was said;
 *   - userspace output is in it too. Its Manifest VGA ops feed the ring
 *     directly (hardware_ops.c), and deliberately not through the serial
 *     mirror, which a board build turns off. A log with the kernel's answers
 *     and none of the commands is one somebody has to guess at.
 *
 * ‼ debug_printf does NOT reach here — it compiles to nothing unless
 * DEBUG=on. A probe meant to be read on a board is written with kprintf.
 */
#ifndef KLIB_LOGRING_H
#define KLIB_LOGRING_H

/* The kernel has its own fixed-width types; <stdint.h> from the host toolchain
 * fights them (measured: conflicting typedefs for uint_fast32_t on the first
 * build of this file). */
#include "ktypes.h"

/* Is this kernel keeping the log? Answered by the kernel rather than guessed
 * from a build flag on the other side of the wall: userspace ASKS, so a
 * program built once behaves honestly against either kernel. */
bool LogRingIsKept(void);

#ifdef CONFIG_PRINTTOFILE

/* 1 MiB, a power of two so the position maps to a slot by mask. A boot in
 * QEMU is ~21 KB; a board with two controllers, thirty ports and a recovery
 * or two is a few hundred. The room left over is for the session that
 * follows, and when it does run out LogRingRead SAYS how much was lost. */
#define LOGRING_CAPACITY (1024u * 1024u)

/* Rides the hook mem_init() already drives for the console lock, so there is
 * no second init call to remember. The ring works before it runs: a BSS-zero
 * spinlock_t is an unlocked one, which is the same property kprintf's own lock
 * has depended on since it was written. */
void LogRingLockInit(void);

/* One byte, from any core, from any context including an interrupt. */
void LogRingPut(char c);

/*
 * Copy out at most `max` bytes starting at ring position `from`.
 *
 * Positions are counted from the first byte the kernel ever said and never
 * restart, so they name a byte for the life of the boot even after the ring
 * has lapped several times. A reader that has been overtaken is TOLD: the
 * copy starts at the oldest byte still held, and *out_oldest says where that
 * was, so the gap is a number the caller can print rather than a silence.
 *
 * Returns the number of bytes copied. *out_written is the position just past
 * the last byte the kernel has said so far — a reader that wants "the log as
 * of now" snapshots it on its first call and stops there, rather than chasing
 * a tail that its own printing keeps extending.
 */
uint64_t LogRingRead(uint64_t from, void *dst, uint64_t max,
                     uint64_t *out_oldest, uint64_t *out_written);

/* ==========================================================================
 * The carry-over: what the machine said LAST time, read after a reset.
 *
 * The ring above dies with the boot that filled it. That is the wrong
 * property for the failures worth catching, and it was measured the hard way:
 * a board wedged itself with a storming interrupt and an exhausted host
 * controller, `logsave` could not run because the volume was exactly what had
 * failed, and the entire account of it was four photographs of a scrolling
 * screen.
 *
 * So every byte the ring keeps is ALSO written into a fixed physical window
 * that this kernel does not clear at boot. Two banks: the boot that is running
 * writes one, the boot before it wrote the other, and neither can erase the
 * other's. On the next start the header says which was which.
 *
 * ‼ WHAT THIS SURVIVES, said plainly so nobody trusts it further than it goes:
 *   - a warm reset — the RESET button, a kernel-issued reboot, a triple fault.
 *     DRAM keeps its contents; the firmware's memory training on a warm start
 *     usually does not rewrite it.
 *   - it does NOT survive removing power. Holding the power button for four
 *     seconds is a hardware override that cuts the rails, and RAM goes with
 *     them. On a wedged machine, press RESET, not power.
 *   - it does NOT survive firmware that retrains and scrubs memory on every
 *     start. That is why the header carries a magic and a version, and why a
 *     window that does not check out is reported as absent rather than
 *     presented as a log.
 *
 * There is no checksum over the data on purpose. A machine that dies mid-write
 * leaves a torn tail, and a torn tail is worth reading; refusing the whole
 * account because its last line is half-written would throw away exactly the
 * evidence the crash exists to provide.
 * ========================================================================== */

/* Called as the first act of kernel_main, before anything is said, because a
 * byte said before it is a byte the next boot will not see. Validates the
 * window against the E820 map at its fixed low address (the kernel's own
 * memory map is not parsed yet, and a blind store into a machine with less RAM
 * than the window needs would be a store into nothing, or into MMIO). */
void LogKeepInit(void);

/* Where the window is, so pmm_init can hold it out of the allocator. Returns
 * false when there is no window — no ring in this build, or the machine could
 * not offer the memory. */
bool LogKeepWindow(uintptr_t *out_phys, uint64_t *out_bytes);

/* How many bytes the previous boot said, and which boot it was. Zero bytes
 * means there is nothing to read: a first start, a cold one, or firmware that
 * scrubbed the window. */
uint64_t LogKeepPreviousBytes(void);
uint32_t LogKeepPreviousBoot(void);

/* Same shape as LogRingRead, over the previous boot's bank. Positions count
 * from that boot's first byte; a reader that asks from before the oldest byte
 * still held is moved forward and told where to. */
uint64_t LogKeepPreviousRead(uint64_t from, void *dst, uint64_t max,
                             uint64_t *out_oldest, uint64_t *out_written);

#else

static inline void LogRingLockInit(void) { }
static inline void LogRingPut(char c)    { (void)c; }
static inline void LogKeepInit(void)     { }
static inline bool LogKeepWindow(uintptr_t *p, uint64_t *b)
{ (void)p; (void)b; return false; }
static inline uint64_t LogKeepPreviousBytes(void) { return 0; }
static inline uint32_t LogKeepPreviousBoot(void)  { return 0; }
static inline uint64_t LogKeepPreviousRead(uint64_t from, void *dst,
                                           uint64_t max, uint64_t *o,
                                           uint64_t *w)
{ (void)from; (void)dst; (void)max; if (o) *o = 0; if (w) *w = 0; return 0; }

/* Declared in both builds so the door that offers the log carries no #ifdef
 * of its own: it asks LogRingIsKept() and refuses, which is the same answer
 * userspace would get from a kernel that simply has no ring. */
static inline uint64_t LogRingRead(uint64_t from, void *dst, uint64_t max,
                                   uint64_t *out_oldest, uint64_t *out_written)
{
    (void)from; (void)dst; (void)max;
    if (out_oldest)  *out_oldest  = 0;
    if (out_written) *out_written = 0;
    return 0;
}

#endif /* CONFIG_PRINTTOFILE */

#endif /* KLIB_LOGRING_H */
