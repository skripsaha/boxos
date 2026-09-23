/* serial.c — COM1, and the Wire: the serial line as the log ring's reader.
 *
 * Everything the kernel says goes into the log ring (klib_logring.h): one
 * store under one lock, from any core, from any context. The Wire is the
 * line's reader — a cursor into that ring, advanced by the UART itself. The
 * transmitter empties, the 16550 raises THRE, IRQ4 pumps the next FIFO-full
 * of bytes from the cursor. No core waits for a UART, ever. What stood here
 * was a spin of up to four million iterations per byte, interrupts off, that
 * then dropped the byte in silence.
 *
 * Producer and pump meet by the Dekker rule, not by a lock held across the
 * put: the pump, finding the ring drained, marks the line idle BEFORE it
 * looks once more; the producer, having put its byte, looks at idle AFTER.
 * With a full fence on each side at least one of them sees the other, so a
 * byte put while the line went quiet is either seen by the pump's last look
 * or kicks the line itself.
 *
 * Before IRQ4 is wired (serial_console_init, late in boot) the line cannot
 * drive itself; a kick then drains the ring synchronously, on the THRE fact,
 * so the early boot lines arrive as they are said and an early panic is
 * heard in full. After that the only synchronous path is WireDrain — the
 * deliberate one, at panic and at halt.
 *
 * The ring holds what was said; the line carries "\r\n" for "\n". A line the
 * ring has lapped — the cursor fell a whole ring behind — carries a count of
 * the bytes it never got, so the gap is a number on the wire and not a seam
 * nobody can see.
 *
 * There is no wire when nothing answers at init — no UART in the slot, or
 * one that does not close the modem loopback: then the ring keeps everything,
 * nothing waits, and the reason is said once on whatever console there is.
 *
 * And "at init" is only the first of two looks. serial_init runs before any
 * ACPI table is mapped, so the address it tries can only be the PC/AT one;
 * WireFollowTheFirmware runs after acpi_init and asks the machine itself,
 * through DBG2, DBGP and SPCR, where its debug port and its console are.
 * When the legacy address turned out to hold nothing and the firmware names
 * a port that answers — another I/O address, or a memory-mapped one whose
 * registers sit four bytes apart — the line moves there and carries the
 * whole boot again. The port is therefore a variable in this file, never a
 * constant in an instruction.
 *
 * ‼ THAT MOVE IS A REPAIR, NOT A PREFERENCE. A line that answered the modem
 * loopback is a UART this machine really presents and is already carrying
 * this boot; moving off it on the strength of a table can only lose, and did
 * — the port the firmware named was opened, the console followed it, and
 * every later look that asked "is this the PC/AT port" then answered no.
 * So the firmware is asked every boot and believed every boot, and acted on
 * only where there is nothing to lose.
 *
 * ‼ AND A PORT CARRIES ITS OWN INTERRUPT. Where the line is and how it asks
 * for attention are one fact about one device: the same interrupt empties
 * the transmitter and says a key was pressed, so a port adopted without its
 * line can be written to and never typed on. WirePort therefore holds both,
 * and a move replaces both at once.
 */
#include "serial.h"
#include "io.h"
#include "klib.h"
#include "klib_logring.h"
#include "keyboard.h"   /* keyboard_inject — RX bytes -> "keyboard" Touch events */
#include "idt.h"        /* irq_register_handler */
#include "irqchip.h"    /* irqchip_enable_irq */
#include "vmm.h"        /* vmm_map_mmio — a firmware-named port may be MMIO */
#include "acpi_dbg2.h"  /* AcpiDebugPortRead — where this machine says it is */

#define SERIAL_DATA          0
#define SERIAL_INT_ENABLE    1
#define SERIAL_INT_ID        2   /* read: IIR  */
#define SERIAL_FIFO_CTRL     2   /* write: FCR */
#define SERIAL_LINE_CTRL     3
#define SERIAL_MODEM_CTRL    4
#define SERIAL_LINE_STATUS   5
#define SERIAL_MODEM_STATUS  6

#define IER_ERBFI            0x01   /* received data available   */
#define IER_ETBEI            0x02   /* transmitter holding empty */
#define LSR_DATA_READY       0x01
#define LSR_THRE             0x20
#define IIR_NONE_PENDING     0x01
#define IIR_ID_MASK          0x0E
#define IIR_THRE             0x02
#define IIR_RX               0x04
#define IIR_RX_TIMEOUT       0x0C
#define IIR_LINE_STATUS      0x06
#define IIR_FIFO_ENABLED     0xC0   /* bits 7:6 = 11 — a 16550A with its FIFO on */

/* PC16550D §8.8 Modem Control Register, §8.9 Modem Status Register. */
#define MCR_DTR              0x01
#define MCR_RTS              0x02
#define MCR_OUT1             0x04
#define MCR_OUT2             0x08
#define MCR_LOOP             0x10
#define MCR_RUNNING          (MCR_DTR | MCR_RTS | MCR_OUT1 | MCR_OUT2)
#define MSR_CTS              0x10
#define MSR_DSR              0x20
#define MSR_RI               0x40
#define MSR_DCD              0x80
#define MSR_STATUS_BITS      (MSR_CTS | MSR_DSR | MSR_RI | MSR_DCD)

