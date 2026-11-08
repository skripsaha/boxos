/* klib_logring.h — the log ring: what the kernel said, kept where it can be
 * asked for afterwards.
 *
 * A machine on a desk has a serial cable and `build/serial.log`. A machine on
 * a bench does not: its whole account of itself is whatever is still on the
 * screen, and the lines that matter most — the earliest ones — are exactly the
 * lines that scrolled away. Photographing the screen recovers a frame, not a
 * chronology.
 *
 * So every byte kprintf says is kept here, in kernel memory: `logsave`
 * writes it to the volume through Current, and the serial line reads it —
 * the Wire (drivers/serial/serial.c) is this ring's reader, driven by the
 * UART's own interrupt, so no core ever waits on a UART. Three reasons this
 * is a ring in memory and not a file written as it goes, all three measured
 * rather than assumed:
 *
 *   - kprintf is called from interrupt context, and a disk is not something an
 *     interrupt handler may touch;
 *   - a plain boot writes ZERO sectors of the volume (f83c7bb, re-measured:
 *     0 of 122326), and a logger writing as it goes destroys that property;
 *   - the failures worth catching are the ones where the MEDIUM stops
 *     answering, and a log written to that medium is missing exactly the
 *     moment it exists for.
 *
 * Always on: the ring is the wire's source, and a kernel without it would
 * have no serial line. `make PRINTTOFILE=on` adds the carry-over window below
 * — the copy that survives a reset.
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
 *   - userspace output is in it too, in the order it reached the screen: the
 *     display program says what it draws into the ring (hw.log.say), and a
 *     program with no display says its text there itself. A log with the
 *     kernel's answers and none of the commands is one somebody has to guess
 *     at.
 *
 * And it is the only thing on the glass. kprintf draws nothing: the display
 * program draws the ring's lines in their order beside everything else, and
 * while no program holds the screen the kernel's Emergency Screen draws its
 * tail (emergency_screen.c). Each byte carries a Tone beside it
 * (boxos_log_tone.h) — what kind of text it is, never its colour.
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
#include "boxos_log_tone.h"

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

/* One byte and its tone, from any core, from any context including an
 * interrupt. Kicks the wire once the byte is in. */
void LogRingPut(char c, uint8_t tone);

/* A whole run of bytes in one tone, under one hold of the ring's lock, so
 * nothing else lands inside it — and the end of an utterance (LogRingSaid).
 * *out_start and *out_end are the run's positions, [start, end): the display
 * program says what it draws and then knows which part of the ring was its
 * own. */
void LogRingSay(const char *bytes, uint64_t n, uint8_t tone,
                uint64_t *out_start, uint64_t *out_end);

/* The end of an utterance: one kprintf, one run said by a program. The bytes
 * before it are a whole thing to show, though a line may not have ended —
 * a prompt, an echoed key and "\b \b" never end one. Marks the bell due if
 * one is hung; ringing it is LogRingBellKnock's. */
void LogRingSaid(void);

/* For the wire and the Emergency Screen, lock-free: the position just past
 * the last byte said, and the byte and its tone at a position. A byte is
 * stable for as long as it is held — a reader that stays within
 * LOGRING_CAPACITY of `written`, and checks again after reading, sees what was
 * said. */
uint64_t LogRingWritten(void);
char     LogRingByteAt(uint64_t pos);
uint8_t  LogRingToneAt(uint64_t pos);

/* ── The bell — how the screen's holder hears the ring without looking ──
 *
 * The display program draws the ring's lines in their order and sleeps when
 * it has drawn them all. Before it sleeps it hangs a bell with the position
 * it has read to; the end of the next utterance past that position rings it
 * once — the same record system.bell pushes (no sender, ERR_WOULD_BLOCK),
 * which moves the Result cursor its sleep is watching. One bell: the ring
 * rings the screen's holder and nobody else (the op checks who is asking).
 *
 * LogRingBellHang answers true — and hangs nothing — when an utterance past
 * `from` is already there: the caller has news and must not sleep. That is
 * what closes the gap between its last read and its hang.
 *
 * ‼ THE KNOCK IS NEVER MADE UNDER THE CONSOLE'S OR THE RING'S LOCK. BatonPass
 * can print — before BatonInit, and on an ICR the LAPIC would not take — and a
 * print under g_kprintf_lock is a core spinning on its own lock with
 * interrupts off. So an utterance only marks the bell due, under the ring's
 * lock; LogRingBellKnock knocks, called once the locks are down — at the end
 * of kvprintf, at the end of hw.log.say, and from the BSP's tick for the
 * utterances that could not knock: an NMI's or a machine check's line (the
 * xAPIC ICR pair is guarded only by cli, which neither respects). A panic's
 * lines knock nobody: every core but its owner stands, the holder's with
 * them, and the account is drawn by the kernel (EmergencyScreenPanicTake). A
 * bell is hung only by a running program, so none is due before BatonInit.
 *
 * The bell remembers the generation of the strand that hung it and rings that
 * incarnation or nobody; it is hung only under the screen's own lock
 * (ScreenBellHang), so it never outlives the hold that hung it. */
bool LogRingBellHang(uint32_t pid, uint32_t generation, uint64_t from);
void LogRingBellDrop(void);
void LogRingBellKnock(void);

/* For a panic on a core that may have died holding the ring's lock. */
void LogRingForceRelease(void);

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

/* The same copy with the tone of every byte copied, into `tones` (as many as
 * the bytes). */
uint64_t LogRingReadTones(uint64_t from, void *dst, uint8_t *tones, uint64_t max,
                          uint64_t *out_oldest, uint64_t *out_written);

/* Write the carry-over window back out of cache, NOW.
 *
 * Callable in every build and from every path: without CONFIG_PRINTTOFILE
 * both are defined and do nothing, so a reset path may ask without knowing
 * whether this image has a window. Takes no lock, allocates nothing and waits
 * on nothing, because its callers are a panic and a one-way reset ladder —
 * a core may already be frozen holding any lock this kernel owns.
 *
 * A warm reset invalidates the caches without writing them back (Intel SDM
 * Vol 3A §11.5.1, Table 11-7 "Invalidating Caches and TLBs"), so a window
 * written only through the ordinary write-back path loses exactly the tail
 * that matters. LogKeepWriteBackName() says which instruction this part got
 * (CLWB, CLFLUSHOPT, CLFLUSH or WBINVD), so a board log states how its own
 * evidence was preserved. */
void        LogKeepFlush(void);
const char *LogKeepWriteBackName(void);

#ifdef CONFIG_PRINTTOFILE

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

static inline void LogKeepInit(void)     { }
static inline bool LogKeepWindow(uintptr_t *p, uint64_t *b)
{ (void)p; (void)b; return false; }
static inline uint64_t LogKeepPreviousBytes(void) { return 0; }
static inline uint32_t LogKeepPreviousBoot(void)  { return 0; }
static inline uint64_t LogKeepPreviousRead(uint64_t from, void *dst,
                                           uint64_t max, uint64_t *o,
                                           uint64_t *w)
{ (void)from; (void)dst; (void)max; if (o) *o = 0; if (w) *w = 0; return 0; }

#endif /* CONFIG_PRINTTOFILE */

#endif /* KLIB_LOGRING_H */
