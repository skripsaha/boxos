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
#include "box/core/manifest.h"   /* MfCall1 — raw kill-other for the killed case */
#include "boxos_decks.h"         /* DECK_SYSTEM, SYSTEM_OP_PROC_KILL */
#include "box/timeouts.h"        /* BOX_TIMEOUT_IPC_MS */
#include "proc_exit.h"           /* PROC_EXIT_KILLED — shared exit disposition */

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

/* Clean self-exit code asserted by test12 — arbitrary non-zero, sign bit clear
 * so it stays a valid >= 0 disposition that never collides with -1/-2. */
#define EXIT_CLEAN_CODE     0x42

/* TT 14 concurrent race detector — N children, each exits with a DISTINCT code
 * CONCURRENT_BASE_CODE + i. Every code is a small positive int (sign bit clear),
 * so it survives SysProcKill's [0, INT32_MAX] disposition mask byte-for-byte and
 * can never be confused with a negative sentinel (-1 killed / -2 crashed). */
#define CONCURRENT_CHILDREN   8
#define CONCURRENT_BASE_CODE  0x100

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

/* Like spawn_role but appends a 4-byte exit code to the role packet — the
 * ROLE_DIE_INDEXED child reads it back and exits with exactly that value. */
static int spawn_role_code(uint8_t role, uint32_t code)
{
    int child = proc_exec("touch_test");
    if (child < 0) return child;
    uint8_t pkt[6] = { ROLE_MAGIC, role };
    memcpy(pkt + 2, &code, sizeof(code));
    send((uint32_t)child, pkt, sizeof(pkt));
    return child;
}

/* Like spawn_role but launches the child through proc_exec_tagged, so the kernel
 * folds `tags` (a caller augment) into the child's tag set on top of the file's
 * own tags — the union path T16 verifies. */
static int spawn_role_tagged(uint8_t role, const char *tags)
{
    int child = proc_exec_tagged("touch_test", tags);
    if (child < 0) return child;
    uint8_t pkt[2] = { ROLE_MAGIC, role };
    send((uint32_t)child, pkt, 2);
    return child;
}

/*
 * drain_state — wipe BOTH ResultRing and TouchRing before a test runs.
 *
 * Each touch_test sub-test relies on a deterministic empty-ring baseline:
 *   - ResultRing leftovers from a previous test's await-timeout or IPC
 *     could be popped by a subsequent receive_wait and skew payload
 *     comparisons.
 *   - TouchRing leftovers are even nastier — touch_pop is FIFO and does
 *     not filter by tag, so a stale Touch from TT N-1 (e.g. TT 10's
 *     wildcard catching multiple paired publishes) will satisfy TT N's
 *     await with the wrong payload, surfacing as "TT N FAIL: ...".
 *     This was the root cause of the historical TT 11 LATCHED flake
 *     (~1-3 % per matrix run before this drain landed).
 *
 * The drain is cheap (each pop returns false immediately on an empty
 * ring), so it's safe to apply to every test entry — defense in depth
 * for any future TT that doesn't yet exist.
 */