/* ‼ THE LOOPBACK IS ASKED TWICE, FOR TWO OPPOSITE ANSWERS.
 *
 * §8.8 bit 4: in loopback the four modem control INPUTS are disconnected
 * from the outside world and the four control OUTPUTS are wired to them
 * instead — DTR to DSR, RTS to CTS, OUT1 to RI, OUT2 to DCD. So the part can
 * be told to produce one pattern and then its complement, and only
 * something that really routes one set of bits into the other can give
 * both. Asking once, as this file did, is a test one value in sixteen
 * passes by accident — which is not a margin to move a console on. */
#define MCR_LOOP_DCD_CTS     (MCR_LOOP | MCR_OUT2 | MCR_RTS)
#define MSR_LOOP_DCD_CTS     (MSR_DCD | MSR_CTS)
#define MCR_LOOP_DSR_RI      (MCR_LOOP | MCR_OUT1 | MCR_DTR)
#define MSR_LOOP_DSR_RI      (MSR_DSR | MSR_RI)

/* PC16550D §8.3, the Interrupt Identification Register — the two things it
 * says about itself no matter what state the part is in.
 *
 * Bit 4 reads 0 in every mode the part has. Bit 5 is deliberately not asked
 * about: the original part holds it at 0 too, but the 16750 and its
 * descendants use it to say their 64-byte FIFO is on, and a UART that
 * arrives from firmware with that FIFO enabled is still a UART.
 *
 * Bits 7:6 are the pair that reports the FIFOs, and of their four values
 * three belong to real parts — 00 no FIFO, 11 a working one, and 10 the
 * original 16550 whose FIFO does not work, which Linux still carries as
 * PORT_16550. Only 01 belongs to nothing that was ever built, which is why
 * it is the one combination refused here rather than "anything but 11". */
#define IIR_RESERVED_ZERO    0x10
#define IIR_FIFO_IMPOSSIBLE  0x40

/* The other half of the 1984 convention. The PC/AT put COM1 at 0x3F8 AND on
 * ISA interrupt request line 4, and the two halves are one statement: a
 * machine that decodes 0x3F8 as a 16550 raises IRQ4 from it, and a machine
 * that does not decode 0x3F8 says nothing about IRQ4 either. It is an ISA
 * number, not a global one — the interrupt controller applies the firmware's
 * own remap to it, which is why it is handed over as a number and not as a
 * pin. */
#define SERIAL_COM1_ISA_IRQ  4

/* ‼ WHERE THE LINE IS, IS NOT SOMETHING THIS KERNEL KNOWS.
 *
 * A 16550 is eight byte-wide registers (PC16550D §8). Whether those eight
 * sit behind an x86 I/O port or at a physical address, one byte apart or
 * four, is a property of the board and not of this code. 0x3F8 is the PC/AT
 * convention of 1984; on a board with no Super-I/O behind LPC/eSPI nothing
 * decodes it at all, which is exactly what the first line of the boot log
 * said on real hardware — LSR reads 0xFF, the bus answering an address no
 * device claims.
 *
 * So the port is a variable, and it is filled twice. Once in serial_init,
 * with the legacy guess, because the log ring needs a reader before a
 * single ACPI table exists. Once more in WireFollowTheFirmware, after ACPI
 * is up, with what DBG2 or DBGP say — the machine's own answer. */
typedef struct {
    bool              mmio;      /* false: x86 port I/O. true: SystemMemory */
    uint16_t          io_base;   /* when !mmio                              */
    volatile uint8_t *mem_base;  /* when mmio                               */
    uint8_t           stride;    /* bytes between consecutive registers     */
    uint8_t           access;    /* width of one register access: 1, 2 or 4 */

    /* How this port asks for attention, and where that was stated. It
     * travels with the address because it is a fact about the same device:
     * see the note at the head of this file. */
    DebugPortLine     line;
    uint32_t          line_number;
} WirePort;

static WirePort g_port = {
    .mmio = false, .io_base = SERIAL_PORT_COM1, .mem_base = NULL,
    .stride = 1, .access = 1,
    /* The convention states the line as surely as it states the address, and
     * this kernel is entitled to exactly as much of it as the port turns out
     * to be worth: if 0x3F8 answers, IRQ4 is its line; if the port moves,
     * both halves go with it. */
    .line = DebugPortLineIsaIrq, .line_number = SERIAL_COM1_ISA_IRQ,
};

/* COM1 is one byte-wide port: one lock, one writer at a time. Held by the
 * pump — from IRQ4, from a kick, from a drain — and by nothing else. */
static spinlock_t g_serial_lock;

static bool     g_wire_present;   /* the UART answered the loopback              */
static uint32_t g_wire_fifo;      /* bytes the transmitter takes when empty       */
static uint64_t g_wire_pos;       /* the ring position the line has reached       */
static bool     g_wire_lf_owed;   /* '\r' went out; its '\n' is still owed        */
static uint64_t g_wire_behind;    /* bytes the line never carried: ring lapped it */

/* 1: the transmitter is empty and nobody is pumping. The Dekker flag. */
static volatile uint32_t g_wire_idle;

