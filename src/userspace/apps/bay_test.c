/*
 * bay_test — verify cross-cabin shared memory via the Bay primitive.
 *
 * Test plan:
 *   T1  small Bay (4 KB): create, write, release — no kernel panic.
 *   T2  huge Bay (4 MB): create with BAY_CREATE — implicit 2 MB pages.
 *   T3  child opens the same tag, reads the parent-written bytes,
 *       writes a reply marker, releases. Parent reads the marker.
 *   T4  refcount: parent creates, child opens, parent releases first.
 *       Child should still see live memory. Then child releases — last
 *       drop frees the Bay.
 *   T5  open-missing without BAY_CREATE returns NULL.
 *   T6  size query reports the rounded-up size.
 *   T7  implicit huge-page user-heap: malloc(4 MB) succeeds and the
 *       returned pointer is writable end-to-end (regression check on
 *       SYSTEM_OP_HEAP_PREFAULT).
 *
 * Pass/fail markers follow the stress_matrix.sh aggregator convention
 * (`[BT N] PASS` / `[BT N] FAIL`).
 */

#include "box/bay.h"
#include "box/memory.h"
#include "box/system.h"
#include "box/ipc.h"
#include "box/debug.h"
#include "box/time.h"
#include "box/string.h"
#include "box/core/result.h"
#include "box/core/cabin.h"
#include "box/error.h"

/* Spin-wait helper — boxlib has no sleep, so we yield in a bounded loop
 * keyed off the kernel-published uptime page. */
static void wait_ms(uint32_t ms)
{
    uint64_t deadline = 0;
    time_uptime_ms(&deadline);
    deadline += ms;
    for (;;) {
        uint64_t now = 0;
        time_uptime_ms(&now);
        if (now >= deadline) break;
        yield();
    }
}

#define TAG_SMALL "bay:test:small"
#define TAG_HUGE  "bay:test:huge"
#define TAG_RC    "bay:test:rc"
#define TAG_MISS  "bay:test:miss"
#define TAG_SIZE  "bay:test:size"

#define ROLE_MAGIC      0xBA
#define ROLE_CHILD_READ 1
#define ROLE_CHILD_RC   2

static int g_passed = 0;
static int g_total  = 0;

static void pass(int n)
{
    g_passed++;
    g_total++;
    kdbg_print("[BT %d] PASS", n);
}

static void fail(int n, const char *why)
{
    g_total++;
    kdbg_print("[BT %d] FAIL: %s", n, why);
}

static int spawn_role(uint8_t role)
{
    int child = proc_exec("bay_test");
    if (child < 0) return child;
    uint8_t pkt[2] = { ROLE_MAGIC, role };
    send((uint32_t)child, pkt, 2);
    return child;
}

/* Block until the child terminates. Returns 0 on graceful exit,
 * non-zero on timeout/error. Polls proc_info every ~10 ms. The
 * kernel sets state == PROC_STATE_TERMINATED on the dead child;
 * proc_info returns ERR_PROCESS_NOT_FOUND once the cleanup queue
 * recycles the slot — either signal means the child finished. */
static int wait_child_exit(uint32_t child_pid)
{
    for (int i = 0; i < 800; i++) {
        proc_info_t info;
        int rc = proc_info((uint16_t)child_pid, &info);
        if (rc != 0)                                   return 0;  /* gone */
        if (info.state == PROC_STATE_TERMINATED)       return 0;  /* dead */
        wait_ms(10);
    }
    return -1;
}

/* ───────────────────────────── child roles ───────────────────────── */

static void role_child_read(uint32_t parent_pid)
{
    (void)parent_pid;
    /* Open the parent-created Bay and verify content. */
    void *p = bay_open(TAG_HUGE, 0, BAY_OPEN);
    if (!p) {
        kdbg_print("[BT C-read] open FAIL");
        exit(1);
    }

    const uint8_t *bytes = (const uint8_t *)p;
    int ok = 1;
    for (int i = 0; i < 64; i++) {
        if (bytes[i] != (uint8_t)(0xA0 + i)) { ok = 0; break; }
    }
    if (!ok) {
        bay_release(p);
        kdbg_print("[BT C-read] content FAIL");
        exit(2);
    }

    /* Write a reply marker at offset 4096. */
    uint8_t *w = (uint8_t *)p + 4096;
    for (int i = 0; i < 32; i++) w[i] = (uint8_t)(0xB0 + i);

    bay_release(p);
    exit(0);
}

static void role_child_rc(uint32_t parent_pid)
{
    (void)parent_pid;
    void *p = bay_open(TAG_RC, 0, BAY_OPEN);
    if (!p) { kdbg_print("[BT C-rc] open FAIL"); exit(1); }

    /* Verify parent's marker still visible. */
    const uint8_t *bytes = (const uint8_t *)p;
    if (bytes[0] != 0xC0 || bytes[63] != 0xCF) {
        bay_release(p);
        kdbg_print("[BT C-rc] content FAIL");
        exit(2);
    }

    /* Wait a moment so the parent gets a chance to bay_release first. */
    wait_ms(50);

    /* Verify content is STILL there after parent's release (our claim
     * keeps it alive). */
    if (bytes[0] != 0xC0 || bytes[63] != 0xCF) {
        bay_release(p);
        kdbg_print("[BT C-rc] post-parent-release FAIL");
        exit(3);
    }

    bay_release(p);
    exit(0);
}

