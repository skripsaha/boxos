#include "box/touch.h"
#include "box/debug.h"
#include "box/print.h"
#include "box/ipc.h"
#include "box/system.h"
#include "box/time.h"
#include "box/core/cabin.h"
#include "box/core/result.h"
#include "box/string.h"
#include "box/error.h"
#include "box/core/manifest.h"
#include "boxos_decks.h"
#include "box/timeouts.h"
#include "box/strand.h"
#include "box/core/strand_self.h"
#include "box/sync.h"
#include "proc_exit.h"

#define TAG_PING    "test:ping"
#define TAG_BCAST   "test:bcast"
#define TAG_TIMER   "test:timer"
#define TAG_PDIED   "process:died"
#define TAG_TIMEOUT "test:timeout"
#define TAG_DELAYED "test:delayed"
#define TAG_LEVEL   "test:level"
#define TAG_OWNERS  "test:owners"
#define TAG_REACT   "test:react"
#define TAG_BURST   "test:burst"

#define ROLE_PING_SENDER    1
#define ROLE_BCAST_LISTEN   2
#define ROLE_BCAST_SEND     3
#define ROLE_DIE_CHILD      4
#define ROLE_LEVEL_CLAIM    5
#define ROLE_NO_TAG_SEND    6
#define ROLE_HAS_TAG_SEND   7
#define ROLE_REACT_SEND     8
#define ROLE_DIE_CODE       9
#define ROLE_LOOP_FOREVER   10
#define ROLE_DIE_INDEXED    11
#define ROLE_TAG_REPORT     12

#define EXIT_CLEAN_CODE     0x42

#define CONCURRENT_CHILDREN   8
#define CONCURRENT_BASE_CODE  0x100

#define COLLECT_QUIET_MS      30000u

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

static int spawn_role_code(uint8_t role, uint32_t code)
{
    int child = proc_exec("touch_test");
    if (child < 0) return child;
    uint8_t pkt[6] = { ROLE_MAGIC, role };
    memcpy(pkt + 2, &code, sizeof(code));
    send((uint32_t)child, pkt, sizeof(pkt));
    return child;
}

static int spawn_role_tagged(uint8_t role, const char *tags)
{
    int child = proc_exec_tagged("touch_test", tags);
    if (child < 0) return child;
    uint8_t pkt[2] = { ROLE_MAGIC, role };
    send((uint32_t)child, pkt, 2);
    return child;
}

static inline void drain_state(void)
{
    Result rd; while (receive_wait(&rd, 50)) { }
    Touch  td; while (touch_pop(&td))         { }
}

static bool collect_until(bool (*heard)(const Result *), uint32_t quiet_ms)
{
    uint64_t last_heard = uptime_ms();
    for (;;) {
        Result r;
        if (receive_wait(&r, quiet_ms)) {
            last_heard = uptime_ms();
            if (heard(&r)) return true;
        } else if (uptime_ms() - last_heard >= quiet_ms) {
            return false;
        }
    }
}


static void role_ping_sender(uint32_t parent_pid)
{
    (void)parent_pid;
    for (volatile int i = 0; i < 80000; i++) {}
    touch_send(TOUCH_TAG_PAIR(TAG_PING), "hello", 5, 0);
    exit(0);
}

static void role_bcast_listen(uint32_t parent_pid)
{
    TouchTag tag = TOUCH_TAG_ID(TAG_BCAST);
    int claim_rc = touch_claim(tag, TOUCH_REST, 0, 0);

    uint8_t ready = (claim_rc == 0) ? 1 : 0;
    send(parent_pid, &ready, 1);

    Touch t;
    int rc = touch_await(tag, &t, 30000);
    uint8_t ok = (rc == 0) ? 1 : 0;
    send(parent_pid, &ok, 1);
    touch_release(tag);
    exit(0);
}