/* ‼ THE LINE DRIVES ITSELF ONLY ONCE ITS OWN INTERRUPT HAS ARRIVED.
 *
 * Not "this kernel armed an interrupt", which is something this kernel did,
 * but "an interrupt off this UART was served", which is something the
 * machine did. The difference is the whole safety of moving the console:
 * the port the firmware named comes with the line a table stated, and a
 * table can be wrong about a board in a way no amount of reading will
 * reveal. While this is false every kick drains the ring synchronously, on
 * the transmitter's own word — so a line that turns out to be somebody
 * else's costs speed and never silence, and no interval anywhere has to
 * decide when to give up waiting for it. The UART's own IIR is what flips
 * it, because that register naming a pending cause is the only statement
 * anywhere that the interrupt just served belonged to this port. */
static volatile bool g_wire_live;

/* What the line says of itself — the count of bytes it fell behind — is not
 * in the ring: it goes out from here, ahead of the ring's bytes. */
static char     g_wire_notice[80];
static uint32_t g_wire_notice_len;
static uint32_t g_wire_notice_pos;

/* One 16550 register, wherever the port turned out to be. The register is
 * eight bits wide in every one of these parts; a wider bus access carries
 * it in the low byte and leaves the rest reserved, which is why the wide
 * cases truncate on the way in and zero-extend on the way out. */
static inline volatile void *uart_at(uint8_t reg)
{
    return (volatile void *)(g_port.mem_base + (uint32_t)reg * g_port.stride);
}

static inline uint8_t uart_in(uint8_t reg)
{
    if (!g_port.mmio)
        return inb((uint16_t)(g_port.io_base + (uint16_t)reg * g_port.stride));

    volatile void *p = uart_at(reg);
    switch (g_port.access) {
    case 4:  return (uint8_t)*(volatile uint32_t *)p;
    case 2:  return (uint8_t)*(volatile uint16_t *)p;
    default: return *(volatile uint8_t *)p;
    }
}

static inline void uart_out(uint8_t reg, uint8_t val)
{
    if (!g_port.mmio) {
        outb((uint16_t)(g_port.io_base + (uint16_t)reg * g_port.stride), val);
        return;
    }

    volatile void *p = uart_at(reg);
    switch (g_port.access) {
    case 4:  *(volatile uint32_t *)p = val;           break;
    case 2:  *(volatile uint16_t *)p = (uint16_t)val; break;
    default: *(volatile uint8_t  *)p = val;           break;
    }
}

static inline bool transmitter_empty(void)
{
    return (uart_in(SERIAL_LINE_STATUS) & LSR_THRE) != 0;
}

static inline void put_wire(char c)
{
    uart_out(SERIAL_DATA, (uint8_t)c);
}

typedef enum {
    WireOpenOk = 0,
    WireOpenNothingDecodes,   /* every register floats high: no decoder there */
    WireOpenNotA16550,        /* something answers, but it is not a UART      */
} WireOpenResult;

/* ‼ NOTHING IS PROGRAMMED UNTIL A 16550 HAS ANSWERED FROM THE ADDRESS.
 *
 * What stood here read LSR once and then wrote five registers — IER, LCR,
 * FCR and MCR twice, plus the divisor — before it asked one question about
 * what was at the other end. On the legacy address that is a bet on a 1984
 * convention and costs nothing but itself; on an address out of DBG2, DBGP
 * or SPCR it is five writes into a register window this kernel knows
 * nothing about beyond the fact that the firmware named it, and those
 * tables name parts — USB debug controllers, LPSS blocks — as readily as
 * they name UARTs. The question it finally asked was one byte wide, and one
 * value in sixteen passes it by accident, so a mapped page of ordinary
 * memory holding the right byte at offset six was enough to carry the whole
 * console into RAM and silence the machine.
 *
 * So the order is: identify, and only then program. Identification asks
 * everything it can without writing at all, and the one register it does
 * write is MCR with the loopback bit set — the state in which §8.8
 * disconnects the part from the outside world instead of driving anything
 * onto it — and it puts back the byte it found there when the answer is no.
 *
 * `why` comes back with the part's own answer in it, because "not a 16550"
 * is worth nothing to somebody in front of a board and the numbers are. */
