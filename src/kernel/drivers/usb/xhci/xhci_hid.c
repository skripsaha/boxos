#include "xhci_hid.h"
#include "usb_descriptors.h"
#include "keyboard.h"
#include "keymaps.h"
#include "klib.h"
#include "touch.h"

/* A USB extended key (arrow, Home/End, Delete, …) takes the road a PS/2 one
 * takes: the 0xE0 prefix, then its set-1 scancode (with the release bit for a
 * release), so it is published, repeated and released by the one driver.
 * Returns 1 if the usage was an extended key, 0 if not. */
static int usb_handle_ext_key(uint8_t usb_code, int is_release)
{
    for (int i = 0; usb_ext_keys[i].usb_code != 0; i++) {
        if (usb_ext_keys[i].usb_code == usb_code) {
            keyboard_handle_scancode(0xE0);
            keyboard_handle_scancode((uint8_t)(usb_ext_keys[i].scancode |
                                               (is_release ? 0x80 : 0)));
            return 1;
        }
    }
    return 0;
}

static usb_boot_keyboard_report_t prev_report = {0};

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
