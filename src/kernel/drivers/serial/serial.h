#ifndef SERIAL_H
#define SERIAL_H

#include "ktypes.h"

#define SERIAL_PORT_COM1  0x3F8
#define SERIAL_PORT_COM2  0x2F8
#define SERIAL_PORT_COM3  0x3E8
#define SERIAL_PORT_COM4  0x2E8

#define SERIAL_COM1       SERIAL_PORT_COM1

void serial_init(void);
void serial_putchar(char c);
void serial_print(const char* str);
/* Emit `len` bytes to COM1 with the global serial lock held across the
 * whole run, so a kprintf from another core cannot splice itself into
 * the middle. `\n` is expanded to `\r\n`. Used by VGA put-string mirror. */
void serial_write(const char *bytes, size_t len);

#endif // SERIAL_H
