#ifndef SERIAL_H
#define SERIAL_H

#include "ktypes.h"

#define SERIAL_PORT_COM1  0x3F8
#define SERIAL_PORT_COM2  0x2F8
#define SERIAL_PORT_COM3  0x3E8
#define SERIAL_PORT_COM4  0x2E8

#define SERIAL_COM1       SERIAL_PORT_COM1

/* Bring COM1 up and learn two facts about it: whether a UART answers at all
 * (the loopback), and how many bytes its transmitter takes when it says it
 * is empty. Nothing is written to the line here. */
void serial_init(void);

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

/* Let the line drive itself: wire IRQ4 for the transmitter-empty and
 * received-data interrupts. The receive half feeds bytes into the keyboard
 * input ring (keyboard_inject), so a host-side serial console drives the
 * shell as typed keys do. Call once, AFTER keyboard_init. */
void serial_console_init(void);

#endif // SERIAL_H