static WireOpenResult wire_identify_port(char *why, size_t why_len)
{
    if (why_len) why[0] = '\0';

    /* An empty slot floats every register high. Linux calls the same read its
     * "LSR safety check" (8250_port.c, serial8250_do_startup: `serial_port_in
     * (port, UART_LSR) == 0xff` -> -ENODEV) and it is the one check that
     * survives a board with no UART at all — there is nothing to program and
     * nothing to ask. */
    if (uart_in(SERIAL_LINE_STATUS) == 0xFF) return WireOpenNothingDecodes;

    /* The part's fixed bits, read with nothing written. IIR is not banked by
     * DLAB and needs no setting up to mean what it means, so this is
     * available before this kernel has touched anything at all — and a
     * question that can be asked without writing is asked first on
     * principle, because the answer decides whether anything may be
     * written here. */
    uint8_t iir = uart_in(SERIAL_INT_ID);
    if ((iir & IIR_RESERVED_ZERO) != 0 ||
        (iir & IIR_FIFO_ENABLED) == IIR_FIFO_IMPOSSIBLE) {
        ksnprintf(why, why_len,
                  "its Interrupt Identification Register reads 0x%02x, where "
                  "a 16550 holds bit 4 at zero and never reports its two "
                  "FIFO bits as 01",
                  (unsigned)iir);
        return WireOpenNotA16550;
    }

    /* ‼ AND THE LOOPBACK IS ASKED WITH THE MODEM LINES, NOT WITH A
     * CHARACTER — TWICE, FOR TWO OPPOSITE ANSWERS.
     *
     * What stood before that wrote 0xAE to the transmitter and read the
     * receiver buffer on the very next instruction. §8.8 bit 4 loops the
     * transmitter SHIFT register into the receiver shift register — the byte
     * is still serialised at the programmed rate, and §8.4 sets DR only
     * "whenever a COMPLETE incoming character has been received and
     * transferred into the Receiver Buffer Register". At divisor 1 that is
     * 86.8 us away; the read followed one I/O cycle later, so on real silicon
     * it returned whatever the FIFO reset left behind and the whole wire was
     * declared absent. Linux concedes the same character time in code —
     * size_fifo() loops back at divisor 1, writes its bytes and waits
     * mdelay(20) before it looks at UART_LSR_DR. Only QEMU and Bochs hid it,
     * both handing the loopback byte to the receiver inside the THR write.
     *
     * The modem loop costs no time at all, and asking it twice costs none
     * either. A window of memory answers the second question with the same
     * byte it answered the first, and the two required answers share no bit,
     * so it cannot give both. */
    uint8_t mcr_found = uart_in(SERIAL_MODEM_CTRL);

    uart_out(SERIAL_MODEM_CTRL, MCR_LOOP_DCD_CTS);
    uint8_t first  = (uint8_t)(uart_in(SERIAL_MODEM_STATUS) & MSR_STATUS_BITS);
    uart_out(SERIAL_MODEM_CTRL, MCR_LOOP_DSR_RI);
    uint8_t second = (uint8_t)(uart_in(SERIAL_MODEM_STATUS) & MSR_STATUS_BITS);

    if (first != MSR_LOOP_DCD_CTS || second != MSR_LOOP_DSR_RI) {
        /* Whatever this is, it is not this kernel's, and it goes back the
         * way it was found. */
        uart_out(SERIAL_MODEM_CTRL, mcr_found);
        ksnprintf(why, why_len,
                  "held in the modem loopback it answered MSR 0x%02x and then "
                  "0x%02x, where the control bits written before each read "
                  "require 0x%02x and 0x%02x",
                  (unsigned)first, (unsigned)second,
                  (unsigned)MSR_LOOP_DCD_CTS, (unsigned)MSR_LOOP_DSR_RI);
        return WireOpenNotA16550;
    }

    return WireOpenOk;
}

/* Now that a 16550 has said it is there, put it in the state this kernel
 * drives. Sets g_wire_fifo and touches nothing else; the caller owns
 * g_wire_present and the log line, because kprintf reaches back into this
 * file through WireKick and must never be called with g_serial_lock held.
 *
 * `set_baud` writes the divisor, and is true only for the legacy PC/AT port:
 * divisor 1 means 115200 because that port's 1.8432 MHz reference clock is
 * part of the same convention that puts it at 0x3F8. A port the firmware
 * named in DBG2 carries neither a baud rate nor a reference clock in that
 * table — DBG2 keeps whole subtypes apart (0x0011 and 0x0013) purely because
 * the clock differs — so the only divisor that can be right on such a port
 * is the one the firmware itself left in it. */
static void wire_program_port(bool set_baud)
{
    uart_out(SERIAL_INT_ENABLE, 0x00);
    if (set_baud) {
        uart_out(SERIAL_LINE_CTRL, 0x80);   /* DLAB on               */
        uart_out(SERIAL_DATA,      0x01);   /* divisor 1 = 115200    */
        uart_out(SERIAL_INT_ENABLE, 0x00);
    }
    uart_out(SERIAL_LINE_CTRL, 0x03);       /* 8N1, DLAB off         */
    uart_out(SERIAL_FIFO_CTRL, 0xC7);       /* FIFO on and cleared   */

    /* The last write is also what takes the part back out of the loopback
     * the identification left it in, so the line faces the outside world
     * again with DTR, RTS, OUT1 and OUT2 all asserted. */
    uart_out(SERIAL_MODEM_CTRL, MCR_RUNNING);

    /* How much the transmitter takes when it says it is empty: a 16550A with
     * its FIFO on answers IIR[7:6] = 11 and takes sixteen; anything else
     * takes one. Read from the part, not assumed. */
    uint8_t iir = uart_in(SERIAL_INT_ID);
    g_wire_fifo = ((iir & IIR_FIFO_ENABLED) == IIR_FIFO_ENABLED) ? 16u : 1u;
}

static WireOpenResult wire_open_port(bool set_baud, char *why, size_t why_len)
{
    WireOpenResult r = wire_identify_port(why, why_len);
    if (r != WireOpenOk) return r;
    wire_program_port(set_baud);
    return WireOpenOk;
}

