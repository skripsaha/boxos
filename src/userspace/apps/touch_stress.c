#include "box/touch.h"
#include "box/debug.h"
#include "box/ipc.h"
#include "box/system.h"
#include "box/time.h"
#include "box/luggage.h"
#include "box/core/cabin.h"
#include "box/core/result.h"
#include "box/string.h"
#include "box/error.h"

#define TAG_FLOOD   "stress:flood"
#define TAG_KV      "stress:kv"

/* A child's role is its LUGGAGE — the line it was started with, in its cabin
 * before its first instruction — not a packet it has to wait for, and never a
 * guess made from who spawned it.
 *
 * This used to be two questions in a row, both wrong. First "is my spawner pid
 * 2?", the shell's pid written as a literal: the shell is a program like any
 * other and its seat is whatever the boot happened to hand it, so a shell that
 * ever starts elsewhere turns the parent run into a child run that reads a role
 * nobody sent. Then a ROLE_MAGIC packet the child waited 150 turns of a 200 ms
 * receive for, after which it exited quietly WITHOUT running its role at all —
 * which the parent could only read as "the listener reported nothing", a FAIL
 * whose real cause was the child's own clock.
 *
 * Word 1 is there or this program is the parent, and there is nothing in
 * between. The shell and the matrix start `touch_stress` with no second word;
 * only this file adds one. */
#define LINE_CHILD_KV     "touch_stress kv"
#define LINE_CHILD_FLOOD  "touch_stress flood"
#define ROLE_WORD_KV      "kv"
#define ROLE_WORD_FLOOD   "flood"

/* Spawn one child on `line` and report its INCARNATION — (pid, generation) — so
 * the parent can later wait for that exact child's end as a state. A pid alone
 * names a seat, and seats are re-let. */
