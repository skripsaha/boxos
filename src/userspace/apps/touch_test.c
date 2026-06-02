#include "box/touch.h"
#include "box/debug.h"
#include "box/ipc.h"
#include "box/system.h"
#include "box/time.h"
#include "box/core/cabin.h"
#include "box/core/result.h"
#include "box/string.h"
#include "box/error.h"

#define TAG_PING    "test:ping"
#define TAG_BCAST   "test:bcast"
#define TAG_TIMER   "test:timer"
#define TAG_PDIED   "process:died"
#define TAG_TIMEOUT "test:timeout"
#define TAG_DELAYED "test:delayed"
#define TAG_LEVEL   "test:level"
#define TAG_OWNERS  "test:owners"
#define TAG_REACT   "test:react"

#define ROLE_PING_SENDER    1
#define ROLE_BCAST_LISTEN   2
#define ROLE_BCAST_SEND     3
#define ROLE_DIE_CHILD      4
#define ROLE_LEVEL_CLAIM    5
#define ROLE_NO_TAG_SEND    6
#define ROLE_HAS_TAG_SEND   7
#define ROLE_REACT_SEND     8

static int g_passed = 0;
static int g_total  = 0;

static uint64_t uptime_ms(void)
{
    uint64_t ms = 0;
    time_uptime_ms(&ms);
    return ms;
}

static void pass(int n)
{
    g_passed++;
    g_total++;
    kdbg_print("[TT %d] PASS", n);
}

static void fail(int n, const char *reason)
{
    g_total++;
    kdbg_print("[TT %d] FAIL: %s", n, reason);
}

#define ROLE_MAGIC 0xBB

static int spawn_role(uint8_t role)
{
    int child = proc_exec("touch_test");
    if (child < 0) return child;
    uint8_t pkt[2] = { ROLE_MAGIC, role };
    send((uint32_t)child, pkt, 2);
    return child;
}

/* ---------- child roles ---------- */

static void role_ping_sender(uint32_t parent_pid)
{
    (void)parent_pid;
    for (volatile int i = 0; i < 80000; i++) {}
    touch_send(TOUCH_TAG_PAIR(TAG_PING), "hello", 5, 0);
    exit(0);
}

static void role_bcast_listen(uint32_t parent_pid)
{
    /* Subscribe. After touch_claim returns OK the kernel has linked us
     * into the bucket — publishes to TAG_BCAST will land in our result
     * ring even if we haven't entered touch_await yet. */
    TouchTag tag = TOUCH_TAG_ID(TAG_BCAST);
    int claim_rc = touch_claim(tag, TOUCH_REST, 0, 0);

    /* Signal subscription readiness to parent. claim_rc encodes whether
     * subscription succeeded; parent treats nonzero as a listener error. */
    uint8_t ready = (claim_rc == 0) ? 1 : 0;
    send(parent_pid, &ready, 1);

    /* 30 s — TCG-tolerant; the actual publish/delivery cycle is µs on
     * real HW, and even heavily contended UEFI 4c TCG completes in <1 s. */
    Touch t;
    int rc = touch_await(tag, &t, 30000);
    uint8_t ok = (rc == 0) ? 1 : 0;
    send(parent_pid, &ok, 1);
    touch_release(tag);
    exit(0);
}

static void role_bcast_send(uint32_t parent_pid)
{
    /* Wait for explicit "go" from parent instead of a timing heuristic.
     * Parent sends GO only after BOTH listeners have signaled ready
     * (touch_claim has returned OK in each), which guarantees both subs
     * are linked into the bucket. The broadcast then deterministically
     * fans out to both. */
    Result r;
    for (int t = 0; t < 50; t++) {
        if (receive_wait(&r, 200)) {
            if (r.sender_pid == parent_pid &&
                r.data_length >= 1 && r.data_addr != 0 &&
                *(const uint8_t *)(uintptr_t)r.data_addr == 0xC0) {
                touch_send(TOUCH_TAG_PAIR(TAG_BCAST), "bcast", 5, 0);
                break;
            }
        }
    }
    exit(0);
}

