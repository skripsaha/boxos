#include "xhci_hid.h"
#include "keyboard.h"
#include "klib.h"

/* ─── USB HID keycode → PS/2 Set 1 scancode (normal keys) ─────────────── */

static const uint8_t usb_to_ps2[256] = {
    [0x04] = 0x1E, [0x05] = 0x30, [0x06] = 0x2E, [0x07] = 0x20,
    [0x08] = 0x12, [0x09] = 0x21, [0x0A] = 0x22, [0x0B] = 0x23,
    [0x0C] = 0x17, [0x0D] = 0x24, [0x0E] = 0x25, [0x0F] = 0x26,
    [0x10] = 0x32, [0x11] = 0x31, [0x12] = 0x18, [0x13] = 0x19,
    [0x14] = 0x10, [0x15] = 0x13, [0x16] = 0x1F, [0x17] = 0x14,
    [0x18] = 0x16, [0x19] = 0x2F, [0x1A] = 0x11, [0x1B] = 0x2D,
    [0x1C] = 0x15, [0x1D] = 0x2C, [0x1E] = 0x02, [0x1F] = 0x03,
    [0x20] = 0x04, [0x21] = 0x05, [0x22] = 0x06, [0x23] = 0x07,
    [0x24] = 0x08, [0x25] = 0x09, [0x26] = 0x0A, [0x27] = 0x0B,
    [0x28] = 0x1C, [0x29] = 0x01, [0x2A] = 0x0E, [0x2B] = 0x0F,
    [0x2C] = 0x39, [0x2D] = 0x0C, [0x2E] = 0x0D, [0x2F] = 0x1A,
    [0x30] = 0x1B, [0x31] = 0x2B, [0x33] = 0x27, [0x34] = 0x28,
    [0x35] = 0x29, [0x36] = 0x33, [0x37] = 0x34, [0x38] = 0x35,
    /* Caps Lock, F1-F12 */
    [0x39] = 0x3A,
    [0x3A] = 0x3B, [0x3B] = 0x3C, [0x3C] = 0x3D, [0x3D] = 0x3E,
    [0x3E] = 0x3F, [0x3F] = 0x40, [0x40] = 0x41, [0x41] = 0x42,
    [0x42] = 0x43, [0x43] = 0x44, [0x44] = 0x57, [0x45] = 0x58,
};

/* ─── USB HID extended keys → ANSI escape sequences ───────────────────── */
/* These keys bypass the PS/2 scancode path entirely and push
   their escape sequence directly into the keyboard buffer. */

typedef struct {
    uint8_t usb_code;
    char    seq[KB_SEQ_MAX];
    uint8_t seq_len;
} UsbExtKey;

static const UsbExtKey usb_ext_keys[] = {
    {0x4F, {'\033', '[', 'C', 0}, 3},      /* Right arrow */
    {0x50, {'\033', '[', 'D', 0}, 3},      /* Left arrow  */
    {0x51, {'\033', '[', 'B', 0}, 3},      /* Down arrow  */
    {0x52, {'\033', '[', 'A', 0}, 3},      /* Up arrow    */
    {0x4A, {'\033', '[', 'H', 0}, 3},      /* Home        */
    {0x4D, {'\033', '[', 'F', 0}, 3},      /* End         */
    {0x4B, {'\033', '[', '5', '~'}, 4},    /* Page Up     */
    {0x4E, {'\033', '[', '6', '~'}, 4},    /* Page Down   */
    {0x49, {'\033', '[', '2', '~'}, 4},    /* Insert      */
    {0x4C, {'\033', '[', '3', '~'}, 4},    /* Delete      */
    {0x00, {0, 0, 0, 0}, 0}                /* sentinel    */
};

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