static inline void drain_state(void)
{
    Result rd; while (receive_wait(&rd, 50)) { }
    Touch  td; while (touch_pop(&td))         { }
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

/* Clean exit carrying a known non-zero code — test12 reads it back off
 * process:died. exit() routes EXIT_CLEAN_CODE through SysProcKill's self-exit
 * disposition. */
static void role_die_code(uint32_t parent_pid)
{
    (void)parent_pid;
    exit(EXIT_CLEAN_CODE);
}

/* Alive-and-looping victim for the killed case. It signals readiness, then
 * yields forever and NEVER self-exits, so the only process:died for its pid is
 * the parent's PROC_KILL (PROC_EXIT_KILLED) — deterministic. yield() each turn
 * keeps a cooperative single core handing the CPU back to the parent. */
static void role_loop_forever(uint32_t parent_pid)
{
    uint8_t ready = 1;
    send(parent_pid, &ready, 1);
    for (;;) yield();
}

/* TT 14 child: subscribe to the burst tag, tell the parent we're armed, then
 * block until the parent's single broadcast releases EVERY child at once. The
 * synchronized release makes all N exits hit the cross-core reaper in one burst
 * (max contention on the touch_cleaned claim) while every child is still alive
 * holding a DISTINCT pid — pid recycling (the allocator hands back the lowest
 * free index immediately) can't fold two children onto one pid. Then exit with
 * the per-child code handed in at spawn. */
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

/* T16 child: report which of its two expected tags landed. "spawn:aug" is the
 * caller augment proc_exec_tagged folded in; "touch_test" is the file's own
 * name-stem tag. Both present proves child = file-tags ∪ caller-tags. */
static void role_tag_report(uint32_t parent_pid)
{
    bool has_aug = false, has_file = false;
    proc_tag_check("spawn:aug", &has_aug);
    proc_tag_check("touch_test", &has_file);
    uint8_t msg[3] = { 0xAA, (uint8_t)(has_aug ? 1 : 0), (uint8_t)(has_file ? 1 : 0) };
    send(parent_pid, msg, sizeof(msg));
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

    /* process:died is a BROADCAST stream — every subscriber sees every death.
     * Under SMP a sibling test's child can die first and land in our ring ahead
     * of ours, so we must FILTER by our own child's pid, not assume the first
     * death is ours. (On 1c cooperative ordering hid this; on 16c it surfaced
     * as the intermittent "wrong pid in payload".) A real consumer of a death
     * broadcast filters the same way. Bounded by attempts + per-wait timeout. */
    bool pid_ok = false;
    bool delivered = false;
    for (int tries = 0; tries < 16 && !pid_ok; tries++) {
        Touch t;
        if (touch_await(tag, &t, 3000) != 0) break;   /* no further death in budget */
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
    drain_state();

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

/* await_died_code — claim must already be active; spin (filtering by `pid`, a
 * broadcast stream) for that pid's process:died and hand back its exit_code.
 * Returns true on a matching death, false on timeout-with-no-match. */
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

/* ---------- T12: clean exit carries its code ---------- */
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

/* ---------- T13: kill-other reports PROC_EXIT_KILLED ---------- */
static void test13(void)
{
    drain_state();

    TouchTag tag = TOUCH_TAG_ID(TAG_PDIED);
    touch_claim(tag, TOUCH_REST, 0, 0);

    int child = spawn_role(ROLE_LOOP_FOREVER);
    if (child < 0) { fail(13, "spawn failed"); touch_release(tag); return; }

    /* Wait for the victim to confirm it is alive and looping, so the kill lands
     * on a running process rather than racing its startup. */
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

    /* T15 covers the boxlib proc_kill() helper; here we deliberately keep the raw
     * Manifest wire — SYSTEM_OP_PROC_KILL with a non-zero target pid — so the
     * kernel kill-other path is proven independent of any userspace wrapper. The
     * kernel forces PROC_EXIT_KILLED for a kill-other regardless of any param code. */
    uint32_t target = (uint32_t)child;
    int kill_rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_PROC_KILL,
                          &target, (uint16_t)sizeof(target),
                          NULL, 0, NULL, 0, NULL,
                          BOX_TIMEOUT_IPC_MS, NULL);
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

/* PROC_EXIT_CRASHED (-2) has no dedicated positive test. It is published only
 * when process_destroy is the FIRST cleanup of a strand — a genuine fault or
 * kernel-forced teardown (the kill and self-exit paths clean touch first and win
 * the disposition). The only userspace trigger is a child faulting on purpose,
 * which routes through the IDT handler and spews a full [EXCEPTION] register dump
 * into every boot log — noise that mimics a real crash. T13 already proves a
 * NEGATIVE sentinel survives the compute -> TouchPublish snapshot -> ring ->
 * payload round-trip, and T14 below proves -2 never appears as a MISLABEL of a
 * clean exit under the cross-core reaper race. So -2 is covered, not faked. */

/* ---------- T14: concurrent exit-code integrity under the reaper race --------
 *
 * Proves the SMP fix: a clean exit must publish its TRUE code even when the
 * self-exit path (SysProcKill, one K-Core) and the reaper (process_destroy,
 * another K-Core) reach TouchCleanupProcess for the same proc at once. Before
 * the fix the loser could double-publish or stamp PROC_EXIT_CRASHED (-2) over a
 * clean code. The window only opens under real cross-core parallelism, so this
 * is a no-op-correct pass on 1c and a genuine detector on bios16/uefi16.
 *
 * N children each carry a UNIQUE code (CONCURRENT_BASE_CODE + i). process:died
 * is claimed BEFORE the first spawn (a claim that postdates a death misses it);
 * all N are spawned and armed on a burst tag, then a single broadcast releases
 * them together so the deaths hit the reaper as one simultaneous burst. Every
 * delivered death is matched by pid against the expected set: a mislabel (-2), a
 * code swapped between children, a duplicate, or a missing death FAILs with the
 * offending pid named. Bounded by a per-death watchdog — a death that never
 * arrives FAILs rather than hangs. */
static void test14(void)
{
    drain_state();

    TouchTag tag = TOUCH_TAG_ID(TAG_PDIED);
    touch_claim(tag, TOUCH_REST, 0, 0);

    uint32_t pid[CONCURRENT_CHILDREN];
    int32_t  want[CONCURRENT_CHILDREN];
    int32_t  got[CONCURRENT_CHILDREN];
    bool     armed[CONCURRENT_CHILDREN];
    bool     seen[CONCURRENT_CHILDREN];

    for (int i = 0; i < CONCURRENT_CHILDREN; i++) {
        want[i]  = CONCURRENT_BASE_CODE + i;
        got[i]   = 0;
        armed[i] = false;
        seen[i]  = false;
        int child = spawn_role_code(ROLE_DIE_INDEXED, (uint32_t)want[i]);
        if (child < 0) { fail(14, "spawn shortfall"); touch_release(tag); return; }
        pid[i] = (uint32_t)child;
    }

    /* Barrier: wait until every child has claimed the burst tag and reported
     * armed. Only then is the broadcast guaranteed to reach all N, and all N
     * pids are simultaneously live (hence distinct). */
    int armed_count = 0;
    Result r;
    for (int t = 0; t < 80 && armed_count < CONCURRENT_CHILDREN; t++) {
        if (!receive_wait(&r, 200)) continue;
        if (r.data_length < 1 || r.data_addr == 0) continue;
        if (*(const uint8_t *)(uintptr_t)r.data_addr != 1) continue;
        for (int i = 0; i < CONCURRENT_CHILDREN; i++) {
            if (r.sender_pid == pid[i] && !armed[i]) {
                armed[i] = true; armed_count++; break;
            }
        }
    }
    if (armed_count != CONCURRENT_CHILDREN) {
        fail(14, "children did not all arm");
        touch_release(tag);
        return;
    }

    /* Release every child with one broadcast — a synchronized burst of exits. */
    if (touch_send(TOUCH_TAG_PAIR(TAG_BURST), "go", 2, 0) < 0) {
        fail(14, "burst broadcast failed");
        touch_release(tag);
        return;
    }

    /* Collect. Each death is matched by pid into our set; deaths for pids we
     * don't own are ignored. The loop ends when all N are in (success) or a
     * touch_await times out (a death never came — FAIL, not hang). Stopping the
     * instant remaining hits 0 means a later recycle of a child's pid can't be
     * mistaken for a duplicate. The iteration cap is a hard backstop. */
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
            if (died.pid != pid[i]) continue;
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
                       pid[i], (int)want[i]);
            g_total++;
            touch_release(tag);
            return;
        }
        if (got[i] != want[i]) {
            kdbg_print("[TT 14] FAIL: pid %u got code %d want %d (mislabel/swap)",
                       pid[i], (int)got[i], (int)want[i]);
            g_total++;
            touch_release(tag);
            return;
        }
    }
    pass(14);
    touch_release(tag);
}

