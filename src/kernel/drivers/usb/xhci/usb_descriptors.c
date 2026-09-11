#include "usb_descriptors.h"
#include "klib.h"

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
        return desc->bMaxPacketSize0 == 9;
    }

    return desc->bMaxPacketSize0 == 8  ||
           desc->bMaxPacketSize0 == 16 ||
           desc->bMaxPacketSize0 == 32 ||
           desc->bMaxPacketSize0 == 64;
}

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

    uint16_t total = cfg->wTotalLength;
    if (total > len) {
        total = len;
    }

    w->ptr = (const uint8_t*)data + cfg->bLength;
    w->end = (const uint8_t*)data + total;
    return true;
}

static const uint8_t* desc_walk_next(desc_walk_t* w, uint8_t* type, uint8_t* dlen)
{
    if (w->ptr + 2 > w->end) {
        return NULL;
    }

    uint8_t length = w->ptr[0];
    if (length < 2 || w->ptr + length > w->end) {
        return NULL;
    }

    const uint8_t* here = w->ptr;
    *type = w->ptr[1];
    *dlen = length;
    w->ptr += length;
    return here;
}

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

    usb_endpoint_info_t pending;
    bool have_pending = false;
    memset(&pending, 0, sizeof(pending));

    const uint8_t* d;
    uint8_t type, dlen;
    while ((d = desc_walk_next(&w, &type, &dlen)) != NULL) {

        if (type == USB_DESC_SS_EP_COMPANION && dlen >= 6) {
            if (have_pending) {
                const usb_ss_ep_companion_desc_t* c =
                    (const usb_ss_ep_companion_desc_t*)d;
                pending.max_burst          = c->bMaxBurst;
                pending.bytes_per_interval = c->wBytesPerInterval;
                pending.has_companion      = true;

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
            pending.max_packet = (uint16_t)(ep->wMaxPacketSize & 0x07FF);
            pending.max_burst  = (uint8_t)((ep->wMaxPacketSize >> 11) & 0x03);
            pending.interval   = ep->bInterval;
            have_pending = true;
        }
    }

    if (have_pending && visit) {
        visit(ctx, &pending);
    }

    return found;
}