void serial_init(void)
{
    spinlock_init(&g_serial_lock);
    g_wire_idle = 1u;

    char why[160];
    WireOpenResult r = wire_open_port(true, why, sizeof(why));
    g_wire_present = (r == WireOpenOk);
    if (g_wire_present) return;

    /* The address is a guess and has to be said as one. Nothing has been
     * read from this machine yet — ACPI is hundreds of lines away — so the
     * only thing behind 0x3F8 here is a convention from 1984. */
    if (r == WireOpenNothingDecodes) {
        kprintf("[SERIAL] nothing decodes 0x%x, where a PC/AT machine keeps COM1 "
                "(LSR reads 0xFF, which is the bus answering for a device that "
                "is not there). That address is this kernel's guess, not a fact "
                "about this machine: the firmware states where its own debug "
                "port is in DBG2, and this kernel reads that table once ACPI is "
                "up and moves the line there if it names somewhere else. Until "
                "then the log ring keeps everything and nothing waits for it.\n",
                (unsigned)SERIAL_PORT_COM1);
    } else {
        kprintf("[SERIAL] something answers at 0x%x but it is not a 16550: %s. "
                "That address is this kernel's guess, and whatever holds it "
                "here is another device — nothing has been programmed there. "
                "The log ring keeps everything; the firmware's own answer in "
                "DBG2 is read once ACPI is up.\n",
                (unsigned)SERIAL_PORT_COM1, why);
    }
}

/* ── the pump ─────────────────────────────────────────────────────────── */

static bool wire_pending(void)
{
    return g_wire_notice_pos < g_wire_notice_len ||
           g_wire_lf_owed ||
           g_wire_pos < LogRingWritten();
}

/* The ring lapped the cursor: move up to the oldest byte still held, and
 * stage the count for the wire. */
static void wire_mind_the_gap(void)
{
    uint64_t written = LogRingWritten();
    uint64_t oldest  = written > (uint64_t)LOGRING_CAPACITY
                     ? written - (uint64_t)LOGRING_CAPACITY : 0;
    if (g_wire_pos >= oldest) return;

    uint64_t gap = oldest - g_wire_pos;
    g_wire_behind  += gap;
    g_wire_pos      = oldest;
    g_wire_lf_owed  = false;

    /* One notice at a time: a line that keeps falling behind would otherwise
     * restart its own notice on every look and never finish saying it. The
     * count carried is the total, so the next notice says everything. */
    if (g_wire_notice_pos < g_wire_notice_len) return;
    g_wire_notice_len = (uint32_t)ksnprintf(g_wire_notice, sizeof(g_wire_notice),
                                            "\r\n[WIRE] %lu byte(s) fell behind "
                                            "(%lu since boot)\r\n",
                                            (unsigned long)gap,
                                            (unsigned long)g_wire_behind);
    g_wire_notice_pos = 0;
}

/* One byte onto the wire. False when nothing is owed. */
static bool wire_emit_one(void)
{
    if (g_wire_notice_pos < g_wire_notice_len) {
        put_wire(g_wire_notice[g_wire_notice_pos++]);
        return true;
    }
    if (g_wire_lf_owed) {
        put_wire('\n');
        g_wire_lf_owed = false;
        g_wire_pos++;
        return true;
    }
    if (g_wire_pos >= LogRingWritten()) return false;

    char c = LogRingByteAt(g_wire_pos);
    if (c == '\n') {
        put_wire('\r');            /* the '\n' follows, in the next slot */
        g_wire_lf_owed = true;
        return true;
    }
    put_wire(c);
    g_wire_pos++;
    return true;
}

/* As much as the transmitter takes, when it says it is empty. g_serial_lock
 * held. Returns with idle = 1 only when nothing is owed — and looks once
 * more after saying so (the Dekker rule, see the file head). Returns with
 * idle = 0 when the transmitter is full or still busy: THRE brings the line
 * back here by itself. */
static void wire_pump_locked(void)
{
    for (;;) {
        wire_mind_the_gap();
        if (!transmitter_empty()) return;

        uint32_t room = g_wire_fifo;
        while (room > 0 && wire_emit_one()) room--;
        if (room == 0) return;

        __atomic_store_n(&g_wire_idle, 1u, __ATOMIC_SEQ_CST);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (!wire_pending()) return;
        __atomic_store_n(&g_wire_idle, 0u, __ATOMIC_SEQ_CST);
    }
}

/* Everything owed, now, on the transmitter's own word. g_serial_lock held. */
static void wire_drain_locked(void)
{
    __atomic_store_n(&g_wire_idle, 0u, __ATOMIC_SEQ_CST);
    while (wire_pending()) {
        wire_pump_locked();
        while (wire_pending() && !transmitter_empty()) {
            asm volatile("pause");
        }
    }
}

void WireKick(void)
{
    if (!g_wire_present) return;

    if (!__atomic_load_n(&g_wire_live, __ATOMIC_SEQ_CST)) {
        spin_lock(&g_serial_lock);
        wire_drain_locked();
        spin_unlock(&g_serial_lock);
        return;
    }

    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (!__atomic_load_n(&g_wire_idle, __ATOMIC_SEQ_CST)) return;

    spin_lock(&g_serial_lock);
    __atomic_store_n(&g_wire_idle, 0u, __ATOMIC_SEQ_CST);
    wire_pump_locked();
    spin_unlock(&g_serial_lock);
}