/* ---------- T15: boxlib proc_kill() kills by pid; death carries KILLED ---------- */
static void test15(void)
{
    drain_state();

    TouchTag tag = TOUCH_TAG_ID(TAG_PDIED);
    touch_claim(tag, TOUCH_REST, 0, 0);

    int child = spawn_role(ROLE_LOOP_FOREVER);
    if (child < 0) { fail(15, "spawn failed"); touch_release(tag); return; }

    /* Wait for the victim to confirm it is alive and looping so proc_kill lands
     * on a running process rather than racing its startup. */
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

    /* The path under test: the boxlib proc_kill() wrapper, kill-other by pid. */
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

    /* Locally deterministic guards (no child needed): proc_kill must refuse
     * self-targeting. pid 0 is the kernel's self-exit form and our own pid is
     * exit()'s job — both come back ERR_INVALID_ARGUMENT and leave us running. */
    if (proc_kill(0) != -ERR_INVALID_ARGUMENT) {
        fail(15, "proc_kill(0) not rejected"); return;
    }
    if (proc_kill(cabin_info()->pid) != -ERR_INVALID_ARGUMENT) {
        fail(15, "proc_kill(self) not rejected"); return;
    }
    /* Still executing here — self never died. */
    pass(15);
}

/* ---------- T16: child = file ∪ caller tags; reserved augment is denied ------- */
static void test16(void)
{
    drain_state();

    /* Union proof: "spawn" is neither reserved nor a tag the touch_test file
     * carries, so a child reporting "spawn:aug" present alongside its own
     * "touch_test" name tag proves the caller augment crossed PROC_EXEC. */
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
        if (b[0] != 0xAA) continue;               /* skip the 0xFE exit sentinel */
        has_aug = b[1]; has_file = b[2]; got = true;
    }
    if (!got)      { fail(16, "tag report not received");           return; }
    if (!has_aug)  { fail(16, "caller augment tag missing on child"); return; }
    if (!has_file) { fail(16, "file name tag missing on child");    return; }

    /* Reserved-key augment must be refused wholesale — the child is never
     * created, so no privilege can leak in. god + system both gate. */
    if (proc_exec_tagged("touch_test", "god") != -ERR_ACCESS_DENIED) {
        fail(16, "reserved 'god' augment not denied"); return;
    }
    if (proc_exec_tagged("touch_test", "system") != -ERR_ACCESS_DENIED) {
        fail(16, "reserved 'system' augment not denied"); return;
    }
    pass(16);
}

