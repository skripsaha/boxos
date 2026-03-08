#ifndef HARDWARE_DECK_H
#define HARDWARE_DECK_H

#include "pocket.h"
#include "boxos_decks.h"

#define HARDWARE_DECK_ID DECK_HARDWARE

#define HW_TIMER_GET_TICKS    0x10
#define HW_TIMER_GET_MS       0x11
#define HW_TIMER_GET_FREQ     0x12
#define HW_TIMER_SCHEDULE     0x13
#define HW_TIMER_CANCEL       0x14

#define HW_RTC_GET_TIME       0x15
#define HW_RTC_GET_UNIX64     0x16
#define HW_RTC_GET_UPTIME     0x17

#define HW_PORT_INB           0x20
#define HW_PORT_OUTB          0x21
#define HW_PORT_INW           0x22
#define HW_PORT_OUTW          0x23
#define HW_PORT_INL           0x24
#define HW_PORT_OUTL          0x25

#define HW_IRQ_ENABLE         0x32
#define HW_IRQ_DISABLE        0x33
#define HW_IRQ_GET_ISR        0x34
#define HW_IRQ_GET_IRR        0x35
#define HW_IRQ_REGISTER       0x30
#define HW_IRQ_UNREGISTER     0x31
#define HW_IRQ_SEND_EOI       0x36

#define HW_CPU_HALT           0x40

// Disk I/O Operations (0x50-0x5F)
#define HW_DISK_INFO          0x53
#define HW_DISK_FLUSH         0x52

// Keyboard Input Opcodes
#define HW_KEYBOARD_GETCHAR     0x60
#define HW_KEYBOARD_READLINE    0x61
#define HW_KEYBOARD_STATUS      0x62

// Keyboard Error Codes
#define HW_KB_SUCCESS           0x00
#define HW_KB_NO_DATA           0x01
#define HW_KB_ACCESS_DENIED     0x02
#define HW_KB_WOULD_BLOCK       0x03
#define HW_KB_INTERRUPTED       0x04
#define HW_KB_INVALID_PARAM     0x05

// VGA Opcodes (0x70-0x7B)
#define HW_VGA_PUTCHAR          0x70
#define HW_VGA_PUTSTRING        0x71
#define HW_VGA_CLEAR_SCREEN     0x72
#define HW_VGA_CLEAR_LINE       0x73
#define HW_VGA_CLEAR_TO_EOL     0x74
#define HW_VGA_GET_CURSOR       0x75
#define HW_VGA_SET_CURSOR       0x76
#define HW_VGA_SET_COLOR        0x77
#define HW_VGA_GET_COLOR        0x78
#define HW_VGA_SCROLL_UP        0x79
#define HW_VGA_NEWLINE          0x7A
#define HW_VGA_GET_DIMENSIONS   0x7B

// VGA Error Codes
#define VGA_SUCCESS              0x00
#define VGA_ERR_OUT_OF_BOUNDS    0x01
#define VGA_ERR_INVALID_CURSOR   0x02
#define VGA_ERR_ACCESS_DENIED    0x03
#define VGA_ERR_STRING_TOO_LONG  0x05

// Power Management Opcodes
#define HW_SYSTEM_REBOOT         0x80
#define HW_SYSTEM_SHUTDOWN       0x81

// USB Host Controller Operations (0x90-0x9F)
#define HW_USB_INIT              0x90
#define HW_USB_RESET             0x91
#define HW_USB_START             0x92
#define HW_USB_STOP              0x93
#define HW_USB_PORT_STATUS       0x94
#define HW_USB_PORT_RESET        0x95
#define HW_USB_PORT_QUERY        0x98
#define HW_USB_ENUM_DEVICE       0x96
#define HW_USB_GET_INFO          0x97

// USB Error Codes
#define USB_ERR_NOT_INITIALIZED  0x10
#define USB_ERR_NO_CONTROLLER    0x11
#define USB_ERR_RESET_TIMEOUT    0x12
#define USB_ERR_INVALID_PORT     0x13
#define USB_ERR_NO_DEVICE        0x14
#define USB_ERR_ACCESS_DENIED    0x15

int hardware_deck_handler(Pocket* pocket);

#endif // HARDWARE_DECK_H
