#include "box/touch.h"
#include "box/debug.h"
#include "box/ipc.h"
#include "box/system.h"
#include "box/time.h"
#include "box/core/cabin.h"
#include "box/core/result.h"
#include "box/string.h"
#include "box/error.h"

#define TAG_FLOOD   "stress:flood"
#define TAG_KV      "stress:kv"

#define ROLE_KV_PRODUCER    1
#define ROLE_FLOOD_LISTENER 2

#define ROLE_MAGIC 0xBB

static int spawn_role(uint8_t role)
{
    int child = proc_exec("touch_stress");
    if (child < 0) return child;
    uint8_t pkt[2] = { ROLE_MAGIC, role };
    send((uint32_t)child, pkt, 2);
    return child;
}

/* Cached last-good uptime so transient time_uptime_ms failures (e.g. under
 * heavy K-Core load when the HW_TIMER_GET_MS reply hits ring backpressure)
 * don't underflow elapsed_ms = uptime_ms() - t0. Returns last-good on error
 * which yields elapsed=0 instead of a 64-bit underflow. */
static uint64_t s_last_uptime_ms = 0;

static uint64_t uptime_ms(void)
{
    uint64_t ms = 0;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (time_uptime_ms(&ms) == 0) {
            s_last_uptime_ms = ms;
            return ms;
        }
        for (volatile int i = 0; i < 100000; i++) {}
    }
    return s_last_uptime_ms;
}

/* -----------------------------------------------------------------------
 * S1 — Producer-consumer: 4 producers × 1000 sends each → parent counts
 * ----------------------------------------------------------------------- */

#define KV_ITERS     1000
#define KV_PRODUCERS 4

static void role_kv_producer(uint32_t parent_pid)
{
    (void)parent_pid;
    for (uint32_t i = 0; i < KV_ITERS; i++) {
        touch_send(TAG_KV, &i, sizeof(i), 0);
    }
    exit(0);
}

static void test_s1(void)
{
    touch_claim(TAG_KV, TOUCH_REST, 0, 0);

    uint64_t t0 = uptime_ms();

    int children[KV_PRODUCERS];
    for (int i = 0; i < KV_PRODUCERS; i++) {
        children[i] = spawn_role(ROLE_KV_PRODUCER);
        if (children[i] < 0) {
            kdbg_print("[STRESS S1] FAIL: spawn failed i=%d", i);
            touch_release(TAG_KV);
            return;
        }
    }

    uint32_t count    = 0;
    uint32_t expected = KV_PRODUCERS * KV_ITERS;

    /* High per-call timeout to tolerate transient ring stalls under
     * heavy MPSC contention. The test as a whole still bounded by the
     * outer 30s default. */
    while (count < expected) {
        Touch t;
        int rc = touch_await(TAG_KV, &t, 30000);
        if (rc != 0) break;
        count++;
    }

    touch_release(TAG_KV);
    uint64_t elapsed = uptime_ms() - t0;

    if (count == expected)
        kdbg_print("[STRESS S1] producer-consumer: expected=%u actual=%u elapsed=%lu ms PASS",
                   expected, count, (unsigned long)elapsed);
    else
        kdbg_print("[STRESS S1] producer-consumer: expected=%u actual=%u FAIL",
                   expected, count);
}

/* -----------------------------------------------------------------------
 * S2 — Tag churn: claim + release 10000 times on different tag names
 * ----------------------------------------------------------------------- */

static void int_to_str(int v, char *buf, int *len)
{
    char tmp[12]; int n = 0;
    if (v == 0) { tmp[n++] = '0'; }
    else { while (v > 0) { tmp[n++] = (char)('0' + v % 10); v /= 10; } }
    for (int i = n - 1; i >= 0; i--) buf[(*len)++] = tmp[i];
}

static void test_s2(void)
{
    uint64_t t0 = uptime_ms();
    bool crashed = false;

    for (int i = 0; i < 10000 && !crashed; i++) {
        char tag[32];
        int p = 0;
        const char *pfx = "s:churn:";
        while (*pfx) tag[p++] = *pfx++;
        int_to_str(i % 200, tag, &p);  /* 200 unique names, cycling */
        tag[p] = '\0';

        int rc = touch_claim(tag, TOUCH_REST, 0, 0);
        if (rc < 0) { crashed = true; break; }
        touch_release(tag);
    }

    /* Final claim must succeed after all churn. */
    int final_rc = touch_claim("s:churn:final", TOUCH_REST, 0, 0);
    if (final_rc == 0) touch_release("s:churn:final");

    uint64_t elapsed = uptime_ms() - t0;
    bool ok = !crashed && (final_rc == 0);

    if (ok)
        kdbg_print("[STRESS S2] tag-churn 10000x: elapsed=%lu ms PASS", (unsigned long)elapsed);
    else
        kdbg_print("[STRESS S2] tag-churn 10000x: FAIL crashed=%d final_rc=%d", (int)crashed, final_rc);
}

/* -----------------------------------------------------------------------
 * S3 — Multi-listener flood: 4 listeners, 1 sender × 500 sends
 * ----------------------------------------------------------------------- */

#define FLOOD_LISTENERS 4
#define FLOOD_SENDS     500

