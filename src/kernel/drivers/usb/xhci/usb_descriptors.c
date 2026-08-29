#include "usb_descriptors.h"
#include "klib.h"

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

/*
 * Walking a configuration descriptor.
 *
 * What follows the nine-byte header is a stream of descriptors, each stating
 * its own length, and the only thing holding it together is that every bLength
 * is honest. It arrives over a wire from a device this kernel has never met, so
 * every step is bounded: a zero or absurd length ends the walk rather than
 * looping on it, and no descriptor is read past the end of what was fetched.
 */
typedef struct {
    const uint8_t* ptr;
    const uint8_t* end;
} desc_walk_t;

static bool desc_walk_begin(desc_walk_t* w, const void* data, uint16_t len)
{
    if (!data || len < 9) {
        return false;
    }

    const usb_config_desc_t* cfg = (const usb_config_desc_t*)data;
    if (cfg->bDescriptorType != USB_DESC_CONFIGURATION || cfg->bLength < 9) {
        return false;
    }

    /* Trust the smaller of what the device claims and what was read. */
    uint16_t total = cfg->wTotalLength;
    if (total > len) {
        total = len;
    }

    w->ptr = (const uint8_t*)data + cfg->bLength;
    w->end = (const uint8_t*)data + total;
    return true;
}

/* Next descriptor, or NULL at the end. On return *type and *dlen describe it. */
static const uint8_t* desc_walk_next(desc_walk_t* w, uint8_t* type, uint8_t* dlen)
{
    if (w->ptr + 2 > w->end) {
        return NULL;
    }

    uint8_t length = w->ptr[0];
    if (length < 2 || w->ptr + length > w->end) {
        return NULL;                    /* a length that cannot be true */
    }

    const uint8_t* here = w->ptr;
    *type = w->ptr[1];
    *dlen = length;
    w->ptr += length;
    return here;
}

/*
 * How many isochronous endpoints this configuration has, across ALL of its
 * interfaces.
 *
 * usb_walk_interface answers about ONE interface — the first that matches a
 * class triple — which is the right question for finding the endpoints a
 * driver needs and the wrong one for finding out what a device has and this
 * kernel does not use. An audio device keeps its isochronous endpoints on a
 * streaming interface nobody here walks, so from every existing caller's point
 * of view they do not exist.
 */
uint8_t usb_count_isoch_endpoints(const void* data, uint16_t len)
{
    desc_walk_t w;
    if (!desc_walk_begin(&w, data, len)) {
        return 0;
    }

    uint8_t seen = 0;
    const uint8_t* d;
    uint8_t type, dlen;
    while ((d = desc_walk_next(&w, &type, &dlen)) != NULL) {
        if (type != USB_DESC_ENDPOINT || dlen < sizeof(usb_endpoint_desc_t)) {
            continue;
        }
        const usb_endpoint_desc_t* ep = (const usb_endpoint_desc_t*)d;
        if ((ep->bmAttributes & 0x03u) == USB_EP_XFER_ISOCH && seen < 255) {
            seen++;
        }
    }
    return seen;
}

void usb_config_first_interface(const void* data, uint16_t len,
                                uint8_t* out_class, uint8_t* out_subclass,
                                uint8_t* out_protocol)
{
    if (out_class)    *out_class = 0;
    if (out_subclass) *out_subclass = 0;
    if (out_protocol) *out_protocol = 0;

    desc_walk_t w;
    if (!desc_walk_begin(&w, data, len)) {
        return;
    }

    const uint8_t* d;
    uint8_t type, dlen;
    while ((d = desc_walk_next(&w, &type, &dlen)) != NULL) {
        if (type == USB_DESC_INTERFACE && dlen >= 9) {
            const usb_interface_desc_t* iface = (const usb_interface_desc_t*)d;
            if (out_class)    *out_class = iface->bInterfaceClass;
            if (out_subclass) *out_subclass = iface->bInterfaceSubClass;
            if (out_protocol) *out_protocol = iface->bInterfaceProtocol;
            return;
        }
    }
}

static bool iface_matches(const usb_interface_desc_t* iface,
                          uint8_t klass, uint8_t subclass, uint8_t proto)
{
    if (klass    != USB_CLASS_ANY && iface->bInterfaceClass    != klass)    return false;
    if (subclass != USB_CLASS_ANY && iface->bInterfaceSubClass != subclass) return false;
    if (proto    != USB_CLASS_ANY && iface->bInterfaceProtocol != proto)    return false;
    return true;
}

