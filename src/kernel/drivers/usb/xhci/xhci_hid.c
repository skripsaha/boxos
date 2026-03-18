#include "xhci_hid.h"
#include "keyboard.h"
#include "keymaps.h"
#include "klib.h"

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

int xhci_parse_config_descriptor(void* data, uint16_t len, usb_keyboard_info_t* info)
{
    if (!data || !info || len < 9) {
        return -1;
    }

    uint8_t* ptr = (uint8_t*)data;
    usb_config_desc_t* cfg = (usb_config_desc_t*)ptr;

    if (cfg->bDescriptorType != USB_DESC_CONFIGURATION) {
        return -2;
    }

    info->config_value = cfg->bConfigurationValue;
    uint16_t total_len = cfg->wTotalLength < len ? cfg->wTotalLength : len;

    ptr += cfg->bLength;
    uint8_t found = 0;

    while (ptr + 2 < (uint8_t*)data + total_len) {
        uint8_t desc_len = ptr[0];
        uint8_t desc_type = ptr[1];

        if (desc_len < 2 || ptr + desc_len > (uint8_t*)data + total_len) {
            break;
        }

        if (desc_type == USB_DESC_INTERFACE) {
            usb_interface_desc_t* iface = (usb_interface_desc_t*)ptr;
            if (iface->bInterfaceClass == USB_HID_CLASS &&
                iface->bInterfaceSubClass == USB_HID_SUBCLASS_BOOT &&
                iface->bInterfaceProtocol == USB_HID_PROTOCOL_KEYBOARD) {
                info->interface_num = iface->bInterfaceNumber;
                found = 1;
            }
        }

        if (found && desc_type == USB_DESC_ENDPOINT) {
            usb_endpoint_desc_t* ep = (usb_endpoint_desc_t*)ptr;
            if ((ep->bmAttributes & 0x03) == USB_EP_TYPE_INTERRUPT &&
                (ep->bEndpointAddress & 0x80)) {
                info->endpoint_addr = ep->bEndpointAddress;
                info->endpoint_dci = ((ep->bEndpointAddress & 0x7F) * 2) + 1;
                info->max_packet_size = ep->wMaxPacketSize;
                info->interval = ep->bInterval;
                return 0;
            }
        }

        ptr += desc_len;
    }

    return found ? -3 : -4;
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
    kb->shift_pressed = (new_mods & 0x22) ? 1 : 0;
    kb->ctrl_pressed  = (new_mods & 0x11) ? 1 : 0;
    kb->alt_pressed   = (new_mods & 0x44) ? 1 : 0;

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
