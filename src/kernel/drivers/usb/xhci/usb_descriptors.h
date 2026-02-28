#ifndef USB_DESCRIPTORS_H
#define USB_DESCRIPTORS_H

#include "ktypes.h"
#include "usb_common.h"

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
    uint8_t iSerial;
    uint8_t bNumConfigurations;
} __attribute__((packed)) usb_device_desc_t;

#define USB_DESC_DEVICE        0x01
#define USB_DESC_CONFIG        0x02
#define USB_DESC_STRING        0x03

#define USB_RT_DEVICE          0x00
#define USB_RT_INTERFACE       0x01
#define USB_RT_ENDPOINT        0x02

bool usb_validate_device_desc(usb_device_desc_t* desc);

#endif