static void role_die_child(uint32_t parent_pid)
{
    (void)parent_pid;
    exit(0);
}

static void role_level_claim(uint32_t parent_pid)
{
    TouchTag tag = TOUCH_TAG_ID(TAG_LEVEL);
    touch_claim(tag, TOUCH_REST, 0, 0);
    Touch t;
    int rc = touch_await(tag, &t, 500);
    uint8_t got = (rc == 0) ? 1 : 0;
    send(parent_pid, &got, 1);
    touch_release(tag);
    exit(0);
}

static void role_no_tag_send(uint32_t parent_pid)
{
    int rc = touch_send(TOUCH_TAG_PAIR(TAG_OWNERS), "x", 1, 0);
    uint8_t denied = (rc != 0) ? 1 : 0;
    send(parent_pid, &denied, 1);
    exit(0);
}

static void role_has_tag_send(uint32_t parent_pid)
{
    int rc = touch_send(TOUCH_TAG_PAIR(TAG_OWNERS), "y", 1, 0);
    uint8_t ok = (rc >= 0) ? 1 : 0;
    send(parent_pid, &ok, 1);
    exit(0);
}

static void role_react_send(uint32_t parent_pid)
{
    (void)parent_pid;
    for (volatile int i = 0; i < 80000; i++) {}
    touch_send(TOUCH_TAG_PAIR(TAG_REACT), "data", 4, 0);
    exit(0);
}

/* ---------- T1: REST round-trip ---------- */
static void test1(void)
{
    TouchTag tag = TOUCH_TAG_ID(TAG_PING);
    touch_claim(tag, TOUCH_REST, 0, 0);

    int child = spawn_role(ROLE_PING_SENDER);
    if (child < 0) { fail(1, "spawn failed"); touch_release(tag); return; }

    Touch t;
    int rc = touch_await(tag, &t, 3000);
    if (rc != 0) { fail(1, "await timed out"); touch_release(tag); return; }

    bool ok = (t.payload_len >= 5);
    if (ok) {
        const char *p = (const char *)t.payload;
        ok = (p[0]=='h' && p[1]=='e' && p[2]=='l' && p[3]=='l' && p[4]=='o');
    }
    if (ok) pass(1); else fail(1, "payload mismatch");
    touch_release(tag);
}

/* ---------- T2: Multicast ---------- */
static void test2(void)
{
    { Result drain; while (receive_wait(&drain, 50)) { } }

    int c1 = spawn_role(ROLE_BCAST_LISTEN);
    if (c1 < 0) { fail(2, "spawn c1 failed"); return; }
    int c2 = spawn_role(ROLE_BCAST_LISTEN);
    if (c2 < 0) { fail(2, "spawn c2 failed"); return; }

    bool ready_c1 = false, ready_c2 = false;
    Result r;
    for (int t = 0; t < 30 && !(ready_c1 && ready_c2); t++) {
        if (receive_wait(&r, 200)) {
            if (r.sender_pid == (uint32_t)c1) ready_c1 = true;
            else if (r.sender_pid == (uint32_t)c2) ready_c2 = true;
        }
    }
    if (!ready_c1 || !ready_c2) { fail(2, "listeners not ready"); return; }

    int sender = spawn_role(ROLE_BCAST_SEND);
    if (sender < 0) { fail(2, "spawn sender failed"); return; }

    /* Listeners are subscribed (both ready=1 above). Tell sender to fire.
     * No timing assumption — sender broadcasts as soon as it receives this. */
    uint8_t go = 0xC0;
    send((uint32_t)sender, &go, 1);

    bool ok_c1 = false, ok_c2 = false;
    uint8_t va = 0, vb = 0;
    /* 60×500ms = 30s — matches the listener's own touch_await timeout.
     * Capture ONLY the first packet from each child. After `send(ok)` the
     * child runs `exit()` which posts a [0xFE, exit_code] sentinel to its
     * spawner — overwriting va/vb on a slow ok-loop iteration would convert
     * the legitimate ok=1 into the unrelated 0xFE byte. The first packet is
     * the one we care about. */
    for (int t = 0; t < 60 && !(ok_c1 && ok_c2); t++) {
        if (receive_wait(&r, 500)) {
            if (r.data_length < 1 || r.data_addr == 0) continue;
            uint8_t v = *(uint8_t *)(uintptr_t)r.data_addr;
            if (r.sender_pid == (uint32_t)c1 && !ok_c1) { ok_c1 = true; va = v; }
            else if (r.sender_pid == (uint32_t)c2 && !ok_c2) { ok_c2 = true; vb = v; }
        }
    }
    bool both_ok = ok_c1 && ok_c2 && va == 1 && vb == 1;
    if (both_ok) pass(2);
    else {
        kdbg_print("[TT 2] FAIL: ok_c1=%d va=%d ok_c2=%d vb=%d",
                   (int)ok_c1, (int)va, (int)ok_c2, (int)vb);
        g_total++;
    }
}