static void role_bcast_send(uint32_t parent_pid)
{
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
    uint8_t denied = (rc < 0) ? 1 : 0;
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

static void role_die_code(uint32_t parent_pid)
{
    (void)parent_pid;
    exit(EXIT_CLEAN_CODE);
}

static void role_loop_forever(uint32_t parent_pid)
{
    uint8_t ready = 1;
    send(parent_pid, &ready, 1);
    for (;;) yield();
}

static void role_die_indexed(uint32_t parent_pid, uint32_t code)
{
    TouchTag burst = TOUCH_TAG_ID(TAG_BURST);
    touch_claim(burst, TOUCH_REST, 0, 0);
    uint8_t ready = 1;
    send(parent_pid, &ready, 1);
    Touch t;
    touch_await(burst, &t, 30000);
    touch_release(burst);
    exit(code);
}

static void role_tag_report(uint32_t parent_pid)
{
    bool has_aug = false, has_file = false;
    proc_tag_check("spawn:aug", &has_aug);
    proc_tag_check("touch_test", &has_file);
    uint8_t msg[3] = { 0xAA, (uint8_t)(has_aug ? 1 : 0), (uint8_t)(has_file ? 1 : 0) };
    send(parent_pid, msg, sizeof(msg));
    exit(0);
}

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

static void test2(void)
{
    drain_state();

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

    uint8_t go = 0xC0;
    send((uint32_t)sender, &go, 1);

    bool ok_c1 = false, ok_c2 = false;
    uint8_t va = 0, vb = 0;
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

static void test4(void)
{
    TouchTag tag = TOUCH_TAG_ID(TAG_PDIED);
    touch_claim(tag, TOUCH_REST, 0, 0);

    int child = spawn_role(ROLE_DIE_CHILD);
    if (child < 0) { fail(4, "spawn failed"); touch_release(tag); return; }

    bool pid_ok = false;
    bool delivered = false;
    for (int tries = 0; tries < 16 && !pid_ok; tries++) {
        Touch t;
        if (touch_await(tag, &t, 3000) != 0) break;
        delivered = true;
        if (t.payload_len >= 4) {
            uint32_t pid = 0;
            memcpy(&pid, t.payload, 4);
            if (pid == (uint32_t)child) pid_ok = true;
        }
    }
    if (pid_ok)         pass(4);
    else if (!delivered) fail(4, "await timed out");
    else                fail(4, "own child death not delivered");
    touch_release(tag);
}

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

static void test7(void)
{
    drain_state();

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

static void test8(void)
{
    {
        Result drain;
        while (receive_wait(&drain, 50)) {  }
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

    int child_has = spawn_role_tagged(ROLE_HAS_TAG_SEND, TAG_OWNERS);
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

static void test10(void)
{
    drain_state();

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

static void test11(void)
{
    drain_state();

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

static bool await_died_code(TouchTag tag, uint32_t pid, int32_t *out_code,
                            bool *out_delivered)
{
    *out_delivered = false;
    for (int tries = 0; tries < 16; tries++) {
        Touch t;
        if (touch_await(tag, &t, 3000) != 0) break;
        *out_delivered = true;
        if (t.payload_len >= sizeof(TouchProcessDied)) {
            TouchProcessDied died;
            memcpy(&died, t.payload, sizeof(died));
            if (died.pid == pid) { *out_code = died.exit_code; return true; }
        }
    }
    return false;
}

static void test12(void)
{
    drain_state();

    TouchTag tag = TOUCH_TAG_ID(TAG_PDIED);
    touch_claim(tag, TOUCH_REST, 0, 0);

    int child = spawn_role(ROLE_DIE_CODE);
    if (child < 0) { fail(12, "spawn failed"); touch_release(tag); return; }

    int32_t code = 0;
    bool delivered = false;
    bool matched = await_died_code(tag, (uint32_t)child, &code, &delivered);

    if (matched && code == EXIT_CLEAN_CODE) pass(12);
    else if (!delivered)  fail(12, "await timed out");
    else if (!matched)    fail(12, "own child death not delivered");
    else                  fail(12, "clean exit_code mismatch");
    touch_release(tag);
}

static void test13(void)
{
    drain_state();

    TouchTag tag = TOUCH_TAG_ID(TAG_PDIED);
    touch_claim(tag, TOUCH_REST, 0, 0);

    int child = spawn_role(ROLE_LOOP_FOREVER);
    if (child < 0) { fail(13, "spawn failed"); touch_release(tag); return; }

    bool ready = false;
    Result r;
    for (int t = 0; t < 30 && !ready; t++) {
        if (receive_wait(&r, 200)) {
            if (r.sender_pid == (uint32_t)child &&
                r.data_length >= 1 && r.data_addr != 0 &&
                *(const uint8_t *)(uintptr_t)r.data_addr == 1) ready = true;
        }
    }
    if (!ready) { fail(13, "loop child not ready"); touch_release(tag); return; }

    uint32_t target = (uint32_t)child;
    int kill_rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_PROC_KILL,
                          &target, (uint16_t)sizeof(target),
                          NULL, 0, NULL, 0, NULL,
                          BOX_ANSWER_GUARANTEED, NULL);
    if (kill_rc != 0) { fail(13, "PROC_KILL failed"); touch_release(tag); return; }

    int32_t code = 0;
    bool delivered = false;
    bool matched = await_died_code(tag, (uint32_t)child, &code, &delivered);

    if (matched && code == PROC_EXIT_KILLED) pass(13);
    else if (!delivered)  fail(13, "await timed out");
    else if (!matched)    fail(13, "killed child death not delivered");
    else                  fail(13, "killed exit_code mismatch");
    touch_release(tag);
}


static uint32_t g_t14_pid[CONCURRENT_CHILDREN];
static bool     g_t14_armed[CONCURRENT_CHILDREN];
static int      g_t14_armed_count;

static bool t14_heard_armed(const Result *r)
{
    if (r->data_length >= 1 && r->data_addr != 0 &&
        *(const uint8_t *)(uintptr_t)r->data_addr == 1) {
        for (int i = 0; i < CONCURRENT_CHILDREN; i++) {
            if (r->sender_pid == g_t14_pid[i] && !g_t14_armed[i]) {
                g_t14_armed[i] = true; g_t14_armed_count++; break;
            }
        }
    }
    return g_t14_armed_count == CONCURRENT_CHILDREN;
}

static void test14(void)
{
    drain_state();

    TouchTag tag = TOUCH_TAG_ID(TAG_PDIED);
    touch_claim(tag, TOUCH_REST, 0, 0);

    int32_t  want[CONCURRENT_CHILDREN];
    int32_t  got[CONCURRENT_CHILDREN];
    bool     seen[CONCURRENT_CHILDREN];

    g_t14_armed_count = 0;
    for (int i = 0; i < CONCURRENT_CHILDREN; i++) {
        want[i]  = CONCURRENT_BASE_CODE + i;
        got[i]   = 0;
        seen[i]  = false;
        g_t14_armed[i] = false;
        int child = spawn_role_code(ROLE_DIE_INDEXED, (uint32_t)want[i]);
        if (child < 0) { fail(14, "spawn shortfall"); touch_release(tag); return; }
        g_t14_pid[i] = (uint32_t)child;
    }

    if (!collect_until(t14_heard_armed, COLLECT_QUIET_MS)) {
        fail(14, "children did not all arm");
        touch_release(tag);
        return;
    }

    if (touch_send(TOUCH_TAG_PAIR(TAG_BURST), "go", 2, 0) < 0) {
        fail(14, "burst broadcast failed");
        touch_release(tag);
        return;
    }

    int remaining = CONCURRENT_CHILDREN;
    bool dup = false;
    uint32_t dup_pid = 0;
    for (int tries = 0; tries < CONCURRENT_CHILDREN * 8 && remaining > 0; tries++) {
        Touch t;
        if (touch_await(tag, &t, 3000) != 0) break;
        if (t.payload_len < sizeof(TouchProcessDied)) continue;
        TouchProcessDied died;
        memcpy(&died, t.payload, sizeof(died));
        for (int i = 0; i < CONCURRENT_CHILDREN; i++) {
            if (died.pid != g_t14_pid[i]) continue;
            if (seen[i]) { dup = true; dup_pid = died.pid; }
            else { seen[i] = true; got[i] = died.exit_code; remaining--; }
            break;
        }
    }

    if (dup) {
        kdbg_print("[TT 14] FAIL: pid %u death published more than once", dup_pid);
        g_total++;
        touch_release(tag);
        return;
    }
    for (int i = 0; i < CONCURRENT_CHILDREN; i++) {
        if (!seen[i]) {
            kdbg_print("[TT 14] FAIL: pid %u death never delivered (want %d)",
                       g_t14_pid[i], (int)want[i]);
            g_total++;
            touch_release(tag);
            return;
        }
        if (got[i] != want[i]) {
            kdbg_print("[TT 14] FAIL: pid %u got code %d want %d (mislabel/swap)",
                       g_t14_pid[i], (int)got[i], (int)want[i]);
            g_total++;
            touch_release(tag);
            return;
        }
    }
    pass(14);
    touch_release(tag);
}

static void test15(void)
{
    drain_state();

    TouchTag tag = TOUCH_TAG_ID(TAG_PDIED);
    touch_claim(tag, TOUCH_REST, 0, 0);

    int child = spawn_role(ROLE_LOOP_FOREVER);
    if (child < 0) { fail(15, "spawn failed"); touch_release(tag); return; }

    bool ready = false;
    Result r;
    for (int t = 0; t < 30 && !ready; t++) {
        if (receive_wait(&r, 200)) {
            if (r.sender_pid == (uint32_t)child &&
                r.data_length >= 1 && r.data_addr != 0 &&
                *(const uint8_t *)(uintptr_t)r.data_addr == 1) ready = true;
        }
    }
    if (!ready) { fail(15, "loop child not ready"); touch_release(tag); return; }

    if (proc_kill((uint32_t)child) != OK) {
        fail(15, "proc_kill returned error");
        touch_release(tag);
        return;
    }

    int32_t code = 0;
    bool delivered = false;
    bool matched = await_died_code(tag, (uint32_t)child, &code, &delivered);
    touch_release(tag);
    if (!matched && !delivered) { fail(15, "await timed out");                return; }
    if (!matched)               { fail(15, "killed child death not delivered"); return; }
    if (code != PROC_EXIT_KILLED) { fail(15, "killed exit_code mismatch");    return; }

    if (proc_kill(0) != -ERR_INVALID_ARGUMENT) {
        fail(15, "proc_kill(0) not rejected"); return;
    }
    if (proc_kill(cabin_info()->pid) != -ERR_INVALID_ARGUMENT) {
        fail(15, "proc_kill(self) not rejected"); return;
    }
    pass(15);
}

static void test16(void)
{
    drain_state();

    int child = spawn_role_tagged(ROLE_TAG_REPORT, "spawn:aug");
    if (child < 0) { fail(16, "tagged spawn failed"); return; }

    bool got = false;
    uint8_t has_aug = 0, has_file = 0;
    Result r;
    for (int t = 0; t < 30 && !got; t++) {
        if (!receive_wait(&r, 200)) continue;
        if (r.sender_pid != (uint32_t)child) continue;
        if (r.data_length < 3 || r.data_addr == 0) continue;
        const uint8_t *b = (const uint8_t *)(uintptr_t)r.data_addr;
        if (b[0] != 0xAA) continue;
        has_aug = b[1]; has_file = b[2]; got = true;
    }
    if (!got)      { fail(16, "tag report not received");           return; }
    if (!has_aug)  { fail(16, "caller augment tag missing on child"); return; }
    if (!has_file) { fail(16, "file name tag missing on child");    return; }

    if (proc_exec_tagged("touch_test", "god") != -ERR_ACCESS_DENIED) {
        fail(16, "reserved 'god' augment not denied"); return;
    }
    if (proc_exec_tagged("touch_test", "system") != -ERR_ACCESS_DENIED) {
        fail(16, "reserved 'system' augment not denied"); return;
    }
    pass(16);
}

static int proc_spawn_raw(const char *tags)
{
    uint8_t params[16];
    uint64_t binary_phys = 0x1000;
    uint64_t binary_size = 64;
    memcpy(params, &binary_phys, sizeof(binary_phys));
    memcpy(params + 8, &binary_size, sizeof(binary_size));
    return MfCall1(DECK_SYSTEM, SYSTEM_OP_PROC_SPAWN,
                   params, (uint16_t)sizeof(params),
                   tags, (uint32_t)strlen(tags),
                   NULL, 0, NULL,
                   BOX_ANSWER_GUARANTEED, NULL);
}

static void test17(void)
{
    drain_state();

    if (proc_spawn_raw("system") != ERR_ACCESS_DENIED) {
        fail(17, "proc.spawn system not denied"); return;
    }

    if (proc_spawn_raw("god") != ERR_ACCESS_DENIED) {
        fail(17, "proc.spawn god not denied"); return;
    }

    if (proc_spawn_raw("utility") == ERR_ACCESS_DENIED) {
        fail(17, "proc.spawn utility over-rejected"); return;
    }
    pass(17);
}

static void test18(void)
{
    if (proc_tag_add("god") != -ERR_ACCESS_DENIED) {
        fail(18, "self-grant 'god' not denied"); return;
    }
    bool has = true;
    if (proc_tag_check("god", &has) != OK) { fail(18, "tag_check failed"); return; }
    if (has) { fail(18, "'god' leaked onto self despite denial"); return; }
    pass(18);
}

static void test19(void)
{
    if (proc_tag_add(TAG_OWNERS) != OK) {
        fail(19, "non-priv self tag rejected"); return;
    }
    proc_tag_remove(TAG_OWNERS);
    pass(19);
}

static void test20(void)
{
    uint32_t parent = cabin_info()->spawner_pid;
    if (parent == 0) { fail(20, "no spawner to test authority against"); return; }

    uint32_t target = parent;
    int kill_rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_PROC_KILL,
                          &target, (uint16_t)sizeof(target),
                          NULL, 0, NULL, 0, NULL,
                          BOX_ANSWER_GUARANTEED, NULL);
    if (kill_rc != ERR_ACCESS_DENIED) { fail(20, "kill of foreign parent not denied"); return; }
    pass(20);
}

static void test21(void)
{
    uint32_t parent = cabin_info()->spawner_pid;
    if (parent == 0) { fail(21, "no spawner to test authority against"); return; }

    const char *tag = "stopped";
    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_TAG_ADD,
                     &parent, (uint16_t)sizeof(parent),
                     tag, (uint32_t)strlen(tag),
                     NULL, 0, NULL,
                     BOX_ANSWER_GUARANTEED, NULL);
    if (rc != ERR_ACCESS_DENIED) { fail(21, "tag-add on foreign parent not denied"); return; }
    pass(21);
}


#define OWED_TAG     "test:owed"
#define OWED_EVENTS  96u

static TouchTagPair    g_owed_pair;
static volatile uint32_t g_owed_claimed;
static volatile uint32_t g_owed_release;
static volatile uint32_t g_owed_got;
static volatile uint32_t g_owed_disorder;
static volatile uint32_t g_owed_done;

static void owed_strand(void *arg)
{
    (void)arg;
    if (touch_claim(g_owed_pair.full, TOUCH_REST, 0, 0) != 0) {
        __atomic_store_n(&g_owed_done, 1, __ATOMIC_RELEASE);
        return;
    }
    __atomic_store_n(&g_owed_claimed, 1, __ATOMIC_RELEASE);

    while (__atomic_load_n(&g_owed_release, __ATOMIC_ACQUIRE) == 0)
        yield();

    uint32_t expect = 0;
    uint64_t deadline = uptime_ms() + 15000;
    for (;;) {
        Touch t;
        if (!touch_wait(&t, 500)) {
            if (uptime_ms() >= deadline) break;
            continue;
        }
        if (t.tag_id != g_owed_pair.full) continue;
        uint32_t seq = 0;
        if (t.payload_len >= sizeof(uint32_t))
            memcpy(&seq, t.payload, sizeof(uint32_t));
        if (seq != expect) __atomic_add_fetch(&g_owed_disorder, 1, __ATOMIC_RELAXED);
        expect++;
        if (__atomic_add_fetch(&g_owed_got, 1, __ATOMIC_ACQ_REL) >= OWED_EVENTS)
            break;
    }
    __atomic_store_n(&g_owed_done, 1, __ATOMIC_RELEASE);
}

static void test22(void)
{
    g_owed_claimed = g_owed_release = g_owed_got = g_owed_disorder = g_owed_done = 0;
    g_owed_pair = touch_intern(OWED_TAG);
    if (g_owed_pair.full == TOUCH_TAG_INVALID) { fail(22, "tag intern failed"); return; }

    if (strand_spawn(owed_strand, NULL) == 0) { fail(22, "strand_spawn failed"); return; }

    uint64_t t0 = uptime_ms();
    while (__atomic_load_n(&g_owed_claimed, __ATOMIC_ACQUIRE) == 0) {
        if (uptime_ms() - t0 > 5000) { fail(22, "strand never claimed the tag"); return; }
        yield();
    }

    uint32_t sent = 0;
    for (uint32_t i = 0; i < OWED_EVENTS; i++) {
        if (touch_send(g_owed_pair, &i, sizeof(i), 0) < 0) break;
        sent++;
    }
    __atomic_store_n(&g_owed_release, 1, __ATOMIC_RELEASE);

    if (sent != OWED_EVENTS) { fail(22, "publisher could not send the burst"); return; }

    t0 = uptime_ms();
    while (__atomic_load_n(&g_owed_done, __ATOMIC_ACQUIRE) == 0) {
        if (uptime_ms() - t0 > 20000) break;
        yield();
    }

    uint32_t got = __atomic_load_n(&g_owed_got,      __ATOMIC_ACQUIRE);
    uint32_t dis = __atomic_load_n(&g_owed_disorder, __ATOMIC_ACQUIRE);
    kdbg_print("[TT 22] ring=64 sent=%u received=%u out-of-order=%u",
               (unsigned)OWED_EVENTS, (unsigned)got, (unsigned)dis);
    if (got == OWED_EVENTS && dis == 0) pass(22);
    else                                fail(22, "a full ring lost or reordered events");
}

#define KEPT_TAG_A   "test:kept.a"
#define KEPT_TAG_B   "test:kept.b"
#define KEPT_EVENTS  300u

static TouchTagPair      g_kept_a, g_kept_b;
static volatile uint32_t g_kept_claimed;
static volatile uint32_t g_kept_release;
static volatile uint32_t g_kept_got;
static volatile uint32_t g_kept_waited;
static volatile uint32_t g_kept_done;
static volatile uint32_t g_kept_pid;

static void kept_touch_strand(void *arg)
{
    (void)arg;
    if (touch_claim(g_kept_a.full, TOUCH_REST, 0, 0) != 0 ||
        touch_claim(g_kept_b.full, TOUCH_REST, 0, 0) != 0) {
        __atomic_store_n(&g_kept_done, 1, __ATOMIC_RELEASE);
        return;
    }
    __atomic_store_n(&g_kept_claimed, 1, __ATOMIC_RELEASE);

    Touch t;
    if (touch_wait_tag(g_kept_a.full, &t, 0))
        __atomic_store_n(&g_kept_waited, 1, __ATOMIC_RELEASE);

    uint32_t got = 0;
    while (touch_try_pop_tag(g_kept_b.full, &t)) got++;
    __atomic_store_n(&g_kept_got, got, __ATOMIC_RELEASE);
    __atomic_store_n(&g_kept_done, 1, __ATOMIC_RELEASE);
}

static void test23(void)
{
    g_kept_claimed = g_kept_release = g_kept_got = g_kept_waited = g_kept_done = 0;
    g_kept_a = touch_intern(KEPT_TAG_A);
    g_kept_b = touch_intern(KEPT_TAG_B);
    if (g_kept_a.full == TOUCH_TAG_INVALID || g_kept_b.full == TOUCH_TAG_INVALID) {
        fail(23, "tag intern failed"); return;
    }
    if (strand_spawn(kept_touch_strand, NULL) == 0) { fail(23, "strand_spawn failed"); return; }

    uint64_t t0 = uptime_ms();
    while (__atomic_load_n(&g_kept_claimed, __ATOMIC_ACQUIRE) == 0) {
        if (uptime_ms() - t0 > 5000) { fail(23, "strand never claimed its tags"); return; }
        yield();
    }

    uint32_t sent = 0;
    for (uint32_t i = 0; i < KEPT_EVENTS; i++) {
        if (touch_send(g_kept_b, &i, sizeof(i), 0) < 0) break;
        sent++;
    }
    uint32_t one = 0;
    if (sent == KEPT_EVENTS && touch_send(g_kept_a, &one, sizeof(one), 0) < 0) sent = 0;
    if (sent != KEPT_EVENTS) { fail(23, "publisher could not send the burst"); return; }

    t0 = uptime_ms();
    while (__atomic_load_n(&g_kept_done, __ATOMIC_ACQUIRE) == 0) {
        if (uptime_ms() - t0 > 20000) break;
        yield();
    }
    uint32_t got = __atomic_load_n(&g_kept_got, __ATOMIC_ACQUIRE);
    kdbg_print("[TT 23] other-tag events published=%u kept=%u waited=%u",
               (unsigned)KEPT_EVENTS, (unsigned)got,
               (unsigned)__atomic_load_n(&g_kept_waited, __ATOMIC_ACQUIRE));
    if (__atomic_load_n(&g_kept_waited, __ATOMIC_ACQUIRE) == 0)
        fail(23, "the wait for the first tag did not come back");
    else if (got == KEPT_EVENTS)
        pass(23);
    else
        fail(23, "events of the other tag were lost while waiting");
}

static void kept_ipc_strand(void *arg)
{
    (void)arg;
    if (touch_claim(g_kept_a.full, TOUCH_REST, 0, 0) != 0) {
        __atomic_store_n(&g_kept_done, 1, __ATOMIC_RELEASE);
        return;
    }
    __atomic_store_n(&g_kept_pid, strand_self(), __ATOMIC_RELEASE);
    __atomic_store_n(&g_kept_claimed, 1, __ATOMIC_RELEASE);

    Touch t;
    if (touch_wait_tag(g_kept_a.full, &t, 0))
        __atomic_store_n(&g_kept_waited, 1, __ATOMIC_RELEASE);
    kdbg_print("[TT 24] strand %u: reply wanted behind %u messages",
               (unsigned)strand_self(), (unsigned)KEPT_EVENTS);

    uint32_t got = 0;
    Result   r;
    while (receive(&r)) got++;
    __atomic_store_n(&g_kept_got, got, __ATOMIC_RELEASE);
    __atomic_store_n(&g_kept_done, 1, __ATOMIC_RELEASE);
}

static void test24(void)
{
    g_kept_claimed = g_kept_release = g_kept_got = g_kept_waited = g_kept_done = g_kept_pid = 0;
    g_kept_a = touch_intern(KEPT_TAG_A);
    if (g_kept_a.full == TOUCH_TAG_INVALID) { fail(24, "tag intern failed"); return; }
    if (strand_spawn(kept_ipc_strand, NULL) == 0) { fail(24, "strand_spawn failed"); return; }

    uint64_t t0 = uptime_ms();
    while (__atomic_load_n(&g_kept_claimed, __ATOMIC_ACQUIRE) == 0) {
        if (uptime_ms() - t0 > 5000) { fail(24, "strand never claimed its tag"); return; }
        yield();
    }
    uint32_t pid = __atomic_load_n(&g_kept_pid, __ATOMIC_ACQUIRE);

    uint32_t sent = 0;
    for (uint32_t i = 0; i < KEPT_EVENTS; i++) {
        if (send(pid, &i, sizeof(i)) != 0) break;
        sent++;
    }
    uint32_t one = 0;
    if (sent == KEPT_EVENTS && touch_send(g_kept_a, &one, sizeof(one), 0) < 0) sent = 0;
    if (sent != KEPT_EVENTS) { fail(24, "sender could not send the burst"); return; }

    t0 = uptime_ms();
    while (__atomic_load_n(&g_kept_done, __ATOMIC_ACQUIRE) == 0) {
        if (uptime_ms() - t0 > 20000) break;
        yield();
    }
    uint32_t got = __atomic_load_n(&g_kept_got, __ATOMIC_ACQUIRE);
    kdbg_print("[TT 24] messages sent=%u kept=%u", (unsigned)KEPT_EVENTS, (unsigned)got);
    if (got == KEPT_EVENTS) pass(24);
    else                    fail(24, "messages ahead of a kernel reply were lost");
}

int main(void)
{
    CabinInfo *ci = cabin_info();

    {
        Result r;
        bool found_role = false;
        uint8_t role = 0;
        uint32_t role_code = 0;

        for (int attempt = 0; attempt < 2 && !found_role; attempt++) {
            if (!receive_wait(&r, 200)) break;
            if (r.data_length >= 2 && r.data_addr != 0) {
                const uint8_t *buf = (const uint8_t *)(uintptr_t)r.data_addr;
                if (buf[0] == ROLE_MAGIC) {
                    role = buf[1];
                    if (r.data_length >= 6)
                        memcpy(&role_code, buf + 2, sizeof(role_code));
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
            case ROLE_DIE_CODE:      role_die_code(ci->spawner_pid);      break;
            case ROLE_LOOP_FOREVER:  role_loop_forever(ci->spawner_pid);  break;
            case ROLE_DIE_INDEXED:   role_die_indexed(ci->spawner_pid, role_code); break;
            case ROLE_TAG_REPORT:    role_tag_report(ci->spawner_pid);    break;
            default: break;
            }
            exit(0);
        }
    }

    kdbg_print("[TT] Touch Integration Tests starting");

    drain_state();

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
    test12();
    test13();
    test14();
    test15();
    test16();
    test17();
    test18();
    test19();
    test20();
    test21();
    test22();
    test23();
    test24();

    kdbg_print("[TT SUMMARY] %d/%d passed", g_passed, g_total);
    return 0;
}