
#include "box/touch.h"
#include "box/debug.h"
#include "box/system.h"
#include "box/core/cabin.h"
#include "box/core/result.h"
#include "box/string.h"

#define LIFECYCLE_OWED_QUIET_MS   5000u
#define LIFECYCLE_USB_LOOK_MS      300u

static bool next_of_two(TouchTag a, TouchTag b, Touch *out, uint32_t quiet_ms)
{
    if (touch_try_pop_tag(a, out)) return true;
    if (touch_try_pop_tag(b, out)) return true;
    if (touch_await(a, out, quiet_ms) == 0) return true;
    return touch_try_pop_tag(b, out);
}

int main(void)
{
    CabinInfo *ci = cabin_info();

    if (ci && ci->spawner_pid != 0 && ci->spawner_pid != 2) {
        kdbg_print("[LIFECYCLE child %lu] hello & exit", (unsigned long)ci->spawner_pid);
        exit(0);
    }

    kdbg_print("[LIFECYCLE] subscribing to process:spawned + process:died");

    TouchTag usb_arrived = TOUCH_TAG_ID(TOUCH_TAG_USB_ARRIVED);
    TouchTag usb_left    = TOUCH_TAG_ID(TOUCH_TAG_USB_LEFT);
    int usb_watching = (touch_claim(usb_arrived, TOUCH_REST, 0, 0) == 0) &&
                       (touch_claim(usb_left,    TOUCH_REST, 0, 0) == 0);
    if (!usb_watching) {
        kdbg_print("[LIFECYCLE] WARN: could not claim usb:arrived / usb:left");
    }

    TouchTag spawned_tag = TOUCH_TAG_ID(TOUCH_TAG_PROCESS_SPAWNED);
    TouchTag died_tag    = TOUCH_TAG_ID(TOUCH_TAG_PROCESS_DIED);

    if (touch_claim(spawned_tag, TOUCH_REST, 0, 0) != 0) {
        kdbg_print("[LIFECYCLE] FAIL: claim process:spawned");
        return 1;
    }
    if (touch_claim(died_tag, TOUCH_REST, 0, 0) != 0) {
        kdbg_print("[LIFECYCLE] FAIL: claim process:died");
        touch_release(spawned_tag);
        return 1;
    }

    int child = proc_exec("lifecycle");
    if (child < 0) {
        kdbg_print("[LIFECYCLE] FAIL: spawn child rc=%d", child);
        touch_release(spawned_tag);
        touch_release(died_tag);
        return 1;
    }
    kdbg_print("[LIFECYCLE] spawned child pid=%d", child);

    int saw_spawn = 0, saw_die = 0;
    while (!saw_spawn || !saw_die) {
        Touch t;
        if (!next_of_two(spawned_tag, died_tag, &t, LIFECYCLE_OWED_QUIET_MS))
            break;

        const uint8_t *p = t.payload;
        if (t.tag_id == spawned_tag) {
            uint32_t pid = 0, parent = 0;
            if (t.payload_len >= 8) {
                memcpy(&pid,    p + 0, 4);
                memcpy(&parent, p + 4, 4);
            }
            kdbg_print("[LIFECYCLE] saw spawn pid=%u parent=%u tagid=%u",
                       pid, parent, (unsigned)t.tag_id);
            if ((int)pid == child) saw_spawn = 1;
        } else if (t.tag_id == died_tag) {
            uint32_t pid   = 0;
            int32_t  exitc = 0;
            if (t.payload_len >= 8) {
                memcpy(&pid,   p + 0, 4);
                memcpy(&exitc, p + 4, 4);
            }
            kdbg_print("[LIFECYCLE] saw die  pid=%u exit=%d", pid, exitc);
            if ((int)pid == child) saw_die = 1;
        }
    }

    touch_release(spawned_tag);
    touch_release(died_tag);

    if (usb_watching) {
        int seen = 0;
        for (;;) {
            Touch t;
            if (!next_of_two(usb_arrived, usb_left, &t, LIFECYCLE_USB_LOOK_MS))
                break;

            TouchUsbDevice d;
            memset(&d, 0, sizeof(d));
            if (t.payload_len >= (int)sizeof(d)) memcpy(&d, t.payload, sizeof(d));

            if (t.tag_id == usb_left) {
                kdbg_print("[LIFECYCLE] usb:left port=%u vid=%x pid=%x",
                           d.port, d.vendor_id, d.product_id);
            } else if (t.tag_id == usb_arrived) {
                kdbg_print("[LIFECYCLE] usb:arrived port=%u slot=%u speed=%u "
                           "vid=%x pid=%x class=%x/%x/%x usb=%x",
                           d.port, d.slot_id, d.speed, d.vendor_id, d.product_id,
                           d.dev_class, d.dev_subclass, d.dev_protocol,
                           d.usb_version);
            } else {
                kdbg_print("[LIFECYCLE] unexpected touch tagid=%u",
                           (unsigned)t.tag_id);
            }
            seen++;
        }
        kdbg_print("[LIFECYCLE] usb events seen: %d", seen);
        touch_release(usb_arrived);
        touch_release(usb_left);
    }

    if (saw_spawn && saw_die) {
        kdbg_print("[LIFECYCLE] PASS: spawn+die delivered for pid=%d", child);
        return 0;
    }
    kdbg_print("[LIFECYCLE] FAIL: saw_spawn=%d saw_die=%d", saw_spawn, saw_die);
    return 1;
}