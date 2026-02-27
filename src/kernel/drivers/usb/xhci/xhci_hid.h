#ifndef XHCI_HID_H
#define XHCI_HID_H

#include "ktypes.h"

#define USB_DESC_CONFIGURATION  0x02
#define USB_DESC_INTERFACE      0x04
#define USB_DESC_ENDPOINT       0x05
#define USB_DESC_HID            0x21

#define USB_HID_CLASS           0x03
#define USB_HID_SUBCLASS_BOOT   0x01
#define USB_HID_PROTOCOL_KEYBOARD 0x01

#define HID_REQ_SET_IDLE        0x0A
#define HID_REQ_SET_PROTOCOL    0x0B

#define USB_REQ_SET_CONFIGURATION 0x09

#define USB_EP_TYPE_INTERRUPT   0x03

#define XHCI_EP_TYPE_INTERRUPT_IN 7

typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t wTotalLength;
    uint8_t bNumInterfaces;
    uint8_t bConfigurationValue;
    uint8_t iConfiguration;
    uint8_t bmAttributes;
    uint8_t bMaxPower;
} __attribute__((packed)) usb_config_desc_t;

typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} __attribute__((packed)) usb_interface_desc_t;

typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bEndpointAddress;
    uint8_t bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
} __attribute__((packed)) usb_endpoint_desc_t;

typedef struct {
    uint8_t modifiers;
    uint8_t reserved;
    uint8_t keycodes[6];
} __attribute__((packed)) usb_boot_keyboard_report_t;

typedef struct {
    uint8_t interface_num;
    uint8_t endpoint_addr;
    uint8_t endpoint_dci;
    uint16_t max_packet_size;
    uint8_t interval;
    uint8_t config_value;
} usb_keyboard_info_t;

int xhci_parse_config_descriptor(void* data, uint16_t len, usb_keyboard_info_t* info);
void xhci_process_keyboard_report(usb_boot_keyboard_report_t* report);

#endif
