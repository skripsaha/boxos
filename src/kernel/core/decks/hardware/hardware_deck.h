#ifndef HARDWARE_DECK_H
#define HARDWARE_DECK_H

#include "ktypes.h"
#include "boxos_decks.h"

#define HARDWARE_DECK_ID DECK_HARDWARE

/* Manifest opcodes (used by hardware_ops.c handlers + boxlib wrappers). */
#define HW_TIMER_GET_TICKS    0x10
#define HW_TIMER_GET_MS       0x11
#define HW_TIMER_GET_FREQ     0x12

#define HW_RTC_GET_TIME       0x15
#define HW_RTC_GET_UNIX64     0x16
#define HW_RTC_GET_UPTIME     0x17

/*
 * ‼ FOURTEEN NUMBERS ARE SPENT HERE, AND NONE OF THEM COMES BACK.
 *
 * 0x20-0x25 were hw.port.inb/outb/inw/outw/inl/outl, 0x30-0x36 were
 * hw.irq.register/unregister/enable/disable/isr/irr/eoi, and 0x40 was
 * hw.cpu.halt. Not one of them was an operation. The first thirteen were this
 * kernel's own `in`, `out` and interrupt-controller writes published outside
 * under a name, and the last was a bare `hlt` reached with interrupts already
 * masked — hardware_ops.c, above the Disk section, says what each of them
 * actually did and what it cost. Their numbers stay spent, so an old caller
 * that still knows one of them can only ever be told there is no such op,
 * never handed a different one. The same rule, and the same reason, as 0x90
 * and 0x93 below.
 */

#define HW_DISK_FLUSH         0x52
#define HW_DISK_INFO          0x53

/* The screen is lent to the one system program that draws (screen.h). */
#define HW_SCREEN_CLAIM         0x60
#define HW_SCREEN_RELEASE       0x61

/*
 * ‼ 0x70-0x7D ARE SPENT, AND NONE OF THEM COMES BACK.
 *
 * They were hw.vga.putchar, putstring, clear, clear_line, clear_eol,
 * cursor.get, cursor.set, color.get, color.set, scroll, newline, dims, paint
 * and step: fourteen ways for any program to draw through the kernel. The
 * kernel draws nothing now — the screen is the display program's (0x60) and
 * a program with no display says its text into the log ring (HW_LOG_SAY). The
 * numbers stay spent so an old caller is told there is no such op, never
 * handed a different one.
 */

#define HW_SYSTEM_REBOOT        0x80
#define HW_SYSTEM_SHUTDOWN      0x81
#define HW_DEBUG_PRINT          0x82
#define HW_LOG_READ             0x83
/* The account of the run BEFORE this one, carried through a warm reset in a
 * fixed physical window (klib_logring.h). Same wire shape as HW_LOG_READ so
 * one reader in userspace serves both. */
#define HW_LOG_PREVIOUS         0x84
/* A program's run of text into the log ring, and the bell the screen's holder
 * hangs on it (klib_logring.h). */
#define HW_LOG_SAY              0x85
#define HW_LOG_BELL             0x86

/* HW_LOG_READ's optional flags word. */
#define HW_LOG_READ_TONES       0x01u

/* The most one HW_LOG_SAY takes: a page. A longer run is several. */
#define HW_LOG_SAY_MAX          4096u

/*
 * ‼ A RETIRED OPCODE IS NEVER REISSUED.
 *
 * 0x90 was hw.usb.init and 0x93 was hw.usb.stop. Both are withdrawn: the first
 * re-ran the driver's boot and took the machine's USB down with it, the second
 * returned success for doing nothing (hardware_ops.c says what each of them
 * actually did). Their numbers stay spent, so an old caller that still knows
 * them can only ever be told there is no such op — never handed a different
 * one. The Boarding Pass keeps its stamp kinds the same way and for the same
 * reason: a number that meant one thing must not come back meaning another.
 */
#define HW_USB_RESET            0x91
#define HW_USB_START            0x92
#define HW_USB_PORT_STATUS      0x94
#define HW_USB_PORT_RESET       0x95
#define HW_USB_ENUM_DEVICE      0x96
#define HW_USB_GET_INFO         0x97
#define HW_USB_PORT_QUERY       0x98

error_t HardwareDeckRegister(void);

/*
 * Prove, on this machine, that asking the hardware deck to put a USB controller
 * back in service does it — and that the two withdrawn opcodes answer to
 * nothing. Runs once, on the first pass through the idle or guide loop with a
 * controller in service and a volume mounted; that is a fact about the machine
 * and not a moment waited for.
 *
 * It costs the machine every USB device it has, once, so it exists only in a
 * build made with USBRECOVER=on. Off, it is not a call at all.
 */
#if CONFIG_USB_RECOVER_PROOF
void HardwareDeckUsbRecoverProof(void);
#else
static inline void HardwareDeckUsbRecoverProof(void) { }
#endif

#endif /* HARDWARE_DECK_H */
