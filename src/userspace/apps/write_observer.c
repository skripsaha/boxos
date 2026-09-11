
#include "box/file.h"
#include "box/touch.h"
#include "box/system.h"
#include "box/core/cabin.h"
#include "box/core/result.h"
#include "box/debug.h"
#include "box/string.h"

#define WO_TAG    "obstest"
#define WO_NAME   "_observer_test"
#define WO_BYTES  256

static int child_role(void)
{
    int fid = create(WO_NAME, WO_TAG);
    if (fid < 0) {
        kdbg_print("[WO child] create rc=%d", fid);
        return 1;
    }
    char buf[WO_BYTES];
    for (uint32_t i = 0; i < WO_BYTES; i++) buf[i] = (char)(i & 0xFF);
    int wrc = fwrite((uint32_t)fid, 0, buf, WO_BYTES);
    if (wrc != WO_BYTES) {
        kdbg_print("[WO child] fwrite rc=%d", wrc);
        delete((uint32_t)fid);
        return 1;
    }
    delete((uint32_t)fid);
    kdbg_print("[WO child] wrote %d bytes & exited", WO_BYTES);
    return 0;
}

int main(void)
{
    CabinInfo *ci = cabin_info();
    if (ci && ci->spawner_pid != 0 && ci->spawner_pid != 2) {
        return child_role();
    }

    TouchTag tag = TOUCH_TAG_ID(WO_TAG);
    if (touch_claim(tag, TOUCH_REST, 0, 0) != 0) {
        kdbg_print("[WO] claim '%s' FAIL", WO_TAG);
        return 1;
    }

    int kid = proc_exec("write_observer");
    if (kid < 0) {
        kdbg_print("[WO] spawn child FAIL rc=%d", kid);
        touch_release(tag);
        return 1;
    }
    kdbg_print("[WO] spawned child pid=%d, awaiting WROTE", kid);

    Touch t;
    int rc = touch_await(tag, &t, 5000);
    touch_release(tag);

    if (rc != 0) {
        kdbg_print("[WO] FAIL: no Touch within 5 s (rc=%d)", rc);
        return 1;
    }

    const uint8_t *p = t.payload;
    if (t.payload_len < 12) {
        kdbg_print("[WO] FAIL: payload too small (%u)", (unsigned)t.payload_len);
        return 1;
    }
    uint32_t got_fid = 0;
    uint8_t  got_op  = 0;
    uint64_t got_bytes = 0;
    memcpy(&got_fid,   p + 0, 4);
    memcpy(&got_op,    p + 4, 1);
    if (t.payload_len >= 24) {
        memcpy(&got_bytes, p + 16, 8);
    }

    kdbg_print("[WO] received WROTE fid=%u op=%u bytes=%lu",
               got_fid, got_op, (unsigned long)got_bytes);

    if (got_op != 1) {
        kdbg_print("[WO] FAIL: op != 1 (write)");
        return 1;
    }
    if (got_bytes != WO_BYTES) {
        kdbg_print("[WO] FAIL: bytes %lu != expected %d",
                   (unsigned long)got_bytes, WO_BYTES);
        return 1;
    }

    kdbg_print("[WO] PASS — Touch WROTE delivered with correct payload");
    return 0;
}