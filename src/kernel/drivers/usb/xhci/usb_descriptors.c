#include "usb_descriptors.h"

/*
 * Is this eighteen bytes a device descriptor, or is it whatever the bus
 * happened to leave in the buffer?
 *
 * The one field worth explaining is bMaxPacketSize0. Below USB 3.0 it is a
 * byte count and the specification allows exactly four of them. At USB 3.0 and
 * above it is not a count at all — it is an exponent, always 9, meaning 512 —
 * and a check that only knows the older meaning rejects every SuperSpeed
 * device on the bus as malformed. It did: a USB 3 flash drive enumerated
 * perfectly all the way to here and was thrown away at the last step.
 *
 * The descriptor says which rule applies, in bcdUSB, so there is no need to be
 * told the port speed to know.
 */
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

    if (desc->bcdUSB >= 0x0300) {
        return desc->bMaxPacketSize0 == 9;      /* 2^9 = 512, and only that */
    }

    return desc->bMaxPacketSize0 == 8  ||
           desc->bMaxPacketSize0 == 16 ||
           desc->bMaxPacketSize0 == 32 ||
           desc->bMaxPacketSize0 == 64;
}
