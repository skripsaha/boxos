#ifndef XHCI_HID_H
#define XHCI_HID_H

#include "ktypes.h"

#define USB_HID_CLASS             0x03
#define USB_HID_SUBCLASS_BOOT     0x01
#define USB_HID_PROTOCOL_KEYBOARD 0x01

#define HID_REQ_SET_IDLE          0x0A
#define HID_REQ_SET_PROTOCOL      0x0B

/* The boot-protocol keyboard report: eight bytes, and the same eight on every
 * keyboard that claims the boot subclass. That is the whole point of the boot
 * protocol — it is what a BIOS can read without parsing a report descriptor,
 * and it is what this kernel reads for the same reason. */
typedef struct {
    uint8_t modifiers;
    uint8_t reserved;
    uint8_t keycodes[6];
} __attribute__((packed)) usb_boot_keyboard_report_t;

void xhci_process_keyboard_report(usb_boot_keyboard_report_t* report);

#endif
