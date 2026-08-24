/*
 * lifecycle — sanity-check for the new kernel-published Touch events:
 *   process:spawned, process:died, system:shutdown/reboot, usb:connect.
 *
 * Subscribes (REST) to the lifecycle tags, spawns a short-lived child,
 * and prints what arrives. End-to-end verification that the kernel
 * publish call sites and userspace tag spelling agree.
 */

#include "box/touch.h"
#include "box/debug.h"
#include "box/system.h"
#include "box/core/cabin.h"
#include "box/core/result.h"
#include "box/string.h"

int main(void)
{
    CabinInfo *ci = cabin_info();

    /* Child role: just exit. The parent's subscribers should observe
     * a process:spawned (from our own creation) followed by a
     * process:died when we exit below. */
    if (ci && ci->spawner_pid != 0 && ci->spawner_pid != 2) {
        kdbg_print("[LIFECYCLE child %lu] hello & exit", (unsigned long)ci->spawner_pid);
        exit(0);
    }

    kdbg_print("[LIFECYCLE] subscribing to process:spawned + process:died");

    /* Claimed first and drained last: a Touch nobody holds is a Touch nobody
     * receives, so the subscription has to exist before the thing it is
     * waiting for happens. Nothing here fails when no USB device is plugged
     * during the run — that is the ordinary case, and the point of this claim
     * is to report an arrival when there is one, not to demand one. */
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

    /* Drain spawned + died for our child (and any unrelated lifecycle
     * traffic that happens to land in our ring during the window). */
    int saw_spawn = 0, saw_die = 0;
    for (int i = 0; i < 60 && (!saw_spawn || !saw_die); i++) {
        Touch t;
        int rc = touch_await(spawned_tag, &t, 100);
        if (rc == 0) {
            const uint8_t *p = t.payload;
            uint32_t pid = 0, parent = 0;
            if (t.payload_len >= 8) {
                memcpy(&pid,    p + 0, 4);
                memcpy(&parent, p + 4, 4);
            }
            kdbg_print("[LIFECYCLE] saw spawn pid=%u parent=%u tagid=%u",
                       pid, parent, (unsigned)t.tag_id);
            if ((int)pid == child) saw_spawn = 1;
        }
        rc = touch_await(died_tag, &t, 100);
        if (rc == 0) {
            const uint8_t *p = t.payload;
            uint32_t pid = 0;
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

    /* Whatever USB arrived or left while the above was running.
     *
     * One await, and the label comes from t.tag_id — not from which call
     * returned. touch_await takes a tag to wait ON, but what it hands back is
     * the next Touch in this process's ring whichever tag it carries, so a
     * departure will happily come back out of a call that named usb:arrived.
     * Labelling by call site produced exactly that: a device being plugged IN
     * and reported as having left. The event knows what it is; ask it. */
    if (usb_watching) {
        int seen = 0;
        int quiet = 0;
        for (int i = 0; i < 32; i++) {
            Touch t;
            if (touch_await(usb_arrived, &t, 100) != 0) {
                if (++quiet >= 3) break;
                continue;
            }
            quiet = 0;

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