/* ---------- T3: Self-touch immediate ---------- */
static void test3(void)
{
    TouchTag tag = TOUCH_TAG_ID(TAG_TIMER);
    touch_claim(tag, TOUCH_REST, 0, 0);
    touch_send(TOUCH_TAG_PAIR(TAG_TIMER), NULL, 0, 0);

    Touch t;
    int rc = touch_await(tag, &t, 1000);
    if (rc == 0) pass(3); else fail(3, "await failed");
    touch_release(tag);
}

/* ---------- T4: process.died ---------- */
static void test4(void)
{
    TouchTag tag = TOUCH_TAG_ID(TAG_PDIED);
    touch_claim(tag, TOUCH_REST, 0, 0);

    int child = spawn_role(ROLE_DIE_CHILD);
    if (child < 0) { fail(4, "spawn failed"); touch_release(tag); return; }

    Touch t;
    int rc = touch_await(tag, &t, 3000);
    if (rc != 0) { fail(4, "await timed out"); touch_release(tag); return; }

    bool pid_ok = false;
    if (t.payload_len >= 4) {
        uint32_t pid = 0;
        memcpy(&pid, t.payload, 4);
        pid_ok = (pid == (uint32_t)child);
    }
    if (pid_ok) pass(4); else fail(4, "wrong pid in payload");
    touch_release(tag);
}

/* ---------- T5: Timeout then data ---------- */
static void test5(void)
{
    TouchTag tag = TOUCH_TAG_ID(TAG_TIMEOUT);
    touch_claim(tag, TOUCH_REST, 0, 0);

    Touch t;
    touch_await(tag, &t, 200);

    touch_send(TOUCH_TAG_PAIR(TAG_TIMEOUT), "wake", 4, 0);
    int rc2 = touch_await(tag, &t, 2000);
    bool woke = (rc2 == 0 && t.payload_len >= 4);

    if (woke) pass(5); else fail(5, "did not wake on data");
    touch_release(tag);
}

/* ---------- T6: Self-touch delayed (after_ms=300) ---------- */
static void test6(void)
{
    TouchTag tag = TOUCH_TAG_ID(TAG_DELAYED);
    touch_claim(tag, TOUCH_REST, 0, 0);

    uint64_t before = uptime_ms();
    touch_send(TOUCH_TAG_PAIR(TAG_DELAYED), "d", 1, 300);

    Touch t;
    int rc = touch_await(tag, &t, 2000);
    uint64_t after = uptime_ms();

    if (rc != 0) { fail(6, "await failed"); touch_release(tag); return; }

    uint64_t elapsed = after - before;
    bool timing_ok = (elapsed >= 200 && elapsed <= 600);
    if (timing_ok) pass(6); else fail(6, "timing out of range");
    touch_release(tag);
}