void WireDrain(void)
{
    if (!g_wire_present) return;
    spin_lock(&g_serial_lock);
    wire_drain_locked();
    spin_unlock(&g_serial_lock);
}

void WireForceRelease(void)
{
    LogRingForceRelease();
    spin_force_release(&g_serial_lock);
}

/* ── the second look: the port the firmware itself named ──────────────── */

/* serial_init runs hundreds of lines before acpi_init, so the only address
 * it can possibly use is the legacy guess: at that moment not one ACPI
 * table has been mapped, and acpi_find_table would answer "no DBG2" for the
 * wrong reason. This is the other half of the question, and it belongs
 * after acpi_init and before serial_console_init — read DBG2, then DBGP,
 * then SPCR, and print what every one of them says about this machine.
 *
 * ‼ WHAT IS DONE WITH THE ANSWER DEPENDS ON WHETHER THERE IS ANYTHING TO
 * LOSE. A port that answered the modem loopback in serial_init is a UART
 * this machine really presents, it is carrying this boot already, and this
 * kernel knows its interrupt line by the same convention that gave it the
 * address. A table cannot improve on that, and acting on one cost exactly
 * what it looks like it would cost: the console was carried off a working
 * port onto whatever the firmware named, and every later question of the
 * form "is this still the PC/AT port" answered no. So the firmware is
 * believed about where its debug port is, and followed only when this
 * kernel has no working port of its own.
 *
 * What goes out afterwards is what the ring still HOLDS, which is not a
 * promise of everything ever said: wire_mind_the_gap raises the cursor to
 * the oldest byte the ring kept and puts the count of the rest on the wire
 * as a number.
 *
 * No interrupt is wired yet — serial_console_init comes later — so nothing
 * can enter the pump from an interrupt while the port underneath it
 * changes. The lock is still taken, because another core may be saying
 * something. */