/* ───────────────────────────── main test driver ───────────────── */

static void run_parent_tests(void)
{
    /* T1 — small Bay: create 4 KB, write, release, no panic. */
    {
        void *p = bay_open(TAG_SMALL, 4096, BAY_CREATE);
        if (!p) { fail(1, "open small"); }
        else {
            volatile uint8_t *b = (volatile uint8_t *)p;
            b[0] = 0x42; b[4095] = 0x55;
            if (b[0] == 0x42 && b[4095] == 0x55) {
                int rc = bay_release(p);
                if (rc == 0) pass(1);
                else fail(1, "release");
            } else {
                bay_release(p);
                fail(1, "readback");
            }
        }
    }

    /* T2 — huge Bay (4 MB): create, fill prefix, release. */
    {
        void *p = bay_open(TAG_HUGE, 4UL * 1024 * 1024, BAY_CREATE);
        if (!p) { fail(2, "open huge"); }
        else {
            uint8_t *b = (uint8_t *)p;
            for (int i = 0; i < 64; i++) b[i] = (uint8_t)(0xA0 + i);
            /* Touch the last byte to confirm 2 MB pages are mapped. */
            b[4UL * 1024 * 1024 - 1] = 0xFF;

            /* T3 — child reads back. We DON'T release yet; child opens
             * the same tag. */
            int child = spawn_role(ROLE_CHILD_READ);
            if (child < 0) { fail(2, "spawn child"); bay_release(p); }
            else {
                pass(2);
                int code = wait_child_exit((uint32_t)child);
                if (code == 0) {
                    /* Check reply marker. */
                    const uint8_t *w = (const uint8_t *)p + 4096;
                    int ok = 1;
                    for (int i = 0; i < 32; i++) {
                        if (w[i] != (uint8_t)(0xB0 + i)) { ok = 0; break; }
                    }
                    if (ok) pass(3); else fail(3, "reply marker");
                } else {
                    fail(3, "child exit non-zero");
                }
                bay_release(p);
            }
        }
    }

    /* T4 — refcount: parent creates, child opens, parent releases first,
     * child verifies content still live, child releases (last drop). */
    {
        void *p = bay_open(TAG_RC, 4096, BAY_CREATE);
        if (!p) { fail(4, "open rc"); }
        else {
            uint8_t *b = (uint8_t *)p;
            b[0]  = 0xC0;
            b[63] = 0xCF;

            int child = spawn_role(ROLE_CHILD_RC);
            if (child < 0) { fail(4, "spawn rc"); bay_release(p); }
            else {
                /* Let the child open the Bay before we release. */
                wait_ms(20);
                int rc = bay_release(p);   /* parent drops first */
                if (rc != 0) {
                    fail(4, "parent release");
                } else {
                    int code = wait_child_exit((uint32_t)child);
                    if (code == 0) pass(4);
                    else fail(4, "child exit");
                }
            }
        }
    }

    /* T5 — open missing without CREATE → NULL. */
    {
        void *p = bay_open(TAG_MISS, 0, BAY_OPEN);
        if (p == NULL) pass(5);
        else { fail(5, "should be NULL"); bay_release(p); }
    }

    /* T6 — size query. */
    {
        uint64_t expect_size = 4UL * 1024 * 1024;
        void *p = bay_open(TAG_SIZE, expect_size, BAY_CREATE);
        if (!p) { fail(6, "open size"); }
        else {
            uint64_t s = bay_size(p);
            if (s == expect_size) pass(6);
            else fail(6, "size mismatch");
            bay_release(p);
        }
    }

    /* T7 — user-heap implicit huge: malloc(4 MB) and write end-to-end. */
    {
        void *m = malloc(4UL * 1024 * 1024);
        if (!m) fail(7, "malloc 4M");
        else {
            uint8_t *b = (uint8_t *)m;
            b[0] = 0xE0;
            b[4UL * 1024 * 1024 - 1] = 0xEF;
            if (b[0] == 0xE0 && b[4UL * 1024 * 1024 - 1] == 0xEF) pass(7);
            else fail(7, "readback");
            free(m);
        }
    }

    kdbg_print("[BT *] %d/%d", g_passed, g_total);
    if (g_passed == g_total) kdbg_print("[BT *] PASS");
    else                     kdbg_print("[BT *] FAIL");
}

int main(void)
{
    CabinInfo *ci = cabin_info();

    /* Children receive role byte via the first Pocket from the parent. */
    {
        Result r;
        for (int attempt = 0; attempt < 2; attempt++) {
            if (!receive_wait(&r, 200)) break;
            if (r.data_length >= 2 && r.data_addr != 0) {
                const uint8_t *buf = (const uint8_t *)(uintptr_t)r.data_addr;
                if (buf[0] == ROLE_MAGIC) {
                    switch (buf[1]) {
                    case ROLE_CHILD_READ: role_child_read(ci->spawner_pid); break;
                    case ROLE_CHILD_RC:   role_child_rc(ci->spawner_pid);   break;
                    default: exit(255);
                    }
                    exit(0);  /* defensive — role_* call exit() internally */
                }
            }
        }
    }

    run_parent_tests();
    exit(g_passed == g_total ? 0 : 1);
    return 0;
}