/* ---------- T7: LEVEL policy ---------- */
static void test7(void)
{
    { Result drain; while (receive_wait(&drain, 50)) { } }

    TouchTag tag = TOUCH_TAG_ID(TAG_LEVEL);
    touch_register(tag, TOUCH_POLICY_LEVEL, TOUCH_CAP_OPEN);

    uint8_t on = 1;
    touch_send(TOUCH_TAG_PAIR(TAG_LEVEL), &on, 1, 0);

    int child = spawn_role(ROLE_LEVEL_CLAIM);
    if (child < 0) { fail(7, "spawn failed"); return; }

    Result r;
    bool got = false;
    for (int t = 0; t < 30; t++) {
        if (receive_wait(&r, 200)) {
            if (r.sender_pid == (uint32_t)child) { got = true; break; }
        }
    }
    bool got_touch = got && r.data_length >= 1 && r.data_addr != 0
                     && *(uint8_t *)(uintptr_t)r.data_addr == 1;

    if (!got_touch) { fail(7, "level claim did not get immediate touch"); return; }

    uint8_t off = 0;
    touch_send(TOUCH_TAG_PAIR(TAG_LEVEL), &off, 1, 0);

    int child2 = spawn_role(ROLE_LEVEL_CLAIM);
    if (child2 < 0) { fail(7, "spawn2 failed"); return; }

    bool got2 = false;
    for (int t = 0; t < 15; t++) {
        if (receive_wait(&r, 200)) {
            if (r.sender_pid == (uint32_t)child2) { got2 = true; break; }
        }
    }
    bool no_touch = got2 && r.data_length >= 1 && r.data_addr != 0
                    && *(uint8_t *)(uintptr_t)r.data_addr == 0;

    if (got_touch && no_touch) pass(7); else fail(7, "level clear did not stop touch");
}

/* ---------- T8: OWNERS capability ---------- */
static void test8(void)
{
    {
        Result drain;
        while (receive_wait(&drain, 50)) { /* discard */ }
    }

    TouchTag tag = TOUCH_TAG_ID(TAG_OWNERS);
    touch_register(tag, TOUCH_POLICY_EDGE, TOUCH_CAP_OWNERS);

    int child_no = spawn_role(ROLE_NO_TAG_SEND);
    if (child_no < 0) { fail(8, "spawn no-tag failed"); return; }

    Result r;
    bool got_no = false;
    for (int t = 0; t < 20; t++) {
        if (receive_wait(&r, 200)) {
            if (r.sender_pid == (uint32_t)child_no) { got_no = true; break; }
        }
    }
    bool denied_ok = got_no && r.data_length >= 1 && r.data_addr != 0
                     && *(uint8_t *)(uintptr_t)r.data_addr == 1;

    if (!denied_ok) { fail(8, "expected ACCESS_DENIED, got success"); return; }

    proc_tag_add(TAG_OWNERS);

    int child_has = spawn_role(ROLE_HAS_TAG_SEND);
    if (child_has < 0) { fail(8, "spawn has-tag failed"); return; }

    bool got_has = false;
    for (int t = 0; t < 20; t++) {
        if (receive_wait(&r, 200)) {
            if (r.sender_pid == (uint32_t)child_has) { got_has = true; break; }
        }
    }
    bool send_ok = got_has && r.data_length >= 1 && r.data_addr != 0
                   && *(uint8_t *)(uintptr_t)r.data_addr == 1;

    if (denied_ok && send_ok) pass(8); else fail(8, "tagged send failed");

    proc_tag_remove(TAG_OWNERS);
}

/* ---------- T9: REACT mode — claim + send + no crash ---------- */
static void test9(void)
{
    TouchTag tag = TOUCH_TAG_ID(TAG_REACT);
    int rc_claim = touch_claim(tag, TOUCH_REACT, 0, 0);
    if (rc_claim < 0) { fail(9, "REACT claim rejected"); return; }

    int child = spawn_role(ROLE_REACT_SEND);
    if (child < 0) { fail(9, "spawn failed"); touch_release(tag); return; }

    for (volatile int i = 0; i < 3000000; i++) {}
    touch_release(tag);

    touch_claim(tag, TOUCH_REST, 0, 0);
    touch_send(TOUCH_TAG_PAIR(TAG_REACT), "rx", 2, 0);
    Touch t;
    int rc = touch_await(tag, &t, 1000);
    touch_release(tag);

    if (rc == 0 && t.payload_len >= 2) pass(9);
    else fail(9, "REACT/REST verify failed");
}

