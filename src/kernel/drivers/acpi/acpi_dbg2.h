#ifndef ACPI_DBG2_H
#define ACPI_DBG2_H

#include "ktypes.h"

/* acpi_dbg2 — where this machine says its debug port is.
 *
 * 0x3F8 is a PC/AT convention from 1984. On a board with no Super-I/O
 * behind LPC/eSPI nothing decodes it at all, and the kernel that programs
 * it is talking to nobody. The machine's own answer lives in two tables:
 *
 *   DBG2  Microsoft "Debug Port Table 2", revision 0. Header field
 *         OffsetDbgDeviceInfo at byte 36 and NumberDbgDeviceInfo at 40
 *         point at a list of Debug Device Information structures, each
 *         carrying a port type, a subtype, one or more Generic Address
 *         Structures and the ACPI namespace path of the device.
 *   DBGP  the older "Debug Port Specification" table it replaces: one
 *         interface type and one Generic Address Structure.
 *   SPCR  Microsoft "Serial Port Console Redirection Table", version 1.10.
 *         The same base address, and the one thing neither of the other
 *         two carries: the interrupt the port raises.
 *
 * This reader parses all three, says out loud what each record describes,
 * and hands back the first record it can actually drive — a 16550-family
 * serial port whose register stride and access size the tables define.
 * Which address space those registers live in is the one question DBG2's
 * Generic Address Structure is forbidden to answer: that table's own note
 * says the Address Space ID field "must be 0", so a zero read there is the
 * value the specification demands of every conforming firmware and states
 * nothing about the machine. The SUBTYPE states it instead, in words, for
 * two of the four this kernel drives — 0x0000 "uses 'legacy' port I/O as
 * seen on x86-based platforms", 0x0001 "supports memory mapped UARTs" — and
 * where the subtype covers both kinds (0x0012, and the Intel LPSS part at
 * 0x0014) the address itself is what is left to decide, which it can: the
 * x86 I/O space is 64 KiB wide, and the bottom 64 KiB of physical memory on
 * this machine is the real-mode interrupt vector table, the BIOS data area
 * and conventional DRAM, never a device's register window. DBGP's and
 * SPCR's own Generic Address Structures are under no such rule — SPCR's
 * field table works COM1 out as Address Space ID 1, System I/O — so there
 * the field is a statement and is read as one.
 *
 * ‼ THAT RULE WAS WRITTEN DOWN HERE BEFORE IT WAS CARRIED OUT. The code
 * applied it to subtype 0x0000 alone; subtypes 0x0001, 0x0012 and 0x0014
 * were classified by the very Address Space ID the note forces to zero, so
 * each of them came out as SystemMemory. A firmware that described its
 * x86 port-I/O UART with 0x0012 — the subtype the specification recommends
 * for new platforms, and the one that "supports all serial ports which can
 * be described by the subtypes 0x0 and 0x1" — therefore handed this kernel
 * 0x3F8 as a physical address, to be mapped and written to as memory.
 *
 * Nothing here touches the port. Mapping it and speaking to it belongs to
 * the driver that owns the line (drivers/serial/serial.c).
 */

typedef enum {
    DebugPortSpaceIo     = 0,   /* an x86 I/O port number                   */
    DebugPortSpaceMemory = 1    /* a physical address, to be mapped as MMIO */
} DebugPortSpace;

/* ‼ HOW THE PORT ASKS FOR ATTENTION IS PART OF WHERE IT IS.
 *
 * A console the kernel can only push by hand is half a console: the same
 * interrupt that empties the transmitter is the one that says a key was
 * pressed, so a port opened without its line can be written to and can
 * never be typed on. And the line does not follow from the address — it
 * follows from the device, which is why it travels in this description
 * rather than being worked out again wherever a port happens to be opened.
 *
 * DBG2 states no interrupt at all: its Debug Device Information structure
 * carries a port type, a subtype, addresses, sizes and a namespace path,
 * and no interrupt field of any kind. DBGP carries even less. SPCR — which
 * exists because the firmware was ALREADY using this port as a console —
 * is the one table of the three that names the line, and it names it twice
 * over: a PC-AT dual-8259 IRQ number at offset 53 and a global system
 * interrupt at 54, either, both or neither. */
typedef enum {
    /* No table on this machine said. The port must be pushed by hand, and
     * the caller has to say so rather than arm a line it guessed. */
    DebugPortLineNotStated = 0,
    /* An 8259 IRQ number, as the ISA bus numbers them: the firmware's GSI
     * remap still has to be applied to it, which is what the interrupt
     * controller does with any ISA number it is handed. */
    DebugPortLineIsaIrq,
    /* A global system interrupt, and nothing at all about how that pin is
     * wired — SPCR states the number and no polarity, no trigger, no bus. */
    DebugPortLineGsi
} DebugPortLine;

typedef struct {
    DebugPortSpace space;
    uint64_t       address;     /* port number, or physical address        */
    uint8_t        stride;      /* bytes between consecutive registers     */
    uint8_t        access;      /* width of one register access: 1, 2 or 4 */
    uint16_t       subtype;     /* DBG2 Table 3 port subtype               */
    DebugPortLine  line;        /* which table field, if any, named a line */
    uint32_t       line_number; /* the IRQ or the GSI, read by `line`      */
    const char    *source;      /* "DBG2", "DBGP" or "SPCR"                */
    char           namepath[40];/* the ACPI object the firmware named      */
} DebugPortDescription;

/* True when the firmware named a serial port this kernel can drive, with
 * `out` filled in. False otherwise — including when no table exists. Says
 * what it found, or did not find, on the console either way. Reads the
 * ACPI table cache, so it must be called after acpi_init(). */
bool AcpiDebugPortRead(DebugPortDescription *out);

#endif /* ACPI_DBG2_H */