bool usb_walk_interface(const void* data, uint16_t len,
                        uint8_t klass, uint8_t subclass, uint8_t proto,
                        uint8_t* out_iface_num,
                        usb_endpoint_visitor visit, void* ctx)
{
    desc_walk_t w;
    if (!desc_walk_begin(&w, data, len)) {
        return false;
    }

    bool inside = false;
    bool found  = false;

    /*
     * An endpoint is not finished being described until the descriptor after
     * it has been looked at.
     *
     * At SuperSpeed every endpoint descriptor is followed by a companion
     * (USB 3.2 Section 9.6.7) carrying the one number the endpoint descriptor
     * has nowhere to put: how many packets may go back to back. So an endpoint
     * is held here until the next descriptor arrives, and only then handed to
     * the visitor — with its companion if one followed, without if none did.
     * Reporting it immediately, which is what this did, meant the companion
     * was skipped over as an unrecognised descriptor and every SuperSpeed
     * endpoint on the machine was configured for a burst of one.
     */
    usb_endpoint_info_t pending;
    bool have_pending = false;
    memset(&pending, 0, sizeof(pending));

    const uint8_t* d;
    uint8_t type, dlen;
    while ((d = desc_walk_next(&w, &type, &dlen)) != NULL) {

        /* The companion belongs to the endpoint immediately before it, and to
         * nothing else. One that arrives with no endpoint held is a device
         * describing itself wrongly, and is ignored rather than guessed at. */
        if (type == USB_DESC_SS_EP_COMPANION && dlen >= 6) {
            if (have_pending) {
                const usb_ss_ep_companion_desc_t* c =
                    (const usb_ss_ep_companion_desc_t*)d;
                pending.max_burst          = c->bMaxBurst;
                pending.bytes_per_interval = c->wBytesPerInterval;
                pending.has_companion      = true;

                /* Mult is the isochronous half of bmAttributes; the bulk half
                 * of the same byte is MaxStreams, which this driver does not
                 * use and must not mistake for a multiplier. */
                if ((pending.attributes & 0x03) == USB_EP_XFER_ISOCH) {
                    pending.mult = (uint8_t)(c->bmAttributes & 0x03);
                }
            }
            continue;
        }

        if (have_pending) {
            have_pending = false;
            if (visit && !visit(ctx, &pending)) {
                return found;
            }
        }

        if (type == USB_DESC_INTERFACE && dlen >= 9) {
            const usb_interface_desc_t* iface = (const usb_interface_desc_t*)d;

            /* A second interface begins where the first one's endpoints end.
             * Alternate settings of the SAME interface number are a different
             * thing, and this driver uses only setting zero — an alternate
             * setting redefines the endpoints, so walking into one would
             * report endpoints the device is not currently using. */
            if (inside) {
                break;
            }

            if (iface->bAlternateSetting == 0 &&
                iface_matches(iface, klass, subclass, proto)) {
                inside = true;
                found  = true;
                if (out_iface_num) {
                    *out_iface_num = iface->bInterfaceNumber;
                }
            }
            continue;
        }

        if (inside && type == USB_DESC_ENDPOINT && dlen >= 7) {
            const usb_endpoint_desc_t* ep = (const usb_endpoint_desc_t*)d;
            memset(&pending, 0, sizeof(pending));
            pending.addr       = ep->bEndpointAddress;
            pending.attributes = ep->bmAttributes;
            /* Bits 12:11 carry the additional-transactions-per-microframe
             * count on high speed, in the same "one less than the count"
             * encoding the SuperSpeed companion uses; the size is the low
             * eleven bits. A device slower than high speed states zero there,
             * so this needs no help from the port to be right. */
            pending.max_packet = (uint16_t)(ep->wMaxPacketSize & 0x07FF);
            pending.max_burst  = (uint8_t)((ep->wMaxPacketSize >> 11) & 0x03);
            pending.interval   = ep->bInterval;
            have_pending = true;
        }
    }

    /* The last endpoint of the last interface has nothing after it. */
    if (have_pending && visit) {
        visit(ctx, &pending);
    }

    return found;
}
