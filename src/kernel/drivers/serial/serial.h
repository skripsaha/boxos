#ifndef SERIAL_H
#define SERIAL_H

#include "ktypes.h"

// Serial port base addresses
#define SERIAL_PORT_COM1  0x3F8
#define SERIAL_PORT_COM2  0x2F8
#define SERIAL_PORT_COM3  0x3E8
#define SERIAL_PORT_COM4  0x2E8

// Legacy compatibility
#define SERIAL_COM1       SERIAL_PORT_COM1

void serial_init(void);
void serial_putchar(char c);
void serial_print(const char* str);

#endif // SERIAL_H