/* Raw proc.spawn wire — no boxlib wrapper exists for it (spawn stays
 * kernel-internal), so issue the Manifest call directly like T13's raw kill.
 * params = [u64 binary_phys][u64 binary_size]; binary_phys = 0x1000 is page-
 * aligned, non-zero and < 4GiB, so it clears SysProcSpawn's early checks while
 * never being dereferenced on a gated reject (the gate fires pre-binary).
 *
 * Sign note: this is a RAW MfCall1, so a kernel-side error comes back as a
 * POSITIVE error_t (box/error.h: positive = kernel error, negative = transport).
 * That is why the comparisons below use +ERR_ACCESS_DENIED, unlike the
 * box_fail-wrapped boxlib stubs (T16) which return the negated -ERR_*. */
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
                   BOX_TIMEOUT_IPC_MS, NULL);
}

/* ---------- T17: proc.spawn child auth-level subset of spawner; no escalation -
 *
 * touch_test runs as "app,utility,test" (has utility, lacks system). The
 * proc.spawn tag gate must let it grant a utility child but deny a system
 * child — otherwise a utility process could mint a system-privileged child
 * (utility->system escalation; a phys address for the forged ELF is leaked via
 * the app-level MEMTAG_INFO base_phys field). The gate runs BEFORE
 * process_create and before the binary is read, so the reject branch is
 * deterministic and dereferences nothing.
 *
 * We assert on "system", not "god": a well-known tag confers its auth bit only
 * when its registry id is < 64, and god/bypass/network are used by no file in
 * the image, so they intern past id 63 and carry a zero auth bit — they are
 * unrepresentable and therefore harmless (a spawned child cannot gain the
 * privilege either, by the same 64-bit mask). "system" is used by system files,
 * so it is representable and is the real, grantable escalation the gate must
 * stop. (The well-known id>=64 zero-bit defect is a pre-existing TagFS bug,
 * tracked as its own fix; once it lands a god assertion can be added here.) */
static void test17(void)
{
    drain_state();

    /* REJECT (pre-binary, deterministic): a utility spawner cannot grant the
     * system privilege it does not itself hold — denied at the SYSTEM auth
     * level the caller cannot reach. */
    if (proc_spawn_raw("system") != ERR_ACCESS_DENIED) {
        fail(17, "proc.spawn system not denied"); return;
    }

    /* ALLOW (not over-rejected): utility is within the spawner's own reach, so
     * the gate passes and the op proceeds to fail on the dummy ELF blob
     * (INVALID_ELF / SPAWN_FAILED). Anything but ACCESS_DENIED proves the gate
     * let it through regardless of what 0x1000 happens to contain. */
    if (proc_spawn_raw("utility") == ERR_ACCESS_DENIED) {
        fail(17, "proc.spawn utility over-rejected"); return;
    }
    pass(17);
}

/* ---------- main ---------- */
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

    kdbg_print("[TT SUMMARY] %d/%d passed", g_passed, g_total);
    return 0;
}
