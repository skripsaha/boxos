#ifndef SERIAL_H
#define SERIAL_H

#include "ktypes.h"

#define SERIAL_PORT_COM1  0x3F8
#define SERIAL_PORT_COM2  0x2F8
#define SERIAL_PORT_COM3  0x3E8
#define SERIAL_PORT_COM4  0x2E8

#define SERIAL_COM1       SERIAL_PORT_COM1

void serial_init(void);

void WireKick(void);

void WireDrain(void);

void WireForceRelease(void);

void serial_console_init(void);

#endif