/* ---------- T10: Wildcard — claim "key:..." catches any "key:value" send ---------- */
static void test10(void)
{
    { Result drain; while (receive_wait(&drain, 50)) { } }

    /* Wildcard subscription resolves bare_id; sends to specific values
     * publish to BOTH full and bare → wildcard receives. */
    TouchTag wild_tag = TOUCH_TAG_ID("wild:...");
    int rc1 = touch_claim(wild_tag, TOUCH_REST, 0, 0);
    if (rc1 < 0) { fail(10, "wildcard claim rejected"); return; }

    touch_send(TOUCH_TAG_PAIR("wild:alpha"), "a", 1, 0);
    Touch t1;
    int rc2 = touch_await(wild_tag, &t1, 500);
    bool got_alpha = (rc2 == 0);

    touch_send(TOUCH_TAG_PAIR("wild:beta"), "b", 1, 0);
    Touch t2;
    int rc3 = touch_await(wild_tag, &t2, 500);
    bool got_beta = (rc3 == 0);

    touch_release(wild_tag);
    if (got_alpha && got_beta) pass(10);
    else fail(10, "wildcard did not catch values");
}

/* ---------- T11: LATCHED — only first publish queued until ack ---------- */
static void test11(void)
{
    { Result drain; while (receive_wait(&drain, 50)) { } }

    TouchTag tag = TOUCH_TAG_ID("latch:slot");
    int rc_reg = touch_register(tag, TOUCH_POLICY_LATCHED, TOUCH_CAP_OPEN);
    if (rc_reg < 0) { fail(11, "register rejected"); return; }

    touch_claim(tag, TOUCH_REST, 0, 0);

    TouchTagPair pair = TOUCH_TAG_PAIR("latch:slot");
    touch_send(pair, "1", 1, 0);
    touch_send(pair, "2", 1, 0);
    touch_send(pair, "3", 1, 0);

    Touch t;
    int r1 = touch_await(tag, &t, 1000);
    bool got_first = (r1 == 0 && t.payload_len >= 1 && (char)t.payload[0] == '1');

    touch_ack(tag);

    touch_send(pair, "4", 1, 0);
    int r2 = touch_await(tag, &t, 1000);
    bool got_post_ack = (r2 == 0 && t.payload_len >= 1);

    touch_release(tag);
    if (got_first && got_post_ack) pass(11);
    else fail(11, "LATCHED ordering or ack failed");
}

/* ---------- main ---------- */
int main(void)
{
    CabinInfo *ci = cabin_info();

    {
        Result r;
        bool found_role = false;
        uint8_t role = 0;

        for (int attempt = 0; attempt < 2 && !found_role; attempt++) {
            if (!receive_wait(&r, 200)) break;
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
            case ROLE_PING_SENDER:   role_ping_sender(ci->spawner_pid);   break;
            case ROLE_BCAST_LISTEN:  role_bcast_listen(ci->spawner_pid);  break;
            case ROLE_BCAST_SEND:    role_bcast_send(ci->spawner_pid);    break;
            case ROLE_DIE_CHILD:     role_die_child(ci->spawner_pid);     break;
            case ROLE_LEVEL_CLAIM:   role_level_claim(ci->spawner_pid);   break;
            case ROLE_NO_TAG_SEND:   role_no_tag_send(ci->spawner_pid);   break;
            case ROLE_HAS_TAG_SEND:  role_has_tag_send(ci->spawner_pid);  break;
            case ROLE_REACT_SEND:    role_react_send(ci->spawner_pid);    break;
            default: break;
            }
            exit(0);
        }
    }

    kdbg_print("[TT] Touch Integration Tests starting");

    { Result drain; while (receive_wait(&drain, 30)) { } }

    test1();
    test2();
    test3();
    test4();
    test5();
    test6();
    test7();
    test8();
    test9();
    test10();
    test11();

    kdbg_print("[TT SUMMARY] %d/%d passed", g_passed, g_total);
    return 0;
}
