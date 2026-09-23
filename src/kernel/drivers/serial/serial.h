#ifndef SERIAL_H
#define SERIAL_H

#include "ktypes.h"

/* The address an IBM PC/AT kept COM1 at, in 1984. It is a convention, not a
 * property of any machine built since: on a board with no Super-I/O behind
 * LPC/eSPI nothing decodes it at all. It stands here as the one address the
 * kernel can try before any ACPI table exists, and it is treated as a guess
 * everywhere it is used — the machine's own answer lives in DBG2/DBGP and is
 * read by WireFollowTheFirmware below. COM2/3/4 were defined here too and
 * were never used by anything: three more numbers with no mechanism behind
 * them, now gone. */
#define SERIAL_PORT_COM1  0x3F8

/* Try the legacy address and learn two facts about whatever is there:
 * whether a UART answers at all (the modem loopback), and how many bytes its
 * transmitter takes when it says it is empty. Nothing is written to the line
 * here. A port that does not answer is said out loud, as a guess that did not
 * come off rather than as a fact about the machine. */
void serial_init(void);

/* The other half of "where is the console", asked of the machine instead of
 * assumed. serial_init runs before a single ACPI table is mapped, so it
 * cannot read DBG2; this reads DBG2, DBGP and SPCR and prints what each of
 * them says about this machine.
 *
 * It MOVES the line only when there is nothing to lose: when the legacy
 * address held no UART and the firmware names a 16550 this kernel can drive
 * — another I/O address, or a memory-mapped one with a wider register stride
 * — it opens that port, moves the Wire onto it and sends what the log ring
 * still holds, so the boot is readable from its first line. A port that
 * answered the modem loopback in serial_init is left exactly where it is: it
 * is a UART this machine really presents, it is carrying the boot already,
 * and its interrupt line is known from the same convention as its address.
 *
 * Whatever the outcome, the port ends up carrying its own interrupt — the
 * PC/AT line for the PC/AT address, what SPCR stated for a firmware-named
 * one, or nothing — and serial_console_init arms what is there.
 *
 * Call once, AFTER acpi_init and BEFORE serial_console_init. */
void WireFollowTheFirmware(void);

/* Wire — the serial line as the log ring's reader (see serial.c).
 *
 * WireKick: the ring grew. If the line is quiet, start it; if it is busy, the
 * transmitter's own interrupt will reach the new bytes. Called by LogRingPut
 * for every byte, from any core and any context, so it must be — and is —
 * one fenced load on the busy path. Before the line drives itself (before
 * serial_console_init) a kick drains the ring synchronously, on the THRE
 * fact, so early boot and an early panic are heard in full. */
void WireKick(void);

/* Everything said so far, onto the wire, before going on: panic, the
 * exception halt, reboot and shutdown. Waits on the transmitter's own word
 * (THRE), never on a clock; returns at once when there is no UART. */
void WireDrain(void);

/* For a panic on a core that may have died holding them: the ring's lock
 * and the line's. Never returns anything to a state a live core relies on
 * — the machine is stopping. */
void WireForceRelease(void);

/* Let the line drive itself: arm the interrupt the open port carries, for
 * the transmitter-empty and received-data causes. The receive half feeds
 * bytes into the keyboard input ring (keyboard_inject), so a host-side
 * serial console drives the shell as typed keys do.
 *
 * The line comes from the port — ISA IRQ 4 for the PC/AT address, or what
 * SPCR stated for a port the firmware named — and a port for which nothing
 * states one is left on the synchronous path with that said out loud, rather
 * than armed on a guess. The console is not treated as self-driving until an
 * interrupt has actually been served with this UART's own status register
 * naming the cause, so a line that turns out to be somebody else's costs
 * speed and never silence.
 *
 * Call once, AFTER keyboard_init. */
void serial_console_init(void);

#endif // SERIAL_H
