#include "usb_descriptors.h"

bool usb_validate_device_desc(usb_device_desc_t* desc) {
    if (!desc) {
        return false;
    }

    if (desc->bLength != 18) {
        return false;
    }

    if (desc->bDescriptorType != USB_DESC_DEVICE) {
        return false;
    }

    if (desc->bMaxPacketSize0 != 8 &&
        desc->bMaxPacketSize0 != 16 &&
        desc->bMaxPacketSize0 != 32 &&
        desc->bMaxPacketSize0 != 64) {
        return false;
    }

    return true;
}
