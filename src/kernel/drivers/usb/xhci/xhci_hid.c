#include "xhci_hid.h"
#include "keyboard.h"
#include "keymaps.h"
#include "klib.h"
#include "touch.h"

/* Check if a USB HID keycode is an extended key and push its sequence.
   Returns 1 if handled, 0 if not an extended key. */
static int usb_handle_ext_key(uint8_t usb_code, int is_release)
{
    for (int i = 0; usb_ext_keys[i].usb_code != 0; i++) {
        if (usb_ext_keys[i].usb_code == usb_code) {
            if (!is_release) {
                keyboard_push_sequence(usb_ext_keys[i].seq,
                                       usb_ext_keys[i].seq_len);
            }
            /* Extended key releases produce no output but are "handled" */
            return 1;
        }
    }
    return 0;
}

static usb_boot_keyboard_report_t prev_report = {0};

/*
 * Walking a configuration descriptor.
 *
 * What follows the nine-byte header is a stream of descriptors, each stating
 * its own length, and the only thing holding it together is that every
 * bLength is honest. It arrives over a wire from a device this kernel has
 * never met, so every step is bounded: a zero or absurd length ends the walk
 * rather than looping on it, and no descriptor is read past the end of what
 * was actually fetched.
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

void xhci_usb_interface_summary(const void* data, uint16_t len,
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

/*
 * Find the boot-protocol keyboard interface and its interrupt IN endpoint.
 *
 * The endpoint has to belong to the keyboard interface and to no other. A
 * keyboard that also carries media keys or a fingerprint reader describes
 * several interfaces, each with its own endpoints, in one flat stream — so the
 * walk stops looking as soon as a *different* interface begins, rather than
 * carrying "found" forward and attaching the first interrupt endpoint it meets
 * afterwards to a keyboard that does not own it.
 */
int xhci_parse_config_descriptor(void* data, uint16_t len, usb_keyboard_info_t* info)
{
    if (!data || !info) {
        return -1;
    }

    const usb_config_desc_t* cfg = (const usb_config_desc_t*)data;
    desc_walk_t w;
    if (!desc_walk_begin(&w, data, len)) {
        return -2;
    }

    info->config_value = cfg->bConfigurationValue;

    bool in_keyboard_interface = false;
    bool saw_keyboard = false;

    const uint8_t* d;
    uint8_t type, dlen;
    while ((d = desc_walk_next(&w, &type, &dlen)) != NULL) {

        if (type == USB_DESC_INTERFACE && dlen >= 9) {
            const usb_interface_desc_t* iface = (const usb_interface_desc_t*)d;
            in_keyboard_interface =
                (iface->bInterfaceClass    == USB_HID_CLASS &&
                 iface->bInterfaceSubClass == USB_HID_SUBCLASS_BOOT &&
                 iface->bInterfaceProtocol == USB_HID_PROTOCOL_KEYBOARD);
            if (in_keyboard_interface) {
                info->interface_num = iface->bInterfaceNumber;
                saw_keyboard = true;
            }
            continue;
        }

        if (in_keyboard_interface && type == USB_DESC_ENDPOINT && dlen >= 7) {
            const usb_endpoint_desc_t* ep = (const usb_endpoint_desc_t*)d;
            if ((ep->bmAttributes & 0x03) == USB_EP_TYPE_INTERRUPT &&
                (ep->bEndpointAddress & 0x80)) {
                info->endpoint_addr = ep->bEndpointAddress;
                /* Device Context Index: endpoint number doubled, plus one for
                 * an IN direction. */
                info->endpoint_dci =
                    (uint8_t)(((ep->bEndpointAddress & 0x0F) * 2) + 1);
                info->max_packet_size = ep->wMaxPacketSize & 0x07FF;
                info->interval = ep->bInterval;
                return 0;
            }
        }
    }

    return saw_keyboard ? -3 : -4;
}

void xhci_process_keyboard_report(usb_boot_keyboard_report_t* report)
{
    if (!report) {
        return;
    }

    /* Phantom state — too many keys pressed simultaneously */
    if (report->keycodes[0] == 0x01 &&
        report->keycodes[1] == 0x01 &&
        report->keycodes[2] == 0x01) {
        debug_printf("[xHCI HID] Phantom state detected - too many keys pressed\n");
        return;
    }

    keyboard_state_t* kb = keyboard_get_state();

    uint8_t new_mods = report->modifiers;
    /* Atomic stores: kb_state is also written by PS/2 IRQ on BSP and read
     * from any user-thread core (shell line discipline / Manifest hw_kb). */
    __atomic_store_n(&kb->shift_pressed, (new_mods & 0x22) ? 1 : 0, __ATOMIC_RELAXED);
    __atomic_store_n(&kb->ctrl_pressed,  (new_mods & 0x11) ? 1 : 0, __ATOMIC_RELAXED);
    __atomic_store_n(&kb->alt_pressed,   (new_mods & 0x44) ? 1 : 0, __ATOMIC_RELAXED);

    /* ── Released keys ── */
    for (int i = 0; i < 6; i++) {
        uint8_t old_key = prev_report.keycodes[i];
        if (old_key == 0) continue;

        uint8_t still_pressed = 0;
        for (int j = 0; j < 6; j++) {
            if (report->keycodes[j] == old_key) {
                still_pressed = 1;
                break;
            }
        }

        if (!still_pressed) {
            /* Try extended key first (arrows, nav) — release produces no output */
            if (usb_handle_ext_key(old_key, 1)) continue;

            uint8_t ps2 = usb_to_ps2[old_key];
            if (ps2 != 0) {
                keyboard_handle_scancode(ps2 | 0x80);
            }
        }
    }

    /* ── Newly pressed keys ── */
    for (int i = 0; i < 6; i++) {
        uint8_t new_key = report->keycodes[i];
        if (new_key == 0) continue;

        uint8_t was_pressed = 0;
        for (int j = 0; j < 6; j++) {
            if (prev_report.keycodes[j] == new_key) {
                was_pressed = 1;
                break;
            }
        }

        if (!was_pressed) {
            /* Try extended key first (arrows, nav) — pushes escape sequence */
            if (usb_handle_ext_key(new_key, 0)) continue;

            uint8_t ps2 = usb_to_ps2[new_key];
            if (ps2 != 0) {
                keyboard_handle_scancode(ps2);
            }
        }
    }

    memcpy(&prev_report, report, sizeof(usb_boot_keyboard_report_t));
}
