#include "serial.h"
#include "io.h"

#define SERIAL_DATA          0
#define SERIAL_INT_ENABLE    1
#define SERIAL_FIFO_CTRL     2
#define SERIAL_LINE_CTRL     3
#define SERIAL_MODEM_CTRL    4
#define SERIAL_LINE_STATUS   5
#define SERIAL_MODEM_STATUS  6
#define SERIAL_SCRATCH       7

void serial_init(void) {
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
    while (serial_transmit_empty() == 0);
    outb(SERIAL_PORT_COM1 + SERIAL_DATA, c);
}

void serial_print(const char* str) {
    while (*str) {
        if (*str == '\n') {
            serial_putchar('\r');    // \n -> \r\n for terminal compatibility
        }
        serial_putchar(*str);
        str++;
    }
}