void WireFollowTheFirmware(void)
{
    DebugPortDescription port;
    if (!AcpiDebugPortRead(&port)) return;   /* it said why, in its own voice */

    bool same = (port.space == DebugPortSpaceIo) && !g_port.mmio &&
                (port.address == (uint64_t)g_port.io_base) &&
                (port.stride  == g_port.stride);

    if (g_wire_present) {
        if (same) {
            kprintf("[SERIAL] %s names the port this kernel already opened "
                    "(I/O 0x%lx), so the line stays where it is\n",
                    port.source, (unsigned long)port.address);
        } else {
            kprintf("[SERIAL] %s names a console at %s 0x%lx, and the port this "
                    "kernel opened at 0x%x answered the modem loopback and is "
                    "carrying this boot. A working console is not given up for "
                    "a described one: the port here is a UART this machine "
                    "really presents, and this kernel knows which interrupt it "
                    "raises, both from the same convention. The firmware's port "
                    "is left alone%s%s\n",
                    port.source,
                    port.space == DebugPortSpaceIo ? "I/O port" : "physical address",
                    (unsigned long)port.address, (unsigned)SERIAL_PORT_COM1,
                    port.namepath[0] ? "; the firmware calls the device " : "",
                    port.namepath[0] ? port.namepath : "");
        }
        return;
    }

    if (same) {
        kprintf("[SERIAL] %s names the very port that did not answer "
                "(I/O 0x%lx) — the firmware describes a debug port this "
                "machine does not present, so there is nowhere to move the "
                "line to and the log ring goes on keeping everything\n",
                port.source, (unsigned long)port.address);
        return;
    }

    WirePort np;
    np.mmio        = (port.space == DebugPortSpaceMemory);
    np.io_base     = np.mmio ? 0u : (uint16_t)port.address;
    np.mem_base    = NULL;
    np.stride      = port.stride;
    np.access      = port.access;
    /* The line comes with the address or not at all. The PC/AT number the
     * old port carried belonged to the PC/AT port and is left behind with
     * it; what this port raises is whatever a table on this machine stated
     * for THIS address, which is DebugPortLineNotStated unless SPCR did. */
    np.line        = port.line;
    np.line_number = port.line_number;

    /* ‼ THE WINDOW IS BORROWED UNTIL THE PORT IS ACCEPTED, NOT GIVEN AWAY.
     * A mapping made for a candidate that then fails to identify itself used
     * to be left behind: one UC page leaked per boot, and — worse than the
     * page — a second, permanent kernel mapping of somebody else's register
     * BAR that nothing in this file would ever look at again. Both handles
     * live out here so that every path below can hand it back. */
    size_t         window = 0;
    volatile void *mapped = NULL;
    if (np.mmio) {
        /* Eight registers, `stride` apart: the whole of a 16550 and nothing
         * beyond it. vmm_map_mmio rounds up to a page and maps it UC, which
         * is the only cacheability a register window may have. */
        window = (size_t)port.stride * 8u;
        mapped = vmm_map_mmio((uintptr_t)port.address, window,
                              VMM_FLAGS_KERNEL_RW);
        if (!mapped) {
            kprintf("[SERIAL] %s names a console at physical 0x%lx and this "
                    "kernel could not map the %lu byte(s) it occupies, so the "
                    "line stays where it was\n",
                    port.source, (unsigned long)port.address,
                    (unsigned long)window);
            return;
        }
        np.mem_base = (volatile uint8_t *)mapped;
    }

    WirePort old      = g_port;
    uint32_t old_fifo = g_wire_fifo;
    char     why[160];

    spin_lock(&g_serial_lock);
    g_port = np;
    WireOpenResult r = wire_open_port(false, why, sizeof(why));
    if (r != WireOpenOk) {
        g_port      = old;
        g_wire_fifo = old_fifo;
        spin_unlock(&g_serial_lock);

        /* g_port already points back at the old line, so the window is no
         * longer reachable from anywhere and goes back to the address space
         * before anything is said about it. */
        if (mapped) vmm_unmap_mmio(mapped, window);

        if (r == WireOpenNothingDecodes) {
            kprintf("[SERIAL] %s names a console at %s 0x%lx, and nothing "
                    "decodes that address either (every register reads 0xFF). "
                    "The firmware describes a port this machine does not "
                    "present; the line stays where it was\n",
                    port.source,
                    np.mmio ? "physical address" : "I/O port",
                    (unsigned long)port.address);
        } else {
            kprintf("[SERIAL] %s names a console at %s 0x%lx, but what answers "
                    "there is not a 16550: %s. Nothing was programmed there "
                    "and the line stays where it was\n",
                    port.source,
                    np.mmio ? "physical address" : "I/O port",
                    (unsigned long)port.address, why);
        }
        return;
    }

    /* ‼ THE CURSOR IS NOT PUT BACK, BECAUSE IT NEVER MOVED.
     *
     * This is reached only while g_wire_present was false, and while it is
     * false WireKick and WireDrain both return before touching the pump —
     * nothing advances g_wire_pos or owes a '\n'. So the cursor already
     * stands at the first byte ever said, the new port carries the whole
     * boot from its beginning, and nothing is carried twice: putting it back
     * to zero here would have been a statement that something had been sent,
     * which would have been the one case where the same bytes went out on
     * two ports. wire_mind_the_gap raises it to the oldest byte the ring
     * still keeps and says the rest as a count. */
    g_wire_present = true;
    spin_unlock(&g_serial_lock);

    kprintf("[SERIAL] %s names the console at %s 0x%lx, the port this kernel "
            "guessed at 0x%x holds nothing, and a 16550 answers where the "
            "firmware said. The Wire moves onto it and pushes out everything "
            "the log ring still holds, so this boot is readable from its first "
            "line%s%s\n",
            port.source,
            np.mmio ? "physical address" : "I/O port",
            (unsigned long)port.address, (unsigned)SERIAL_PORT_COM1,
            port.namepath[0] ? "; the firmware calls the device " : "",
            port.namepath[0] ? port.namepath : "");

    /* And what came with it, said here rather than left to be discovered at
     * serial_console_init: the interrupt is what decides whether this console
     * can be typed on, and it is the half of the move that has no PC/AT
     * convention standing behind it. */
    if (g_port.line == DebugPortLineIsaIrq) {
        kprintf("[SERIAL] that port arrives with its interrupt as well: IRQ %u, "
                "stated for this very address, so the console keeps both halves "
                "and not only the one that can be written to\n",
                (unsigned)g_port.line_number);
    } else {
        kprintf("[SERIAL] and it arrives without an interrupt this kernel can "
                "arm: the PC/AT IRQ 4 belonged to the PC/AT address and stays "
                "with it, and no table on this machine states a line for this "
                "one that names its wiring. What that costs is said in full "
                "when the console is brought up\n");
    }
}

/* ── IRQ4: the line's own word, and what arrives on it ────────────────── */

static void serial_com1_irq(void)
{
    for (;;) {
        uint8_t iir = uart_in(SERIAL_INT_ID);
        if (iir & IIR_NONE_PENDING) return;

        /* This UART's own IIR names a pending cause, which is the only proof
         * anywhere that the line this kernel armed is the line this port
         * raises — and until that proof exists the transmitter is pumped by
         * hand. It is asked here, past the "nothing pending" exit, so that an
         * interrupt belonging to somebody else on a shared line cannot make
         * the claim on this port's behalf. */
        __atomic_store_n(&g_wire_live, true, __ATOMIC_SEQ_CST);

        switch (iir & IIR_ID_MASK) {
        case IIR_THRE:
            spin_lock(&g_serial_lock);
            __atomic_store_n(&g_wire_idle, 0u, __ATOMIC_SEQ_CST);
            wire_pump_locked();
            spin_unlock(&g_serial_lock);
            break;

        case IIR_RX:
        case IIR_RX_TIMEOUT:
            /* Drain the whole FIFO. CR->LF so a terminal's Enter submits the
             * line (the PS/2 path delivers '\n' for Enter, which is what the
             * line editor expects). */
            while (uart_in(SERIAL_LINE_STATUS) & LSR_DATA_READY) {
                char c = (char)uart_in(SERIAL_DATA);
                if (c == '\r') c = '\n';
                keyboard_inject(&c, 1);
            }
            break;

        case IIR_LINE_STATUS:
            (void)uart_in(SERIAL_LINE_STATUS);
            break;

        default:
            (void)uart_in(SERIAL_MODEM_STATUS);
            break;
        }
    }
}

