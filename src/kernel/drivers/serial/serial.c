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
 */
#include "serial.h"
#include "io.h"
#include "klib.h"
#include "klib_logring.h"
#include "keyboard.h"   /* keyboard_inject — RX bytes -> "keyboard" Touch events */
#include "idt.h"        /* irq_register_handler */
#include "irqchip.h"    /* irqchip_enable_irq */

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

#define SERIAL_COM1_GSI  4   /* legacy ISA IRQ4 = COM1, identity-mapped GSI */

/* COM1 is one byte-wide port: one lock, one writer at a time. Held by the
 * pump — from IRQ4, from a kick, from a drain — and by nothing else. */
static spinlock_t g_serial_lock;

static bool     g_wire_present;   /* the UART answered the loopback              */
static bool     g_wire_live;      /* IRQ4 wired: the line drives itself          */
static uint32_t g_wire_fifo;      /* bytes the transmitter takes when empty       */
static uint64_t g_wire_pos;       /* the ring position the line has reached       */
static bool     g_wire_lf_owed;   /* '\r' went out; its '\n' is still owed        */
static uint64_t g_wire_behind;    /* bytes the line never carried: ring lapped it */

/* 1: the transmitter is empty and nobody is pumping. The Dekker flag. */
static volatile uint32_t g_wire_idle;

/* What the line says of itself — the count of bytes it fell behind — is not
 * in the ring: it goes out from here, ahead of the ring's bytes. */
static char     g_wire_notice[80];
static uint32_t g_wire_notice_len;
static uint32_t g_wire_notice_pos;

static inline bool transmitter_empty(void)
{
    return (inb(SERIAL_PORT_COM1 + SERIAL_LINE_STATUS) & LSR_THRE) != 0;
}

static inline void put_wire(char c)
{
    outb(SERIAL_PORT_COM1 + SERIAL_DATA, (uint8_t)c);
}

