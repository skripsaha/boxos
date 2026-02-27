#include "serial.h"
#include "io.h"

// Serial port registers (offsets from base port)
#define SERIAL_DATA          0  // Data register (read/write)
#define SERIAL_INT_ENABLE    1  // Interrupt enable
#define SERIAL_FIFO_CTRL     2  // FIFO control
#define SERIAL_LINE_CTRL     3  // Line control
#define SERIAL_MODEM_CTRL    4  // Modem control
#define SERIAL_LINE_STATUS   5  // Line status
#define SERIAL_MODEM_STATUS  6  // Modem status
#define SERIAL_SCRATCH       7  // Scratch register

void serial_init(void) {
    // Disable interrupts
    outb(SERIAL_PORT_COM1 + SERIAL_INT_ENABLE, 0x00);

    // Set baud rate divisor (115200 baud)
    // Enable DLAB (Divisor Latch Access Bit)
    outb(SERIAL_PORT_COM1 + SERIAL_LINE_CTRL, 0x80);
    // Set divisor to 1 (low byte) for 115200 baud
    outb(SERIAL_PORT_COM1 + SERIAL_DATA, 0x01);
    // Set divisor high byte
    outb(SERIAL_PORT_COM1 + SERIAL_INT_ENABLE, 0x00);

    // Disable DLAB, set 8 bits, no parity, 1 stop bit (8N1)
    outb(SERIAL_PORT_COM1 + SERIAL_LINE_CTRL, 0x03);

    // Enable FIFO, clear, 14-byte threshold
    outb(SERIAL_PORT_COM1 + SERIAL_FIFO_CTRL, 0xC7);

    // Enable IRQs, set RTS/DSR
    outb(SERIAL_PORT_COM1 + SERIAL_MODEM_CTRL, 0x0B);

    // Test serial port (loopback test)
    outb(SERIAL_PORT_COM1 + SERIAL_MODEM_CTRL, 0x1E);
    outb(SERIAL_PORT_COM1 + SERIAL_DATA, 0xAE);

    // Check if serial is working (loopback returns same byte)
    if (inb(SERIAL_PORT_COM1 + SERIAL_DATA) != 0xAE) {
        // Serial port failed, but continue anyway
    }

    // Set normal operation mode (not loopback)
    outb(SERIAL_PORT_COM1 + SERIAL_MODEM_CTRL, 0x0F);
}

static int serial_transmit_empty(void) {
    return inb(SERIAL_PORT_COM1 + SERIAL_LINE_STATUS) & 0x20;
}

void serial_putchar(char c) {
    // Wait for transmit buffer to be empty
    while (serial_transmit_empty() == 0);

    // Send character
    outb(SERIAL_PORT_COM1 + SERIAL_DATA, c);
}

void serial_print(const char* str) {
    while (*str) {
        // Convert \n to \r\n for proper terminal display
        if (*str == '\n') {
            serial_putchar('\r');
        }
        serial_putchar(*str);
        str++;
    }
}
