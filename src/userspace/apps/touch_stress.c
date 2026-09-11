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

static int spawn_role(uint8_t role, uint32_t *out_gen)
{
    int child = proc_exec_gen("touch_stress", NULL, out_gen);
    if (child <= 0) return child;
    uint8_t pkt[2] = { ROLE_MAGIC, role };
    send((uint32_t)child, pkt, 2);
    return child;
}

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


#define KV_ITERS     1000
#define KV_PRODUCERS 4

static void role_kv_producer(uint32_t parent_pid)
{
    (void)parent_pid;
    TouchTagPair pair = TOUCH_TAG_PAIR(TAG_KV);
    for (uint32_t i = 0; i < KV_ITERS; i++) {
        touch_send(pair, &i, sizeof(i), 0);
    }
    exit(0);
}

static void test_s1(void)
{
    TouchTag tag = TOUCH_TAG_ID(TAG_KV);
    touch_claim(tag, TOUCH_REST, 0, 0);

    uint64_t t0 = uptime_ms();

    int children[KV_PRODUCERS];
    for (int i = 0; i < KV_PRODUCERS; i++) {
        children[i] = spawn_role(ROLE_KV_PRODUCER, NULL);
        if (children[i] <= 0) {
            kdbg_print("[STRESS S1] FAIL: spawn failed i=%d", i);
            touch_release(tag);
            return;
        }
    }

    uint32_t count    = 0;
    uint32_t expected = KV_PRODUCERS * KV_ITERS;

    while (count < expected) {
        Touch t;
        int rc = touch_await(tag, &t, 30000);
        if (rc != 0) break;
        count++;
    }

    touch_release(tag);
    uint64_t elapsed = uptime_ms() - t0;

    if (count == expected)
        kdbg_print("[STRESS S1] producer-consumer: expected=%u actual=%u elapsed=%lu ms PASS",
                   expected, count, (unsigned long)elapsed);
    else
        kdbg_print("[STRESS S1] producer-consumer: expected=%u actual=%u FAIL",
                   expected, count);
}


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
        char tag_str[32];
        int p = 0;
        const char *pfx = "s:churn:";
        while (*pfx) tag_str[p++] = *pfx++;
        int_to_str(i % 200, tag_str, &p);
        tag_str[p] = '\0';

        TouchTagPair pair = touch_intern(tag_str);
        TouchTag tid = touch_pair_choose(pair);
        if (tid == TOUCH_TAG_INVALID) { crashed = true; break; }

        int rc = touch_claim(tid, TOUCH_REST, 0, 0);
        if (rc != 0) { crashed = true; break; }
        int rc2 = touch_release(tid);
        if (rc2 != 0) { crashed = true; break; }
    }

    int final_rc = -1;
    TouchTag final_tid = TOUCH_TAG_ID("s:churn:final");
    if (final_tid != TOUCH_TAG_INVALID) {
        final_rc = touch_claim(final_tid, TOUCH_REST, 0, 0);
        if (final_rc == 0) touch_release(final_tid);
    }

    uint64_t elapsed = uptime_ms() - t0;
    bool ok = !crashed && (final_rc == 0);

    if (ok)
        kdbg_print("[STRESS S2] tag-churn 10000x: elapsed=%lu ms PASS", (unsigned long)elapsed);
    else
        kdbg_print("[STRESS S2] tag-churn 10000x: FAIL crashed=%d final_rc=%d", (int)crashed, final_rc);
}


#define FLOOD_LISTENERS 4
#define FLOOD_SENDS     500

static void role_flood_listener(uint32_t parent_pid)
{
    TouchTag tag = TOUCH_TAG_ID(TAG_FLOOD);
    touch_claim(tag, TOUCH_REST, 0, 0);
    uint8_t ready = 1;
    send(parent_pid, &ready, 1);

    uint32_t count = 0;
    while (count < FLOOD_SENDS) {
        Touch t;
        int rc = touch_await(tag, &t, 8000);
        if (rc != 0) break;
        count++;
    }

    touch_release(tag);
    send(parent_pid, &count, sizeof(count));
    exit(0);
}

static void test_s3(void)
{
    { Result drain; while (receive_wait(&drain, 50)) { } }

    uint64_t t0 = uptime_ms();

    int      children[FLOOD_LISTENERS];
    uint32_t gens[FLOOD_LISTENERS];
    for (int i = 0; i < FLOOD_LISTENERS; i++) {
        children[i] = spawn_role(ROLE_FLOOD_LISTENER, &gens[i]);
        if (children[i] <= 0) {
            kdbg_print("[STRESS S3] FAIL: spawn failed i=%d", i);
            return;
        }
    }

    Result r;
    int ready_count = 0;
    bool ready[FLOOD_LISTENERS] = { false };
    while (ready_count < FLOOD_LISTENERS) {
        (void)receive_wait(&r, 0);
        for (int i = 0; i < FLOOD_LISTENERS; i++) {
            if (!ready[i] && r.sender_pid == (uint32_t)children[i]) {
                ready[i] = true; ready_count++; break;
            }
        }
    }

    for (volatile int i = 0; i < 500000; i++) {}

    TouchTagPair pair = TOUCH_TAG_PAIR(TAG_FLOOD);
    for (int i = 0; i < FLOOD_SENDS; i++) {
        touch_send(pair, &i, sizeof(i), 0);
    }

    for (int i = 0; i < FLOOD_LISTENERS; i++)
        (void)process_gone((uint32_t)children[i], gens[i], NULL);

    uint32_t counts[FLOOD_LISTENERS] = {0};
    while (receive(&r)) {
        for (int i = 0; i < FLOOD_LISTENERS; i++) {
            if (r.sender_pid == (uint32_t)children[i] &&
                r.data_length >= 4 && r.data_addr != 0) {
                memcpy(&counts[i], (const void *)(uintptr_t)r.data_addr, 4);
                break;
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

int main(void)
{
    CabinInfo *ci = cabin_info();

    if (ci && ci->spawner_pid != 0 && ci->spawner_pid != 2) {
        Result r;
        uint8_t role = 0;
        for (;;) {
            (void)receive_wait(&r, 0);
            if (r.data_length >= 2 && r.data_addr != 0) {
                const uint8_t *buf = (const uint8_t *)(uintptr_t)r.data_addr;
                if (buf[0] == ROLE_MAGIC) { role = buf[1]; break; }
            }
        }

        switch (role) {
        case ROLE_KV_PRODUCER:    role_kv_producer(ci->spawner_pid);    break;
        case ROLE_FLOOD_LISTENER: role_flood_listener(ci->spawner_pid); break;
        default: break;
        }
        exit(0);
    }

    kdbg_print("[STRESS] Touch Stress Tests starting");

    test_s1();
    test_s2();
    test_s3();

    kdbg_print("[STRESS] Done");
    return 0;
}