static int spawn_role(const char *line, uint32_t *out_gen)
{
    return proc_exec_gen(line, NULL, out_gen);
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

/* -----------------------------------------------------------------------
 * S1 — Producer-consumer: 4 producers × 1000 sends each → parent counts
 * ----------------------------------------------------------------------- */

#define KV_ITERS     1000
#define KV_PRODUCERS 4

/* How long the tag must say NOTHING before the parent stops draining and goes
 * to ask the producers' records. NOT a verdict — the records are — so its only
 * job is to tell a gap between two of a producer's sends from the end of the
 * sending. A producer's loop has nothing blocking in it, so consecutive events
 * on this tag are one syscall apart; a whole second is a thousand times the
 * worst a syscall costs even on a contended TCG host, so a second of silence is
 * the end of production and not a pause in it. Being early costs nothing
 * either: the parent's TouchRing carries thousands of slots — more than
 * KV_PRODUCERS × KV_ITERS all told — so the events of a producer still running
 * wait there while the parent is parked on a record, and are counted after. */
#define S1_QUIET_MS  1000u

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

    int      children[KV_PRODUCERS];
    uint32_t gens[KV_PRODUCERS];
    for (int i = 0; i < KV_PRODUCERS; i++) {
        children[i] = spawn_role(LINE_CHILD_KV, &gens[i]);
        if (children[i] <= 0) {
            kdbg_print("[STRESS S1] FAIL: spawn failed i=%d", i);
            touch_release(tag);
            return;
        }
    }

    /* Drain while they produce — the overlap IS what S1 is named for. What ends
     * the collection is a FACT and not a deadline: a producer publishes its
     * KV_ITERS and then EXITS, and an event already published cannot evaporate
     * when its sender dies. So a stretch of quiet on the tag is not a verdict,
     * it is the moment to go and ask the producers' own records; process_gone
     * answers at once for one that has finished and parks on one still running,
     * so there is no window between asking and being registered. Once all four
     * have ended, the next stretch of quiet is the end of the story.
     *
     * The bare 30 s touch_await this replaces was that deadline standing in for
     * the fact: when it expired it ended the count and reported a working system
     * short, with nothing said about whether anyone was still producing. */
    uint32_t count     = 0;
    uint32_t expected  = KV_PRODUCERS * KV_ITERS;
    bool     ended     = false;
    uint64_t last_kv   = uptime_ms();

    while (count < expected) {
        Touch t;
        if (touch_await(tag, &t, S1_QUIET_MS) == 0) {
            count++; last_kv = uptime_ms(); continue;
        }
        /* touch_await measures its own deadline off the TSC (boxlib touch.c);
         * the clock this test keeps is the ClockBoard's, so an empty-handed
         * return is a reason to look at it, never silence by itself. */
        if (uptime_ms() - last_kv < S1_QUIET_MS) continue;
        if (ended) break;                    /* quiet, and no producer left to speak */
        for (int i = 0; i < KV_PRODUCERS; i++)
            (void)process_gone((uint32_t)children[i], gens[i], NULL);
        ended   = true;
        last_kv = uptime_ms();
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

    /* Dynamic tag names — must intern each iteration (no caching). */
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

/* -----------------------------------------------------------------------
 * S3 — Multi-listener flood: 4 listeners, 1 sender × 500 sends
 * ----------------------------------------------------------------------- */

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
        children[i] = spawn_role(LINE_CHILD_FLOOD, &gens[i]);
        if (children[i] <= 0) {
            kdbg_print("[STRESS S3] FAIL: spawn failed i=%d", i);
            return;
        }
    }

    /* Every listener sends `ready` the instant its claim returns, so four
     * reports are on their way from four children we have just been handed the
     * pids of. The wait for them SLEEPS and has no budget to run out: 60 turns
     * of a 200 ms receive was a twelve-second guess that, when it expired,
     * reported listeners as missing and skipped the flood entirely. The loop's
     * only exit is the fourth report; a child that faults before reporting
     * leaves this parent parked, which is a wedge Nightwatch names on facts
     * rather than a red run on a busy host. */
    Result r;
    int ready_count = 0;
    bool ready[FLOOD_LISTENERS] = { false };
    while (ready_count < FLOOD_LISTENERS) {
        (void)receive_wait(&r, 0);   /* no deadline: returns only WITH a message */
        for (int i = 0; i < FLOOD_LISTENERS; i++) {
            if (!ready[i] && r.sender_pid == (uint32_t)children[i]) {
                ready[i] = true; ready_count++; break;
            }
        }
    }

    /* Nothing stands between the barrier and the flood. Half a million volatile
     * turns used to, as an unnamed grace period for the listeners to "settle" —
     * but a listener sends its `ready` only after touch_claim has returned, and
     * a returned claim means the kernel has already linked that cabin into the
     * tag's bucket, so every publish from here lands whether the listener has
     * reached touch_await or not. The spin was waiting for something that had
     * already happened, and it was also the one thing in this test that made the
     * flood LESS adversarial: the race worth running is the one where the sends
     * start the instant the last claim is in. */
    TouchTagPair pair = TOUCH_TAG_PAIR(TAG_FLOOD);
    for (int i = 0; i < FLOOD_SENDS; i++) {
        touch_send(pair, &i, sizeof(i), 0);
    }

    /* A listener sends its count and then EXITS, so the count is not something
     * to wait a window for — it is already in this ring by the time the child
     * is gone, and a pocket already pushed cannot evaporate when its sender
     * dies. So: wait for each child's END as a state (unbounded and correct —
     * every listener leaves its await loop on its own budget and exits), then
     * take what they left. The 100 turns of a 200 ms receive this replaces was
     * a twenty-second guess that reported zeros as lost frames. */
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

    /* Word 1 of the luggage names the role, and it is already there. A word we
     * do not know is a caller's mistake — say so and end, rather than run the
     * parent suite a second time inside a child. */
    const char *role = luggage_word(1);
    if (role) {
        if (strcmp(role, ROLE_WORD_KV)    == 0) role_kv_producer(ci->spawner_pid);
        if (strcmp(role, ROLE_WORD_FLOOD) == 0) role_flood_listener(ci->spawner_pid);
        kdbg_print("[STRESS C-?] unknown role '%s'", role);
        exit(255);
    }

    kdbg_print("[STRESS] Touch Stress Tests starting");

    test_s1();
    test_s2();
    test_s3();

    kdbg_print("[STRESS] Done");
    return 0;
}