/* ‼ WHICH LINE THIS PORT RAISES IS A QUESTION FOR THE MACHINE, AND THE
 * THREE WAYS OF ANSWERING IT ON THE MACHINE'S BEHALF WERE ALL REFUSED.
 *
 * DBG2 states no interrupt whatever — a port type, a subtype, addresses,
 * sizes and a namespace path, and not one interrupt field — and DBGP states
 * less. So there are four things that could be done with a port that moved
 * there, and only one of them is honest.
 *
 *   ARM IRQ 4 ANYWAY. The number is half of the PC/AT convention whose other
 *   half is the address 0x3F8, and this port is not at that address; on a
 *   board with no Super-I/O the line belongs to whatever else the chipset put
 *   there. It would hand this handler another device's interrupts, and leave
 *   the transmitter waiting on a THRE that never comes. Refused.
 *
 *   POLL THE RECEIVER ON A TICK. A poll needs a period, and a period is a
 *   number standing in for a mechanism; worse, this kernel puts the whole
 *   machine to sleep when nothing is owed, and a console poll is a wakeup
 *   source that never stops. It would trade a dead receiver for a machine
 *   that never idles. Refused.
 *
 *   ASK THE NAMESPACE. DBG2 does give the ACPI path of the device, and that
 *   object's _CRS states its interrupt. This kernel's interpreter evaluates
 *   _PRT and a PCI Interrupt Link's _CRS, and nothing here can reach a plain
 *   device's. Named because it is the answer, and not claimed because it is
 *   not built.
 *
 *   ASK SPCR, WHICH IS THE TABLE THAT STATES IT. The Serial Port Console
 *   Redirection table exists because the firmware was already using this
 *   port as a console, and it carries at offset 53 the PC-AT IRQ the UART
 *   raises. That is a statement about this address, written by this machine.
 *   Taken — see AcpiDebugPortRead.
 *
 * So the line is armed from a description or not at all, and a port that
 * arrives without one is said out loud along with everything it costs. And
 * whichever line is armed, the console does not start driving itself until
 * an interrupt off THIS UART has actually been served: see g_wire_live. */
void serial_console_init(void)
{
    if (!g_wire_present) return;

    if (g_port.line != DebugPortLineIsaIrq) {
        if (g_port.line == DebugPortLineGsi) {
            kprintf("[SERIAL] the console is the port the firmware named, and "
                    "the only line stated for it is global system interrupt %u "
                    "— a pin, with no polarity, no trigger and no bus rule "
                    "behind it. This kernel arms lines from descriptions and "
                    "that is not one, so it is not armed\n",
                    (unsigned)g_port.line_number);
        } else {
            kprintf("[SERIAL] the console is the port the firmware named, and "
                    "no table on this machine states which interrupt it raises "
                    "— DBG2 and DBGP carry no interrupt field at all, and this "
                    "firmware published no SPCR to say\n");
        }
        kprintf("[SERIAL] so this line is pushed out as each line is said, on "
                "the transmitter's own word, instead of driving itself: every "
                "byte still arrives and nothing is lost. What is lost is the "
                "receive half — nothing typed on this console reaches the "
                "shell, because a keystroke announces itself on the same "
                "interrupt and there is nowhere to wait for it\n");
        return;
    }

    /* 1-byte RX trigger so a single character interrupts immediately, and
     * the receive FIFO cleared of boot-time noise. The transmit FIFO is left
     * alone: it may hold bytes the synchronous kicks put there. */
    uart_out(SERIAL_FIFO_CTRL, 0x03);
    /* The Wire drains the log ring onto the line. Late means bytes fall
     * behind and the reader is told how many; nothing in flight is lost.
     *
     * The number handed over is an ISA interrupt request line, not a pin:
     * both of these apply the firmware's own remap of it, so a board whose
     * MADT moves this line lands on the pin the board says and not on the
     * one the number would suggest. */
    irq_register_handler((uint8_t)g_port.line_number, serial_com1_irq,
                         RIGHT_OF_WAY_A_PERSON_WAITS);
    irqchip_enable_irq((uint8_t)g_port.line_number);

    kprintf("[SERIAL] the console answers on IRQ %u, and it is believed to be "
            "this port's line only once an interrupt arrives on it with this "
            "UART's own status register naming the cause; until then every "
            "byte is still pushed out by hand\n",
            (unsigned)g_port.line_number);

    /* From here the line can drive itself: ETBEI raises the interrupt
     * whenever the transmitter empties — at once, if it is empty now — and
     * ERBFI when a byte arrives. MCR.OUT2 is already set by serial_init, so
     * the UART's INT line reaches the interrupt controller. */
    spin_lock(&g_serial_lock);
    uart_out(SERIAL_INT_ENABLE, IER_ERBFI | IER_ETBEI);
    /* Drained, not pumped, and under the same lock. A pump can leave bytes
     * owed against a THRE interrupt that has not been proved to arrive; this
     * leaves nothing owed at all, so the console is whole at this instant
     * whether or not the line turns out to be this port's. */
    wire_drain_locked();
    spin_unlock(&g_serial_lock);
}
