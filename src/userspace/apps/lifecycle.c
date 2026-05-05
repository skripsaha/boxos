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

    if (touch_claim(TOUCH_TAG_PROCESS_SPAWNED, TOUCH_REST, 0, 0) != 0) {
        kdbg_print("[LIFECYCLE] FAIL: claim process:spawned");
        return 1;
    }
    if (touch_claim(TOUCH_TAG_PROCESS_DIED, TOUCH_REST, 0, 0) != 0) {
        kdbg_print("[LIFECYCLE] FAIL: claim process:died");
        touch_release(TOUCH_TAG_PROCESS_SPAWNED);
        return 1;
    }

    int child = proc_exec("lifecycle");
    if (child < 0) {
        kdbg_print("[LIFECYCLE] FAIL: spawn child rc=%d", child);
        touch_release(TOUCH_TAG_PROCESS_SPAWNED);
        touch_release(TOUCH_TAG_PROCESS_DIED);
        return 1;
    }
    kdbg_print("[LIFECYCLE] spawned child pid=%d", child);

    /* Drain spawned + died for our child (and any unrelated lifecycle
     * traffic that happens to land in our ring during the window). */
    int saw_spawn = 0, saw_die = 0;
    for (int i = 0; i < 60 && (!saw_spawn || !saw_die); i++) {
        Touch t;
        int rc = touch_await(TOUCH_TAG_PROCESS_SPAWNED, &t, 100);
        if (rc == 0) {
            const uint8_t *p = (const uint8_t *)(uintptr_t)t.payload_addr;
            uint32_t pid = 0, parent = 0;
            if (p && t.payload_len >= 8) {
                memcpy(&pid,    p + 0, 4);
                memcpy(&parent, p + 4, 4);
            }
            kdbg_print("[LIFECYCLE] saw spawn pid=%u parent=%u tagid=%u",
                       pid, parent, (unsigned)t.tag_id);
            if ((int)pid == child) saw_spawn = 1;
        }
        rc = touch_await(TOUCH_TAG_PROCESS_DIED, &t, 100);
        if (rc == 0) {
            const uint8_t *p = (const uint8_t *)(uintptr_t)t.payload_addr;
            uint32_t pid = 0;
            int32_t  exitc = 0;
            if (p && t.payload_len >= 8) {
                memcpy(&pid,   p + 0, 4);
                memcpy(&exitc, p + 4, 4);
            }
            kdbg_print("[LIFECYCLE] saw die  pid=%u exit=%d", pid, exitc);
            if ((int)pid == child) saw_die = 1;
        }
    }

    touch_release(TOUCH_TAG_PROCESS_SPAWNED);
    touch_release(TOUCH_TAG_PROCESS_DIED);

    if (saw_spawn && saw_die) {
        kdbg_print("[LIFECYCLE] PASS: spawn+die delivered for pid=%d", child);
        return 0;
    }
    kdbg_print("[LIFECYCLE] FAIL: saw_spawn=%d saw_die=%d", saw_spawn, saw_die);
    return 1;
}
