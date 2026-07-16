#include "serial.h"
#include "io.h"
#include "klib.h"
#include "keyboard.h"   /* keyboard_inject — RX bytes -> shell input ring */
#include "idt.h"        /* irq_register_handler */
#include "irqchip.h"    /* irqchip_enable_irq */

#define SERIAL_DATA          0
#define SERIAL_INT_ENABLE    1
#define SERIAL_FIFO_CTRL     2
#define SERIAL_LINE_CTRL     3
#define SERIAL_MODEM_CTRL    4
#define SERIAL_LINE_STATUS   5
#define SERIAL_MODEM_STATUS  6
#define SERIAL_SCRATCH       7

/* COM1 is a single-byte I/O port — writes from concurrent cores
 * interleave at byte granularity. Without serialisation a string
 * like "memtest" emitted from Core 5 racing kprintf's "[%s] ..."
 * frame from Core 2 produced the binary-garbage you'd see in
 * build/serial.log (`m`+attr-byte+`e`+...). One global IRQ-safe
 * spinlock per character keeps the output coherent — serial is
 * never on a hot path, so the cost is irrelevant. */
static spinlock_t g_serial_lock;

void serial_init(void) {
    spinlock_init(&g_serial_lock);

    outb(SERIAL_PORT_COM1 + SERIAL_INT_ENABLE, 0x00);

    // Enable DLAB, set divisor = 1 (115200 baud)
    outb(SERIAL_PORT_COM1 + SERIAL_LINE_CTRL, 0x80);
    outb(SERIAL_PORT_COM1 + SERIAL_DATA, 0x01);
    outb(SERIAL_PORT_COM1 + SERIAL_INT_ENABLE, 0x00);

    outb(SERIAL_PORT_COM1 + SERIAL_LINE_CTRL, 0x03);   // 8N1
    outb(SERIAL_PORT_COM1 + SERIAL_FIFO_CTRL, 0xC7);   // Enable FIFO, 14-byte threshold
    outb(SERIAL_PORT_COM1 + SERIAL_MODEM_CTRL, 0x0B);

    // Loopback test
    outb(SERIAL_PORT_COM1 + SERIAL_MODEM_CTRL, 0x1E);
    outb(SERIAL_PORT_COM1 + SERIAL_DATA, 0xAE);
    if (inb(SERIAL_PORT_COM1 + SERIAL_DATA) != 0xAE) {
        // Serial port failed, continue anyway
    }

    outb(SERIAL_PORT_COM1 + SERIAL_MODEM_CTRL, 0x0F);
}

static int serial_transmit_empty(void) {
    return inb(SERIAL_PORT_COM1 + SERIAL_LINE_STATUS) & 0x20;
}

void serial_putchar(char c) {
    /* Bounded busy-wait: at 115200 baud the FIFO drains in ~87us per byte;
     * cap at ~4M iterations (~1ms on a 4GHz CPU) so a missing/disconnected
     * COM1 cannot wedge the kernel forever. Drop the byte on overflow.
     *
     * IRQ-safe spinlock for SMP: COM1 is one byte-wide port — without the
     * lock, two cores executing this function intersect at the wait+outb
     * pair and produce interleaved nonsense on the wire (memtest letters
     * mixed with kprintf attribute bytes — the binary garbage seen in
     * serial.log during heavy multi-core output). */
    spin_lock(&g_serial_lock);
    for (uint32_t spins = 0; spins < 4000000u; spins++) {
        if (serial_transmit_empty()) {
            outb(SERIAL_PORT_COM1 + SERIAL_DATA, c);
            spin_unlock(&g_serial_lock);
            return;
        }
        asm volatile("pause");
    }
    spin_unlock(&g_serial_lock);
}

/* Internal: emit one byte assuming the lock is already held. */
static inline void serial_emit_locked(char c) {
    for (uint32_t spins = 0; spins < 4000000u; spins++) {
        if (serial_transmit_empty()) {
            outb(SERIAL_PORT_COM1 + SERIAL_DATA, c);
            return;
        }
        asm volatile("pause");
    }
}

void serial_print(const char* str) {
    /* Take the lock once per call so the whole string is atomic on the
     * wire (per-char locking would still allow interleaving between two
     * cores serial_print()'ing in parallel). */
    spin_lock(&g_serial_lock);
    while (*str) {
        char c = *str++;
        if (c == '\n') serial_emit_locked('\r');
        serial_emit_locked(c);
    }
    spin_unlock(&g_serial_lock);
}

void serial_write(const char *bytes, size_t len) {
    if (!bytes || len == 0) return;
    spin_lock(&g_serial_lock);
    for (size_t i = 0; i < len; i++) {
        char c = bytes[i];
        if (c == '\n') serial_emit_locked('\r');
        serial_emit_locked(c);
    }
    spin_unlock(&g_serial_lock);
}

/* ---------------------------------------------------------------------
 *  COM1 serial console — inbound (RX) path.
 *
 *  serial_init brings COM1 up TX-only with every UART interrupt masked. This
 *  adds the receive half: an IRQ4 handler drains the RX FIFO and feeds each
 *  byte into the keyboard input ring (keyboard_inject), so a host-side serial
 *  console drives the shell exactly as typed keys do — event-driven, no
 *  polling. Wired from main.c AFTER keyboard_init so the target ring exists.
 * ------------------------------------------------------------------ */
#define SERIAL_COM1_GSI  4   /* legacy ISA IRQ4 = COM1, identity-mapped GSI */

static void serial_com1_irq(void) {
    /* Only ERBFI (RX-data-available) is enabled, so inbound data is the sole
     * interrupt source and reading RBR clears it. Drain the whole FIFO. CR->LF
     * so a terminal's Enter submits the line (the PS/2 path delivers '\n' for
     * Enter, which is what keyboard_readline expects). */
    while (inb(SERIAL_PORT_COM1 + SERIAL_LINE_STATUS) & 0x01) {
        char c = (char)inb(SERIAL_PORT_COM1 + SERIAL_DATA);
        if (c == '\r') c = '\n';
        keyboard_inject(&c, 1);
    }
}

void serial_console_init(void) {
    /* 1-byte RX trigger so a single character interrupts immediately, and
     * clear any boot-time noise sitting in the FIFO. */
    outb(SERIAL_PORT_COM1 + SERIAL_FIFO_CTRL, 0x07);
    irq_register_handler(SERIAL_COM1_GSI, serial_com1_irq);
    irqchip_enable_irq(SERIAL_COM1_GSI);
    /* ERBFI — received-data-available interrupt. MCR.OUT2 is already set by
     * serial_init, so the UART's INT line reaches the IOAPIC. */
    outb(SERIAL_PORT_COM1 + SERIAL_INT_ENABLE, 0x01);
}