static void role_flood_listener(uint32_t parent_pid)
{
    touch_claim(TAG_FLOOD, TOUCH_REST, 0, 0);
    uint8_t ready = 1;
    send(parent_pid, &ready, 1);

    uint32_t count = 0;
    while (count < FLOOD_SENDS) {
        Touch t;
        int rc = touch_await(TAG_FLOOD, &t, 8000);
        if (rc != 0) break;
        count++;
    }

    touch_release(TAG_FLOOD);
    send(parent_pid, &count, sizeof(count));
    exit(0);
}

static void test_s3(void)
{
    /* Drain leftover. */
    { Result drain; while (receive_wait(&drain, 50)) { } }

    uint64_t t0 = uptime_ms();

    int children[FLOOD_LISTENERS];
    for (int i = 0; i < FLOOD_LISTENERS; i++) {
        children[i] = spawn_role(ROLE_FLOOD_LISTENER);
        if (children[i] < 0) {
            kdbg_print("[STRESS S3] FAIL: spawn failed i=%d", i);
            return;
        }
    }

    /* Wait for ready from each specific child (filter by sender_pid). */
    Result r;
    int ready_count = 0;
    bool ready[FLOOD_LISTENERS] = { false };
    for (int t = 0; t < 60 && ready_count < FLOOD_LISTENERS; t++) {
        if (receive_wait(&r, 200)) {
            for (int i = 0; i < FLOOD_LISTENERS; i++) {
                if (!ready[i] && r.sender_pid == (uint32_t)children[i]) {
                    ready[i] = true; ready_count++; break;
                }
            }
        }
    }
    if (ready_count < FLOOD_LISTENERS) {
        kdbg_print("[STRESS S3] FAIL: only %d/%d listeners ready",
                   ready_count, FLOOD_LISTENERS);
        return;
    }

    /* Small drain pause so all 4 listeners have entered touch_await. */
    for (volatile int i = 0; i < 500000; i++) {}

    for (int i = 0; i < FLOOD_SENDS; i++) {
        touch_send(TAG_FLOOD, &i, sizeof(i), 0);
    }

    /* Wait for count from each specific child (filter by sender_pid). */
    uint32_t counts[FLOOD_LISTENERS] = {0};
    int counts_received = 0;
    for (int t = 0; t < 100 && counts_received < FLOOD_LISTENERS; t++) {
        if (receive_wait(&r, 200)) {
            for (int i = 0; i < FLOOD_LISTENERS; i++) {
                if (r.sender_pid == (uint32_t)children[i] &&
                    r.data_length >= 4 && r.data_addr != 0) {
                    memcpy(&counts[i], (const void *)(uintptr_t)r.data_addr, 4);
                    counts_received++;
                    break;
                }
            }
        }
    }

    uint64_t elapsed = uptime_ms() - t0;
    bool all_ok = true;
    for (int i = 0; i < FLOOD_LISTENERS; i++) {
        if (counts[i] != FLOOD_SENDS) all_ok = false;
    }

    if (all_ok) {
        kdbg_print("[STRESS S3] flood L0=%u L1=%u L2=%u L3=%u elapsed=%lu ms PASS",
                   counts[0], counts[1], counts[2], counts[3], (unsigned long)elapsed);
    } else {
        kdbg_print("[STRESS S3] flood L0=%u L1=%u L2=%u L3=%u FAIL",
                   counts[0], counts[1], counts[2], counts[3]);
    }
}

/* ---------- main ---------- */
int main(void)
{
    CabinInfo *ci = cabin_info();

    /* Child detection: a non-zero spawner_pid that is NOT the shell (PID 2)
     * means we were spawned by another touch_stress instance for a role.
     * Wait indefinitely-with-bound for the role packet rather than fall
     * through to running the full test recursively (each fall-through
     * spawns 4 more children, cascading load until the system thrashes).
     *
     * Shell-launched: spawner_pid == 2. Run the full test battery.
     * Test-launched: spawner_pid != 2 (and != 0). MUST find a role; if
     * the role packet never arrives, exit silently. */
    if (ci && ci->spawner_pid != 0 && ci->spawner_pid != 2) {
        Result r;
        bool found_role = false;
        uint8_t role = 0;

        for (int attempt = 0; attempt < 150 && !found_role; attempt++) {
            if (!receive_wait(&r, 200)) continue;
            if (r.data_length >= 2 && r.data_addr != 0) {
                const uint8_t *buf = (const uint8_t *)(uintptr_t)r.data_addr;
                if (buf[0] == ROLE_MAGIC) {
                    role = buf[1];
                    found_role = true;
                }
            }
        }

        if (found_role) {
            switch (role) {
            case ROLE_KV_PRODUCER:    role_kv_producer(ci->spawner_pid);    break;
            case ROLE_FLOOD_LISTENER: role_flood_listener(ci->spawner_pid); break;
            default: break;
            }
        }
        /* Either the role completed or the role packet never arrived. In
         * both cases exit — never recurse into the parent test. */
        exit(0);
    }

    kdbg_print("[STRESS] Touch Stress Tests starting");

    test_s1();
    test_s2();
    test_s3();

    kdbg_print("[STRESS] Done");
    return 0;
}
