#ifndef USB_COMMON_H
#define USB_COMMON_H

#include "ktypes.h"

#define USB_CLASS_MASS_STORAGE   0x08
#define USB_CLASS_HID            0x03
#define USB_CLASS_HUB            0x09

#define USB_SPEED_FULL           0x01
#define USB_SPEED_LOW            0x02
#define USB_SPEED_HIGH           0x03
#define USB_SPEED_SUPER          0x04

#define USB_DIR_OUT              0x00
#define USB_DIR_IN               0x80

#define USB_REQ_GET_DESCRIPTOR   0x06
#define USB_REQ_SET_ADDRESS      0x05
#define USB_REQ_SET_CONFIGURATION 0x09

#define USB_DT_DEVICE            0x01
#define USB_DT_CONFIG            0x02
#define USB_DT_STRING            0x03
#define USB_DT_INTERFACE         0x04
#define USB_DT_ENDPOINT          0x05

typedef struct {
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed)) usb_setup_packet_t;

typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubClass;
    uint8_t bDeviceProtocol;
    uint8_t bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t iManufacturer;
    uint8_t iProduct;
    uint8_t iSerialNumber;
    uint8_t bNumConfigurations;
} __attribute__((packed)) usb_device_descriptor_t;

#endif