void serial_init(void)
{
    spinlock_init(&g_serial_lock);
    g_wire_idle = 1u;

    /* An empty slot floats every register high. Linux calls the same read its
     * "LSR safety check" (8250_port.c, serial8250_do_startup: `serial_port_in
     * (port, UART_LSR) == 0xff` -> -ENODEV) and it is the one check that
     * survives a board with no UART at all — there is nothing to program and
     * nothing to ask. */
    if (inb(SERIAL_PORT_COM1 + SERIAL_LINE_STATUS) == 0xFF) {
        g_wire_present = false;
        kprintf("[SERIAL] COM1 at 0x%x: LSR reads 0xFF, no UART there. "
                "The log ring keeps everything; nothing goes out on the line "
                "and nothing waits for it.\n",
                (unsigned)SERIAL_PORT_COM1);
        return;
    }

    outb(SERIAL_PORT_COM1 + SERIAL_INT_ENABLE, 0x00);
    outb(SERIAL_PORT_COM1 + SERIAL_LINE_CTRL, 0x80);   /* DLAB on               */
    outb(SERIAL_PORT_COM1 + SERIAL_DATA, 0x01);        /* divisor 1 = 115200    */
    outb(SERIAL_PORT_COM1 + SERIAL_INT_ENABLE, 0x00);
    outb(SERIAL_PORT_COM1 + SERIAL_LINE_CTRL, 0x03);   /* 8N1, DLAB off         */
    outb(SERIAL_PORT_COM1 + SERIAL_FIFO_CTRL, 0xC7);   /* FIFO on and cleared   */
    outb(SERIAL_PORT_COM1 + SERIAL_MODEM_CTRL, 0x0B);

    /* ‼ THE LOOPBACK IS ASKED WITH THE MODEM LINES, NOT WITH A CHARACTER.
     *
     * What stood here wrote 0xAE to the transmitter and read the receiver
     * buffer on the very next instruction. §8.8 bit 4 loops the transmitter
     * SHIFT register into the receiver shift register — the byte is still
     * serialised at the programmed rate, and §8.4 sets DR only "whenever a
     * COMPLETE incoming character has been received and transferred into the
     * Receiver Buffer Register". At the divisor just written, 1, that is
     * 86.8 us away; the read followed one I/O cycle later, so on real silicon
     * it returned whatever the FIFO reset left behind and the whole wire was
     * declared absent. Linux concedes the same character time in code —
     * size_fifo() loops back at divisor 1, writes its bytes and waits
     * mdelay(20) before it looks at UART_LSR_DR. Only QEMU and Bochs hid it,
     * both handing the loopback byte to the receiver inside the THR write.
     *
     * The modem loop costs no time at all: in loopback the four modem control
     * outputs are tied to the four status inputs (§8.8 bit 4), and §8.9 names
     * the two this asks for — MCR.RTS reads back as MSR.CTS, MCR.OUT2 as
     * MSR.DCD, while DTR and OUT1, left clear, must read back as DSR and RI
     * clear. That is exactly Linux autoconfig()'s test, down to the compared
     * value. */
    outb(SERIAL_PORT_COM1 + SERIAL_MODEM_CTRL, MCR_LOOP | MCR_OUT2 | MCR_RTS);
    uint8_t msr = (uint8_t)(inb(SERIAL_PORT_COM1 + SERIAL_MODEM_STATUS) &
                            MSR_STATUS_BITS);
    outb(SERIAL_PORT_COM1 + SERIAL_MODEM_CTRL, MCR_RUNNING);
    g_wire_present = (msr == (MSR_DCD | MSR_CTS));

    if (!g_wire_present) {
        kprintf("[SERIAL] COM1 at 0x%x: the UART did not close the modem "
                "loopback (MSR 0x%02x, expected 0x%02x). The log ring keeps "
                "everything; nothing goes out on the line and nothing waits "
                "for it.\n",
                (unsigned)SERIAL_PORT_COM1, (unsigned)msr,
                (unsigned)(MSR_DCD | MSR_CTS));
        return;
    }

    /* How much the transmitter takes when it says it is empty: a 16550A with
     * its FIFO on answers IIR[7:6] = 11 and takes sixteen; anything else
     * takes one. Read from the part, not assumed. */
    uint8_t iir = inb(SERIAL_PORT_COM1 + SERIAL_INT_ID);
    g_wire_fifo = ((iir & IIR_FIFO_ENABLED) == IIR_FIFO_ENABLED) ? 16u : 1u;
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

    if (!g_wire_live) {
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

/* ── IRQ4: the line's own word, and what arrives on it ────────────────── */

static void serial_com1_irq(void)
{
    for (;;) {
        uint8_t iir = inb(SERIAL_PORT_COM1 + SERIAL_INT_ID);
        if (iir & IIR_NONE_PENDING) return;

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
            while (inb(SERIAL_PORT_COM1 + SERIAL_LINE_STATUS) & LSR_DATA_READY) {
                char c = (char)inb(SERIAL_PORT_COM1 + SERIAL_DATA);
                if (c == '\r') c = '\n';
                keyboard_inject(&c, 1);
            }
            break;

        case IIR_LINE_STATUS:
            (void)inb(SERIAL_PORT_COM1 + SERIAL_LINE_STATUS);
            break;

        default:
            (void)inb(SERIAL_PORT_COM1 + SERIAL_MODEM_STATUS);
            break;
        }
    }
}

void serial_console_init(void)
{
    if (!g_wire_present) return;

    /* 1-byte RX trigger so a single character interrupts immediately, and
     * the receive FIFO cleared of boot-time noise. The transmit FIFO is left
     * alone: it may hold bytes the synchronous kicks put there. */
    outb(SERIAL_PORT_COM1 + SERIAL_FIFO_CTRL, 0x03);
    irq_register_handler(SERIAL_COM1_GSI, serial_com1_irq);
    irqchip_enable_irq(SERIAL_COM1_GSI);

    /* From here the line drives itself: ETBEI raises IRQ4 whenever the
     * transmitter empties — at once, if it is empty now — and ERBFI when a
     * byte arrives. MCR.OUT2 is already set by serial_init, so the UART's
     * INT line reaches the IOAPIC. */
    spin_lock(&g_serial_lock);
    g_wire_live = true;
    outb(SERIAL_PORT_COM1 + SERIAL_INT_ENABLE, IER_ERBFI | IER_ETBEI);
    /* One pump under the same lock, so the line leaves here in a state its
     * own rules describe: idle and marked so, or busy and owed a THRE. The
     * synchronous drains never marked the line idle — they had no need to. */
    __atomic_store_n(&g_wire_idle, 0u, __ATOMIC_SEQ_CST);
    wire_pump_locked();
    spin_unlock(&g_serial_lock);
}
