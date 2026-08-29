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

/*
 * SuperSpeed Endpoint Companion (USB 3.2 Section 9.6.7).
 *
 * At SuperSpeed an endpoint descriptor is only half the story: how many
 * packets the endpoint can move back to back is stated here and nowhere else.
 * A device that says nothing about it does not exist — the companion is
 * mandatory for every endpoint of every SuperSpeed interface — but a HOST that
 * does not read it configures the endpoint for one packet per burst, which for
 * a flash drive that can do sixteen is fifteen sixteenths of the bus left on
 * the floor. Nothing reports this; it simply runs slowly, forever.
 *
 * bMaxBurst is the burst size MINUS ONE, and the xHCI Max Burst Size field is
 * the same encoding, so the value travels unchanged.
 */
typedef struct {
    uint8_t  bLength;               /* 6 */
    uint8_t  bDescriptorType;       /* 0x30 */
    uint8_t  bMaxBurst;             /* packets per burst, minus one */
    uint8_t  bmAttributes;          /* bulk: MaxStreams[4:0]; isoch: Mult[1:0] */
    uint16_t wBytesPerInterval;     /* periodic endpoints only */
} __attribute__((packed)) usb_ss_ep_companion_desc_t;

#define USB_DESC_DEVICE        0x01
#define USB_DESC_CONFIG        0x02
#define USB_DESC_CONFIGURATION 0x02
#define USB_DESC_STRING        0x03
#define USB_DESC_INTERFACE     0x04
#define USB_DESC_ENDPOINT      0x05
#define USB_DESC_SS_EP_COMPANION 0x30

#define USB_RT_DEVICE          0x00
#define USB_RT_INTERFACE       0x01
#define USB_RT_ENDPOINT        0x02

/* Transfer types in bmAttributes[1:0]. */
#define USB_EP_XFER_CONTROL    0
#define USB_EP_XFER_ISOCH      1
#define USB_EP_XFER_BULK       2
#define USB_EP_XFER_INTERRUPT  3

/* "Any" in an interface match. 0xFF is a legal class value in USB, but not one
 * this kernel drives, so borrowing it as the wildcard costs nothing real and
 * keeps the match a single triple instead of a triple plus three flags. */
#define USB_CLASS_ANY          0xFF

bool usb_validate_device_desc(usb_device_desc_t* desc);

/* Class, subclass and protocol of the first interface a configuration
 * describes. This is what decides who drives the device — including the answer
 * "nobody, yet", which is a legitimate outcome and not a failure. */
void usb_config_first_interface(const void* cfg, uint16_t len,
                                uint8_t* out_class, uint8_t* out_subclass,
                                uint8_t* out_protocol);

typedef struct {
    uint8_t  addr;              /* bEndpointAddress, direction in bit 7 */
    uint8_t  attributes;        /* transfer type in bits 1:0 */
    uint16_t max_packet;        /* the 11-bit size, burst bits masked off */
    uint8_t  interval;          /* bInterval, in the units of the device's speed */

    /*
     * How much the endpoint can move at once, which is stated in two
     * completely different places depending on how fast the device is.
     *
     * At high speed a periodic endpoint asks for extra transactions per
     * microframe in bits 12:11 of wMaxPacketSize — the field this walk used to
     * mask off and throw away. At SuperSpeed every endpoint states its burst
     * in a companion descriptor that follows it. Both end up in the same xHCI
     * Max Burst Size field, in the same encoding: one less than the count.
     *
     * bytes_per_interval is the companion's, and is what the controller
     * reserves bandwidth from for a periodic SuperSpeed endpoint. Zero means
     * the device never said, and the caller computes the older way.
     */
    uint8_t  max_burst;         /* packets per burst, minus one */
    uint8_t  mult;              /* SuperSpeed isochronous only */
    uint16_t bytes_per_interval;
    bool     has_companion;
} usb_endpoint_info_t;

/* Return false to stop the walk. */
typedef bool (*usb_endpoint_visitor)(void* ctx, const usb_endpoint_info_t* ep);

/*
 * Walk the endpoints belonging to the first interface matching a class triple.
 *
 * A configuration descriptor is a flat stream: interfaces and their endpoints
 * are told apart only by the order they appear in, so an endpoint belongs to
 * whichever interface most recently began. The walk therefore stops the moment
 * a different interface starts, which is what keeps a keyboard from claiming
 * the interrupt endpoint of the media-key interface sitting next to it.
 *
 * Endpoints are reported to a visitor rather than collected into an array, so
 * there is no count to cap and nothing to truncate: each caller keeps the one
 * or two it recognises and ignores the rest.
 *
 * Returns false when no interface matches.
 */
/* How many isochronous endpoints the whole configuration carries — see the
 * note in the implementation for why usb_walk_interface cannot answer this. */
uint8_t usb_count_isoch_endpoints(const void* cfg, uint16_t len);

bool usb_walk_interface(const void* cfg, uint16_t len,
                        uint8_t klass, uint8_t subclass, uint8_t proto,
                        uint8_t* out_iface_num,
                        usb_endpoint_visitor visit, void* ctx);

#endif
